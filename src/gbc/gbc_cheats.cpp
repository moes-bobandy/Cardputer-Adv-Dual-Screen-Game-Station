#include "gbc_cheats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <vector>
#include <string>

#include "esp_heap_caps.h"

#include "share/game_save.h"
#include "share/emu_log_cpp.h"
#include "cardputer/LGFX_ILI9341.h"
#include "cardputer/CardputerView.h"
#include "cardputer/CardputerInput.h"
#include "cardputer/VerticalSelector.h"
#include "gbc_display.h"

extern "C" {
  void gbc_cheat_writeb(unsigned a, unsigned char b);
}

#define GBC_CHEATS_DIR "/sd/gbc_cheats"
#define GBC_CHEATS_MAX 512
#define GBC_CHEAT_NAME_MAX 40

struct GbcCheat {
  uint16_t addr;
  uint8_t  value;
  bool     enabled;
  char     name[GBC_CHEAT_NAME_MAX];
};

static GbcCheat s_cheats[GBC_CHEATS_MAX];
static int      s_count = 0;
static char     s_cht_path[256];
static char     s_txt_path[256];
static char     s_rom_base[160];
static volatile bool s_menu_req = false;

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------
static void strip_ext(char* name) {
  char* dot = strrchr(name, '.');
  if (dot) *dot = '\0';
}

static void make_cheat_paths(const char* romPathOrName) {
  s_cht_path[0] = '\0';
  s_txt_path[0] = '\0';
  s_rom_base[0] = '\0';

  const char* base = share::gameSaveBasename(romPathOrName);
  if (!base || !*base) {
    strncpy(s_rom_base, "unknown", sizeof(s_rom_base) - 1);
  } else {
    strncpy(s_rom_base, base, sizeof(s_rom_base) - 1);
    s_rom_base[sizeof(s_rom_base) - 1] = '\0';
    strip_ext(s_rom_base);
  }

  snprintf(s_cht_path, sizeof(s_cht_path), GBC_CHEATS_DIR "/%s.cht", s_rom_base);
  snprintf(s_txt_path, sizeof(s_txt_path), GBC_CHEATS_DIR "/%s.txt", s_rom_base);
}

// ---------------------------------------------------------------------------
// GameShark decode: 01xxCDAB / 91xxCDAB -> write xx to address 0xABCD
// Example: 01FF9FD5 -> value 0xFF at 0xD59F
// ---------------------------------------------------------------------------
static bool parse_gs_code(const char* hex8, uint16_t* outAddr, uint8_t* outVal) {
  if (!hex8 || !outAddr || !outVal) return false;

  char buf[9] = {0};
  int n = 0;
  for (const char* p = hex8; *p && n < 8; ++p) {
    if (isspace((unsigned char)*p) || *p == '-' || *p == '_') continue;
    if (!isxdigit((unsigned char)*p)) return false;
    buf[n++] = (char)toupper((unsigned char)*p);
  }
  if (n != 8) return false;

  unsigned long code = strtoul(buf, nullptr, 16);
  uint8_t type = (uint8_t)((code >> 24) & 0xFF);
  if (type != 0x01 && type != 0x91) return false;

  *outVal  = (uint8_t)((code >> 16) & 0xFF);
  uint8_t cd = (uint8_t)((code >> 8) & 0xFF); // address low
  uint8_t ab = (uint8_t)(code & 0xFF);        // address high
  *outAddr = (uint16_t)((ab << 8) | cd);

  // Frame-applied writes are intentionally restricted to cartridge RAM,
  // work RAM and high RAM. In particular, never let a typo write ROM,
  // cartridge control registers, VRAM/OAM, or MMIO/interrupt registers.
  if (!((*outAddr >= 0xA000 && *outAddr <= 0xDFFF) ||
        (*outAddr >= 0xFF80 && *outAddr <= 0xFFFE))) {
    return false;
  }
  return true;
}

static bool add_code(uint16_t addr, uint8_t value, bool enabled, const char* name) {
  if (s_count >= GBC_CHEATS_MAX) return false;
  GbcCheat& c = s_cheats[s_count++];
  c.addr = addr;
  c.value = value;
  c.enabled = enabled;
  if (name && *name) {
    strncpy(c.name, name, GBC_CHEAT_NAME_MAX - 1);
    c.name[GBC_CHEAT_NAME_MAX - 1] = '\0';
  } else {
    snprintf(c.name, sizeof(c.name), "%04X=%02X", addr, value);
  }
  return true;
}

