#include "gbc_settings.h"

#include <Preferences.h>
#include "gbc_display.h"

extern "C" {
#include "gnuboy/gnuboy.h"
}

static int clamp_pal(int pal) {
  if (pal < 0 || pal >= GB_PALETTE_COUNT) return GB_PALETTE_SGB;
  return pal;
}

static int clamp_zoom(int zoom) {
  if (zoom < 100) return 100;
  if (zoom > 150) return 150;
  return zoom;
}

void gbc_settings_load(void) {
  Preferences prefs;
  if (!prefs.begin("cardputer_emu", true)) return;

  gbcFullScreen = prefs.getBool("gbc_full", false);
  gbcZoomPercent = clamp_zoom(prefs.getInt("gbc_zoom", 100));
  gbc_colour_correct = prefs.getBool("gbc_cc", false) ? 1 : 0;
  gbc_frame_blend = prefs.getBool("gbc_blend", false) ? 1 : 0;
  gbc_night = prefs.getBool("gbc_night", false) ? 1 : 0;
  int pal = clamp_pal(prefs.getInt("gb_pal", GB_PALETTE_SGB));
  gbPalette = pal;
  gbcDmgPalette = pal;
  prefs.end();
}

void gbc_settings_save(void) {
  Preferences prefs;
  if (!prefs.begin("cardputer_emu", false)) return;

  prefs.putBool("gbc_full", gbcFullScreen);
  prefs.putInt("gbc_zoom", clamp_zoom(gbcZoomPercent));
  prefs.putBool("gbc_cc", gbc_colour_correct != 0);
  prefs.putBool("gbc_blend", gbc_frame_blend != 0);
  prefs.putBool("gbc_night", gbc_night != 0);
  int pal = (gbPalette >= 0) ? gbPalette : gbcDmgPalette;
  prefs.putInt("gb_pal", clamp_pal(pal));
  prefs.end();
}
