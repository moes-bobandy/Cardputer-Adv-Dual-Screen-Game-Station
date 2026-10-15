// gbc_display.h
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool gbcFullScreen;
extern int  gbcZoomPercent;
extern int  gbPalette;
extern int  gbcDmgPalette;

/* 0 = off. Read by the LCD core (colour) and the display task (blend, night). */
extern volatile int gbc_colour_correct;
extern volatile int gbc_frame_blend;
extern volatile int gbc_night;

void gbc_display_init(void);
void gbc_display_start(void);
void gbc_display_stop(void);

/* Shared SPI3 bus: true while GbcDisp holds startWrite..endWrite */
int gbc_display_bus_busy(void);
/* Spin until display releases SPI (or timeout ~500ms). */
void gbc_display_wait_idle(void);

// Called from the GNUBOY video callback
void gbc_display_submit_frame(const uint16_t *fb,
                              int pitch,   // in pixels
                              int width,
                              int height);

void gbc_toggle_scale_mode(void);
void gbc_toggle_frame_blend(void);
void gbc_toggle_colour_correct(void);
void gbc_toggle_night(void);
void gbc_cycle_dmg_palette(void);
const char* gbc_scale_name(void);
const char* gbc_dmg_palette_name(void);

#ifdef __cplusplus
}
#endif
