#include "gbc_save.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "share/game_save.h"
#include "share/emu_log_cpp.h"
#include "cardputer/LGFX_ILI9341.h"
#include "gbc_display.h"

// ==== API ====
extern "C" {
  bool gnuboy_sram_dirty(void);
  uint32_t gnuboy_sram_write_gen(void);
  int  gnuboy_sram_image_bytes(void);
  int  gnuboy_load_sram(const char *file);
  int  gnuboy_save_sram(const char *file, bool quick_save);
}

// ==== Config ====
#define GBC_SAVE_DIR "/sd/gbc_saves"
/* Wait after the most recent SRAM write before autosave. Save bursts finish in
 * well under 1s on typical RPGs; 2s leaves headroom without leaving a long
 * window where GO→restart / power-off loses the in-game save (was 8000). */
#define SETTLE_MS 2000

// ==== Etat ====
static char*        g_save_path   = nullptr;
static TaskHandle_t g_task        = nullptr;
static SemaphoreHandle_t g_save_mu = nullptr;
static TickType_t   g_next_check  = 0;
static TickType_t   g_next_allow  = 0;
static TickType_t   g_last_write_tick = 0;
static uint32_t     g_last_write_gen = 0;
static uint32_t     g_last_saved_gen = 0;
static TickType_t   g_last_save   = 0;

static volatile bool g_flag_check = false;
static volatile bool g_flag_flush = false;

// ============================================================================
// Utils FS
// ============================================================================
static void make_save_path(const char* romPathOrName) {
  // Get the base filename (without full path) from the ROM path or name
  const char* base = share::gameSaveBasename(romPathOrName);
  char name[160] = {0};  // Temporary buffer for the save filename

  if (base && *base) {
    // Copy base name safely into local buffer
    strncpy(name, base, sizeof(name) - 1);

    // Find the last dot in the filename (file extension separator)
    char *dot = strrchr(name, '.');

    if (dot) {
      // If an extension exists, terminate the string at the dot
      // This removes the original extension (e.g., .gb, .gbc)
      *dot = '\0';
    }

    // Append ".sav" safely, ensuring no buffer overflow
    strncat(name, ".sav", sizeof(name) - strlen(name) - 1);

  } else {
    // Fallback name if no valid base name is found
    strcpy(name, "gbc_autosave.sav");
  }

  // Build the final save path: GBC_SAVE_DIR/<filename>.sav
  int n = snprintf(g_save_path, PATH_MAX, GBC_SAVE_DIR "/%s", name);

  // Ensure null-termination if truncated
  if (n < 0 || (size_t)n >= PATH_MAX) {
    g_save_path[PATH_MAX - 1] = '\0';
  }
}

static void ensure_save_lock() {
  if (!g_save_mu) {
    g_save_mu = xSemaphoreCreateMutex();
  }
}