static void trim_inplace(char* s) {
  if (!s) return;
  char* start = s;
  while (*start && isspace((unsigned char)*start)) ++start;
  if (start != s) memmove(s, start, strlen(start) + 1);
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1])) {
    s[--len] = '\0';
  }
  // strip surrounding quotes
  if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') || (s[0] == '\'' && s[len - 1] == '\''))) {
    s[len - 1] = '\0';
    memmove(s, s + 1, len - 1);
  }
}

static void add_codes_from_token_list(const char* codes, bool enabled, const char* desc) {
  if (!codes) return;
  int multi = 0;
  for (const char* p = codes; *p; ++p) if (*p == '+') multi++;

  const char* tok = codes;
  int idx = 0;
  while (*tok) {
    const char* tokenEnd = strchr(tok, '+');
    if (!tokenEnd) tokenEnd = tok + strlen(tok);

    while (tok < tokenEnd && isspace((unsigned char)*tok)) ++tok;
    while (tokenEnd > tok && isspace((unsigned char)tokenEnd[-1])) --tokenEnd;

    // A normalized code is only eight digits; leave modest room for the
    // accepted '-'/'_' separators without putting a large scratch buffer on
    // the Arduino task stack.
    char token[24];
    size_t tokenLen = (size_t)(tokenEnd - tok);
    if (tokenLen >= sizeof(token)) {
      tokenLen = 0; // overlong/malformed token: reject it below
    } else {
      memcpy(token, tok, tokenLen);
    }
    token[tokenLen] = '\0';

    uint16_t addr = 0;
    uint8_t val = 0;
    if (tokenLen && parse_gs_code(token, &addr, &val)) {
      char name[GBC_CHEAT_NAME_MAX];
      if (desc && *desc) {
        if (multi > 0) {
          snprintf(name, sizeof(name), "%s #%d", desc, idx + 1);
        } else {
          strncpy(name, desc, sizeof(name) - 1);
          name[sizeof(name) - 1] = '\0';
        }
      } else {
        name[0] = '\0';
      }
      add_code(addr, val, enabled, name[0] ? name : nullptr);
      idx++;
    }
    if (!*tokenEnd) break;
    tok = tokenEnd + 1;
  }
}

static bool looks_like_hex8(const char* s) {
  int n = 0;
  for (const char* p = s; *p; ++p) {
    if (isspace((unsigned char)*p) || *p == '-' || *p == '_') continue;
    if (!isxdigit((unsigned char)*p)) return false;
    n++;
  }
  return n == 8;
}

static bool parse_cht_buffer(char* buf, size_t len) {
  // Collect RetroArch-style keyed cheats, then fall back to bare codes.
  struct Pending {
    char desc[GBC_CHEAT_NAME_MAX];
    char code[128];
    bool enable;
    bool has_code;
  };

  // Pre-scan for RetroArch cheatN_ keys. Bare hex dumps (e.g. Have All Cards
  // 445 codes) skip the Pending table entirely (~87 KiB at MAX=512).
  bool has_ra = false;
  {
    const char* scan = buf;
    const char* end = buf + len;
    while (scan < end) {
      const char* nl = (const char*)memchr(scan, '\n', (size_t)(end - scan));
      size_t lineLen = nl ? (size_t)(nl - scan) : (size_t)(end - scan);
      const char* p = scan;
      size_t remain = lineLen;
      while (remain > 0 && isspace((unsigned char)*p)) { ++p; --remain; }
      if (remain >= 6 && strncmp(p, "cheat", 5) == 0 &&
          isdigit((unsigned char)p[5])) {
        has_ra = true;
        break;
      }
      if (!nl) break;
      scan = nl + 1;
    }
  }

  Pending* pending = nullptr;
  if (has_ra) {
    // Sparse indices up to GBC_CHEATS_MAX-1 (~87 KiB at 512 with code[128]).
    // Must not live on the small Arduino app-task stack.
    // MALLOC_CAP_8BIT permits normal RAM or PSRAM.
    pending = (Pending*)heap_caps_calloc(
      GBC_CHEATS_MAX, sizeof(Pending), MALLOC_CAP_8BIT);
    if (!pending) {
      EMU_LOG("[GBC][CHEAT] pending table allocation failed (%u bytes)\n",
              (unsigned)(GBC_CHEATS_MAX * sizeof(Pending)));
      return false;
    }
    for (int i = 0; i < GBC_CHEATS_MAX; ++i) pending[i].enable = true;
  }

  char* line = buf;
  char* end = buf + len;
  while (line < end) {
    char* nl = (char*)memchr(line, '\n', (size_t)(end - line));
    if (nl) *nl = '\0';
    // strip CR
    size_t lineLen = strlen(line);
    if (lineLen > 0 && line[lineLen - 1] == '\r') line[lineLen - 1] = '\0';

    char* p = line;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p && *p != '#' && !(p[0] == '/' && p[1] == '/')) {
      // cheatN_xxx = value (only when Pending table was allocated)
      if (pending && strncmp(p, "cheat", 5) == 0 &&
          isdigit((unsigned char)p[5])) {
        char* underscore = strchr(p, '_');
        char* eq = strchr(p, '=');
        if (underscore && eq && eq > underscore) {
          int idx = atoi(p + 5);
          if (idx >= 0 && idx < GBC_CHEATS_MAX) {
            *eq = '\0';
            char* key = underscore + 1;
            char* val = eq + 1;
            trim_inplace(key);
            trim_inplace(val);
            if (strcmp(key, "desc") == 0 || strcmp(key, "description") == 0) {
              strncpy(pending[idx].desc, val, GBC_CHEAT_NAME_MAX - 1);
            } else if (strcmp(key, "code") == 0) {
              strncpy(pending[idx].code, val, sizeof(pending[idx].code) - 1);
              pending[idx].has_code = true;
            } else if (strcmp(key, "enable") == 0) {
              pending[idx].enable =
                (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
            }
          }
        }
      } else if (looks_like_hex8(p) || strchr(p, '+')) {
        // bare code line or code+code
        add_codes_from_token_list(p, false, nullptr);
      }
    }

    if (!nl) break;
    line = nl + 1;
  }

  if (pending) {
    for (int i = 0; i < GBC_CHEATS_MAX; ++i) {
      if (!pending[i].has_code) continue;
      // Always load disabled; user enables in the `c` menu (avoids bad
      // English-translation addresses corrupting boot if applied early).
      add_codes_from_token_list(
        pending[i].code,
        false,
        pending[i].desc[0] ? pending[i].desc : nullptr
      );
    }
    heap_caps_free(pending);
  }
  return true;
}

