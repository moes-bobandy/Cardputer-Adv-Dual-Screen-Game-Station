#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Init paths from ROM name; does not load yet. */
void gbc_cheats_init(const char* romPathOrName);

/**
 * SPI-safe load from /sd/gbc_cheats/<basename>.cht (or .txt).
 * Returns number of codes loaded (0 if missing/empty).
 */
int gbc_cheats_load(void);

/** Apply all enabled GameShark codes via writeb each frame. */
void gbc_cheats_apply(void);

void gbc_cheats_enable_all(bool enable);
int  gbc_cheats_count(void);

void gbc_cheats_request_menu(void);
bool gbc_cheats_menu_requested(void);
void gbc_cheats_clear_menu_request(void);

#ifdef __cplusplus
}

/** Blocking in-game cheat menu (pauses emu; caller must skip gnuboy_run). */
void gbc_cheats_run_menu(void);
#endif
