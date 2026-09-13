#include "gbc_save.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "share/game_save.h"
#include "share/emu_log_cpp.h"
#include "cardputer/LGFX_ILI9341.h"
#include "gbc_display.h"

// ==== API ====
extern "C" {
  bool gnuboy_sram_dirty(void);
  uint32_t gnuboy_sram_write_gen(void);
  int  gnuboy_load_sram(const char *file);
  int  gnuboy_save_sram(const char *file, bool quick_save);
}

// ==== Config ====
#define GBC_SAVE_DIR "/sd/gbc_saves"
#define SETTLE_MS 8000  /* wait after the most recent SRAM write */

// ==== Etat ====
static char*        g_save_path   = nullptr;
static TaskHandle_t g_task        = nullptr;
static TickType_t   g_next_check  = 0;
static TickType_t   g_next_allow  = 0;
static TickType_t   g_last_write_tick = 0;
static uint32_t     g_last_write_gen = 0;
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

// ============================================================================
// Save
// ============================================================================
static bool save_now() {
  if (!g_save_path) return false;
  if (!share::gameSaveEnsureParentReady(GBC_SAVE_DIR)) {
    EMU_LOG("[GBC][SAVE] storage path not ready, skip save\n");
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

  if (rc != 0) {
    EMU_LOG("[GBC][SAVE] SRAM save FAILED rc=%d for %s\n", rc, g_save_path);
    return false;
  }

  g_last_save   = xTaskGetTickCount();
  g_last_write_tick = xTaskGetTickCount();

  EMU_LOG("[GBC][SAVE] SRAM saved to %s (full atomic)\n", g_save_path);
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
          (now - g_last_write_tick) >= pdMS_TO_TICKS(SETTLE_MS) &&
          now >= g_next_allow) {
        do_flush = true;
      }
    }

    // Explicit request_flush bypasses the settle delay, but retains GAP_MS.
    if (do_flush && now >= g_next_allow) {
      bool ok = save_now();
      if (ok) {
        g_flag_flush = false;
        // Wait 15s ONLY if the save succeeded
        g_next_allow = xTaskGetTickCount() + pdMS_TO_TICKS(GAP_MS);
      } else {
        EMU_LOG("[GBC][SAVE] save failed, will retry on next tick\n");
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

  g_save_path[0] = '\0';
  make_save_path(romPathOrName);

  g_next_check  = 0;
  g_next_allow  = 0;
  g_last_write_tick = xTaskGetTickCount();
  g_last_write_gen = gnuboy_sram_write_gen();
  g_last_save   = 0;
  g_flag_check  = false;
  g_flag_flush  = false;

  if (!g_task) {
    xTaskCreatePinnedToCore(
      SaveTask,
      "GBC_SaveTask",
      8192,
      nullptr,
      3,   // below audio (6) and display (5); still quiesce LCD first
      &g_task,
      1    // core 1 — avoid preempting display mid-SPI on core 0
    );
  }

  EMU_LOG("[GBC][SAVE] path=%s\n", g_save_path);
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

  struct stat st;
  if (stat(g_save_path, &st) != 0) {
    EMU_LOG("[GBC][SAVE] no existing save file for %s\n", g_save_path);
    lcd_resume_after_bus();
    share::setGameIsSaving(false);
    return;
  }

  EMU_LOG("[GBC][SAVE] existing save size: %ld bytes\n", (long)st.st_size);

  // 4) Load SRAM from SD
  int rc = gnuboy_load_sram(g_save_path);
  if (rc != 0) {
    // Do not wipe/truncate the .sav — leave file untouched for retry
    EMU_LOG("[GBC][SAVE] load FAILED rc=%d for %s (file left untouched)\n",
            rc, g_save_path);
  } else {
    EMU_LOG("[GBC][SAVE] SRAM loaded from %s\n", g_save_path);

    FILE* f = fopen(g_save_path, "rb");
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
  save_now();
}