static int load_file_spi_safe(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return -1;

  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long sz = ftell(f);
  if (sz < 0 || sz > 64 * 1024) { fclose(f); return -1; }
  rewind(f);

  char* buf = (char*)heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_8BIT);
  if (!buf) { fclose(f); return -1; }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[got] = '\0';

  bool parsed = parse_cht_buffer(buf, got);
  heap_caps_free(buf);
  return parsed ? s_count : -1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
extern "C" void gbc_cheats_init(const char* romPathOrName) {
  s_count = 0;
  s_menu_req = false;
  memset(s_cheats, 0, sizeof(s_cheats));
  make_cheat_paths(romPathOrName);
  EMU_LOG("[GBC][CHEAT] path=%s\n", s_cht_path);
}

extern "C" int gbc_cheats_load(void) {
  s_count = 0;
  memset(s_cheats, 0, sizeof(s_cheats));

  if (!s_cht_path[0]) return 0;

  // Ensure parent exists (mkdir ok); missing file is not an error
  share::gameSaveEnsureParentReady(GBC_CHEATS_DIR);

  share::setGameIsSaving(true);
  gbc_display_wait_idle();
  lcd_quiesce();

  int rc = -1;
  struct stat st;
  if (stat(s_cht_path, &st) == 0) {
    EMU_LOG("[GBC][CHEAT] loading %s\n", s_cht_path);
    rc = load_file_spi_safe(s_cht_path);
  } else if (stat(s_txt_path, &st) == 0) {
    EMU_LOG("[GBC][CHEAT] loading %s\n", s_txt_path);
    rc = load_file_spi_safe(s_txt_path);
  } else {
    EMU_LOG("[GBC][CHEAT] no cheat file for %s\n", s_rom_base);
  }

  lcd_resume_after_bus();
  share::setGameIsSaving(false);

  // Codes are present but OFF until the user toggles them in the `c` menu.
  for (int i = 0; i < s_count; ++i) s_cheats[i].enabled = false;

  EMU_LOG("[GBC][CHEAT] loaded %d codes (all disabled by default)\n", s_count);
  return (rc < 0) ? 0 : s_count;
}

extern "C" void gbc_cheats_apply(void) {
  if (s_count == 0) return;
  for (int i = 0; i < s_count; ++i) {
    if (!s_cheats[i].enabled) continue;
    // Address range already validated at parse time (A000-DFFF / FF80-FFFE).
    gbc_cheat_writeb(s_cheats[i].addr, s_cheats[i].value);
  }
}

extern "C" void gbc_cheats_enable_all(bool enable) {
  for (int i = 0; i < s_count; ++i) s_cheats[i].enabled = enable;
}

extern "C" int gbc_cheats_count(void) {
  return s_count;
}

extern "C" void gbc_cheats_request_menu(void) {
  s_menu_req = true;
}