// ============================================================================
// Save
// ============================================================================
static bool save_now() {
  if (!g_save_path) return false;
  if (!share::gameSaveEnsureParentReady(GBC_SAVE_DIR)) {
    EMU_LOG("[GBC][SAVE] storage path not ready, skip save\n");
    return false;
  }

  ensure_save_lock();
  if (!g_save_mu || xSemaphoreTake(g_save_mu, pdMS_TO_TICKS(15000)) != pdTRUE) {
    EMU_LOG("[GBC][SAVE] could not take save lock, skip\n");
    return false;
  }

  // 1) Stop display from starting new SPI frames first
  share::setGameIsSaving(true);
  // 2) Wait until any in-flight GbcDisp startWrite..endWrite finishes
  gbc_display_wait_idle();
  // 3) Quiesce shared SPI3 LCD before SD (same SCK/MOSI; CS 5 vs 12)
  lcd_quiesce();

  int rc = -1;
  const int max_attempts = 3;

  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    rc = gnuboy_save_sram(g_save_path, false);
    if (rc == 0) {
      break;
    }

    EMU_LOG("[GBC][SAVE] gnuboy_save_sram failed (rc=%d) for %s (attempt %d/%d, %s)\n",
           rc, g_save_path, attempt, max_attempts, "full");

    if (attempt < max_attempts) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }

  // 5) Resume LCD bus
  lcd_resume_after_bus();
  // 6) Allow display frames again
  share::setGameIsSaving(false);

  xSemaphoreGive(g_save_mu);

  if (rc != 0) {
    EMU_LOG("[GBC][SAVE] SRAM save FAILED rc=%d for %s\n", rc, g_save_path);
    return false;
  }

  g_last_save = xTaskGetTickCount();
  /* Do not push g_last_write_tick forward: SRAM writes that landed during
   * the snapshot must still be able to settle and flush. Only mark the
   * image caught-up when gnuboy actually cleared dirty. */
  if (!gnuboy_sram_dirty()) {
    g_last_saved_gen = gnuboy_sram_write_gen();
    g_last_write_gen = g_last_saved_gen;
  }

  EMU_LOG("[GBC][SAVE] SRAM saved to %s (durable)\n", g_save_path);
  return true;
}


// ============================================================================
// Task
// ============================================================================
static void SaveTask(void* /*arg*/) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    bool do_check = g_flag_check; g_flag_check = false;
    bool explicit_flush = g_flag_flush;
    bool do_flush = explicit_flush;
    TickType_t now = xTaskGetTickCount();

    if (do_check) {
      uint32_t gen = gnuboy_sram_write_gen();
      if (gen != g_last_write_gen) {
        g_last_write_gen = gen;
        g_last_write_tick = now;
      }

      if (gnuboy_sram_dirty() &&
          (now - g_last_write_tick) >= pdMS_TO_TICKS(SETTLE_MS)) {
        do_flush = true;
      }
    }

    if (!do_flush) {
      continue;
    }

    /* GAP throttles repeated SD writes, but must NEVER block flushing SRAM that
     * is newer than the last successful .sav — that was the Dirt rollback:
     * autosave at T, in-game save at T+5s, GO quit at T+10s → GAP still open,
     * dirty never flushed, reopen loads the older autosave. */
    const bool newer_than_sd =
        gnuboy_sram_dirty() && (g_last_write_gen != g_last_saved_gen);
    const bool gap_ok = (now >= g_next_allow) || explicit_flush || newer_than_sd;

    if (!gap_ok) {
      /* Keep flush request armed so we retry after GAP. */
      if (explicit_flush) {
        g_flag_flush = true;
      }
      continue;
    }

    bool ok = save_now();
    if (ok) {
      g_flag_flush = false;
      g_next_allow = xTaskGetTickCount() + pdMS_TO_TICKS(GAP_MS);
    } else {
      EMU_LOG("[GBC][SAVE] save failed, will retry on next tick\n");
      /* Keep explicit flush armed; settle path re-checks dirty next tick. */
      if (explicit_flush) {
        g_flag_flush = true;
      }
    }
  }
}

// ============================================================================
// API
// ============================================================================
extern "C" void gbc_save_init(const char* romPathOrName) {
  if (!g_save_path) {
    g_save_path = (char*)malloc(PATH_MAX);
    if (!g_save_path) {
      EMU_LOG("[GBC][SAVE] OOM on path alloc, autosave disabled\n");
      return;
    }
  }

  ensure_save_lock();

  g_save_path[0] = '\0';
  make_save_path(romPathOrName);

  g_next_check  = 0;
  g_next_allow  = 0;
  g_last_write_tick = xTaskGetTickCount();
  g_last_write_gen = gnuboy_sram_write_gen();
  g_last_saved_gen = g_last_write_gen;
  g_last_save   = 0;
  g_flag_check  = false;
  g_flag_flush  = false;

  /* GO→restart in share/input.cpp calls this so dirty SRAM hits SD first. */
  share::gameSaveSetQuitFlush(gbc_save_force_flush);

  if (!g_task) {
    xTaskCreatePinnedToCore(
      SaveTask,
      "GBC_SaveTask",
      12288,
      nullptr,
      3,   // below audio (6) and display (5); still quiesce LCD first
      &g_task,
      1    // core 1 — avoid preempting display mid-SPI on core 0
    );
  }

  EMU_LOG("[GBC][SAVE] path=%s settle=%dms\n", g_save_path, SETTLE_MS);
}

