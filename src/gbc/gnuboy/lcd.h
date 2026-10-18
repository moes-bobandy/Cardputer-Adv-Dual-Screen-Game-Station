#pragma once

#include "gnuboy.h"

void gb_lcd_init(void);
void gb_lcd_reset(bool hard);
void gb_lcd_emulate(int cycles);
void gb_lcd_stat_trigger(void);
void gb_lcd_lcdc_change(byte b);
void gb_lcd_pal_dirty(void);
/* Build the RGB565 palette now if it exists (warm filter included). */
void gb_lcd_rebuild_palette(void);