extern "C" bool gbc_cheats_menu_requested(void) {
  return s_menu_req;
}

extern "C" void gbc_cheats_clear_menu_request(void) {
  s_menu_req = false;
}

// ---------------------------------------------------------------------------
// In-game menu
// ---------------------------------------------------------------------------
void gbc_cheats_run_menu(void) {
  s_menu_req = false;

  // Pause display task SPI so UI owns the panel (no setColorDepth / COLMOD).
  share::setGameIsSaving(true);
  gbc_display_wait_idle();

  CardputerView display;
  CardputerInput input;
  // Do NOT call display.initialize() — that re-runs setColorDepth mid-run.
  // Re-bind the already-booted panel pointer for CardputerView drawing helpers
  // (sets CardputerView::Display so VerticalSelector draws on external).
  display.setExternalDisplay(&externalDisplay);

  // Immediate visual feedback before VerticalSelector (external + internal).
  externalDisplay.fillScreen(TFT_BLACK);
  externalDisplay.setTextColor(TFT_WHITE, TFT_BLACK);
  externalDisplay.setTextDatum(middle_center);
  externalDisplay.setTextSize(2);
  externalDisplay.drawString("CHEATS",
                             externalDisplay.width() / 2,
                             externalDisplay.height() / 2);
  {
    auto& id = M5Cardputer.Display;
    id.fillScreen(TFT_BLACK);
    id.setTextDatum(middle_center);
    id.setTextColor(TFT_WHITE, TFT_BLACK);
    id.setTextSize(1);
    id.drawString("Cheats open", id.width() / 2, id.height() / 2);
  }
  delay(200);

  input.flushInput(50);

  auto build_options = [&](std::vector<std::string>& opts) {
    opts.clear();
    opts.push_back("Enable all");
    opts.push_back("Disable all");
    opts.push_back("Reload from SD");
    for (int i = 0; i < s_count; ++i) {
      char row[64];
      snprintf(row, sizeof(row), "%s %s",
               s_cheats[i].enabled ? "[ON]" : "[OFF]",
               s_cheats[i].name);
      opts.push_back(row);
    }
    opts.push_back("Back");
  };

  if (s_count == 0) {
    display.topBar("Cheats", true, false);
    display.subMessage("No cheats loaded", 800);
    std::vector<std::string> emptyOpts = {
      "Reload from SD",
      "Back"
    };
    VerticalSelector sel(display, input);
    int choice = sel.select("Cheats", emptyOpts, true, false, {}, {}, false, false);
    if (choice == 0) {
      // Reload needs SPI-safe SD
      gbc_cheats_load();
      // gbc_cheats_load clears setGameIsSaving — re-assert pause for UI
      share::setGameIsSaving(true);
      gbc_display_wait_idle();
      display.setExternalDisplay(&externalDisplay);
    }
    if (s_count == 0) {
      display.topBar("Cheats", true, false);
      display.subMessage("Still empty — check SD", 1200);
      share::setGameIsSaving(false);
      return;
    }
  }

  VerticalSelector selector(display, input);
  int selIndex = 0;  // preserve highlight across toggle/redraw
  while (true) {
    std::vector<std::string> options;
    build_options(options);
    int choice = selector.select("Cheats", options, true, false, {}, {}, false, false, selIndex);
    if (choice < 0) break; // ESC / left = back
    selIndex = choice;  // stay on same row after enable/disable/reload

    const int idxEnableAll  = 0;
    const int idxDisableAll = 1;
    const int idxReload     = 2;
    const int idxFirstCheat = 3;
    const int idxBack       = idxFirstCheat + s_count;

    if (choice == idxBack) break;
    if (choice == idxEnableAll) {
      gbc_cheats_enable_all(true);
      continue;
    }
    if (choice == idxDisableAll) {
      gbc_cheats_enable_all(false);
      continue;
    }
    if (choice == idxReload) {
      gbc_cheats_load();
      share::setGameIsSaving(true);
      gbc_display_wait_idle();
      display.setExternalDisplay(&externalDisplay);
      if (s_count == 0) {
        display.subMessage("No cheat file found", 1000);
      } else {
        char msg[48];
        snprintf(msg, sizeof(msg), "Loaded %d codes", s_count);
        display.subMessage(msg, 800);
      }
      continue;
    }
    if (choice >= idxFirstCheat && choice < idxBack) {
      int ci = choice - idxFirstCheat;
      s_cheats[ci].enabled = !s_cheats[ci].enabled;
      continue;
    }
  }

  externalDisplay.fillScreen(TFT_BLACK);
  share::setGameIsSaving(false);
}