extern "C" void gbc_save_load(void) {
  if (!g_save_path) return;
  if (!share::gameSaveEnsureParentReady(GBC_SAVE_DIR)) {
    EMU_LOG("[GBC][SAVE] skip load (storage not ready)\n");
    return;
  }

  // Same SPI-safe order as save_now(): ILI9341 must not own SPI3 during SD SRAM I/O
  // 1) Stop display from starting new SPI frames
  share::setGameIsSaving(true);
  // 2) Wait until any in-flight GbcDisp startWrite..endWrite finishes
  gbc_display_wait_idle();
  // 3) Quiesce shared SPI3 LCD before SD (same SCK/MOSI; CS 5 vs 12)
  lcd_quiesce();

  /* v13 renamed .sav -> .sav.bak BEFORE the new file was durable, then
   * esp_restart could drop the tmp->sav install. Every later boot then
   * loaded that same .bak (or the untouched original .sav) and ignored a
   * newer .tmp. Pick the newest VALID image; never a stale backup when
   * .sav or a newer .tmp is good. */
  const int banks = gnuboy_sram_image_bytes();
  const int expect_rtc = banks > 0 ? banks + 48 : 0;

  auto size_ok = [&](off_t sz) -> bool {
    if (banks <= 0 || sz <= 0) return false;
    return sz == (off_t)banks || sz == (off_t)expect_rtc;
  };

  char path_sav[PATH_MAX];
  char path_tmp[PATH_MAX];
  char path_bak[PATH_MAX];
  strncpy(path_sav, g_save_path, sizeof(path_sav) - 1);
  path_sav[sizeof(path_sav) - 1] = '\0';
  path_tmp[0] = '\0';
  path_bak[0] = '\0';
  if ((size_t)snprintf(path_tmp, sizeof(path_tmp), "%s.tmp", g_save_path) >= sizeof(path_tmp))
    path_tmp[0] = '\0';
  if ((size_t)snprintf(path_bak, sizeof(path_bak), "%s.bak", g_save_path) >= sizeof(path_bak))
    path_bak[0] = '\0';

  struct stat st_sav, st_tmp, st_bak, st;
  bool has_sav = path_sav[0] && stat(path_sav, &st_sav) == 0 && size_ok(st_sav.st_size);
  bool has_tmp = path_tmp[0] && stat(path_tmp, &st_tmp) == 0 && size_ok(st_tmp.st_size);
  bool has_bak = path_bak[0] && stat(path_bak, &st_bak) == 0 && size_ok(st_bak.st_size);

  char load_path[PATH_MAX];
  load_path[0] = '\0';
  if (has_tmp && (!has_sav || st_tmp.st_mtime > st_sav.st_mtime)) {
    strncpy(load_path, path_tmp, sizeof(load_path) - 1);
    st = st_tmp;
    EMU_LOG("[GBC][SAVE] loading newer .tmp (mtime %ld > .sav %ld)\n",
            (long)st_tmp.st_mtime, has_sav ? (long)st_sav.st_mtime : -1L);
  } else if (has_sav) {
    strncpy(load_path, path_sav, sizeof(load_path) - 1);
    st = st_sav;
    EMU_LOG("[GBC][SAVE] loading canonical .sav\n");
  } else if (has_bak) {
    strncpy(load_path, path_bak, sizeof(load_path) - 1);
    st = st_bak;
    EMU_LOG("[GBC][SAVE] .sav missing/invalid; loading .sav.bak fallback\n");
  } else if (has_tmp) {
    strncpy(load_path, path_tmp, sizeof(load_path) - 1);
    st = st_tmp;
    EMU_LOG("[GBC][SAVE] loading leftover .tmp\n");
  }
  if (!load_path[0]) {
    EMU_LOG("[GBC][SAVE] no existing save file for %s\n", g_save_path);
    lcd_resume_after_bus();
    share::setGameIsSaving(false);
    return;
  }
  load_path[sizeof(load_path) - 1] = '\0';

  /* Heal a card stuck on .tmp/.bak: once we know which image is current,
   * copy it onto the canonical .sav so the next boot cannot prefer a stale
   * backup. */
  if (strcmp(load_path, path_sav) != 0) {
    FILE* in = fopen(load_path, "rb");
    FILE* out = in ? fopen(path_sav, "wb") : nullptr;
    if (in && out) {
      uint8_t buf[1024];
      size_t n;
      bool ok = true;
      while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
      }
      if (ok && fflush(out) == 0) {
        int fd = fileno(out);
        if (fd >= 0) fsync(fd);
      } else {
        ok = false;
      }
      fclose(out);
      fclose(in);
      if (ok) {
        int dfd = open(GBC_SAVE_DIR, O_RDONLY);
        if (dfd >= 0) { fsync(dfd); close(dfd); }
        EMU_LOG("[GBC][SAVE] promoted %s -> %s\n", load_path, path_sav);
      }
    } else {
      if (in) fclose(in);
      if (out) fclose(out);
    }
  }

  EMU_LOG("[GBC][SAVE] existing save size: %ld bytes (%s)\n",
          (long)st.st_size, load_path);

  // 4) Load SRAM from SD
  int rc = gnuboy_load_sram(load_path);
  if (rc != 0) {
    // Do not wipe/truncate the .sav — leave file untouched for retry
    EMU_LOG("[GBC][SAVE] load FAILED rc=%d for %s (file left untouched)\n",
            rc, load_path);
  } else {
    EMU_LOG("[GBC][SAVE] SRAM loaded from %s\n", load_path);
    g_last_saved_gen = gnuboy_sram_write_gen();
    g_last_write_gen = g_last_saved_gen;

    FILE* f = fopen(load_path, "rb");
    if (f) {
      uint8_t buf[16];
      size_t got = fread(buf, 1, sizeof(buf), f);
      fclose(f);
      EMU_LOG("[GBC][SAVE] first bytes: ");
      for (size_t i = 0; i < got; ++i) EMU_LOG("%02X ", buf[i]);
      EMU_LOG("\n");
    }
  }

  // 5) Resume LCD bus
  lcd_resume_after_bus();
  // 6) Allow display frames again
  share::setGameIsSaving(false);
}

extern "C" void gbc_save_tick(void) {
  if (share::gameIsSaving()) return;
  if (!g_save_path) return;
  TickType_t now = xTaskGetTickCount();
  if (now < g_next_check) return;

  g_next_check = now + pdMS_TO_TICKS(CHECK_MS);
  g_flag_check = true;

  if (g_task) xTaskNotifyGive(g_task);
}

extern "C" void gbc_save_request_flush(void) {
  g_flag_flush = true;
  if (g_task) xTaskNotifyGive(g_task);
}

extern "C" void gbc_save_force_flush(void) {
  /* Synchronous path for GO→quit and for the cheat-menu pause. save_now()
   * takes the mutex so we do not race SaveTask. Always write current SRAM,
   * retry once if the snapshot raced an in-game save, then pause so the SD
   * controller finishes the fsync before esp_restart. */
  while (share::gameIsSaving()) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (!save_now()) {
    vTaskDelay(pdMS_TO_TICKS(20));
    save_now();
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}
