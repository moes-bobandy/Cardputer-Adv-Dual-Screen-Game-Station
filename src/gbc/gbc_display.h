// gbc_display.h
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool gbcFullScreen;
extern int  gbcZoomPercent;
extern int  gbPalette;

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

#ifdef __cplusplus
}
#endif
