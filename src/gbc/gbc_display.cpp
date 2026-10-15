#include "gbc_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <string.h>
#include "esp_heap_caps.h"
extern "C" {
  #include "gnuboy/gnuboy.h"
#include "share/emu_log_cpp.h"
  void gb_lcd_pal_dirty(void);
}

#include "../cardputer/CardputerView.h"
#include "share/game_save.h"
#include "gbc_settings.h"

extern LGFX_ILI9341 externalDisplay;

// Globals
// Integer scale is the default. gbcFullScreen selects near-fullscreen nearest-neighbour.
bool gbcFullScreen = false;
int  gbcZoomPercent = 100;
int gbPalette = 36; // DMG default palette
int gbcDmgPalette = 36;

extern "C" volatile int gbc_colour_correct = 0;
extern "C" volatile int gbc_frame_blend = 0;

typedef struct {
  const uint16_t *fb;
  int pitch;
  int width;
  int height;
} GbcFrameMsg;

static QueueHandle_t s_frameQ  = nullptr;
static TaskHandle_t  s_task    = nullptr;
static volatile bool s_displayBusBusy = false;
static uint16_t     *s_lineBuf = nullptr;
static int           s_lineCap = 0;

static constexpr int kMaxDstW = 320;
static constexpr int kMaxDstH = 240;
static constexpr int kBatchLines = 16; // small DMA batches; never yield inside startWrite

struct GbcDisplayTransform {
  int dstW;
  int dstH;
  int xOffset;
  int yOffset;
  int intScale; // >0 equal N×N pixels; 0 = nearest map
};

static GbcDisplayTransform s_transform;
static uint16_t s_xMap[kMaxDstW];
static uint16_t s_yMap[kMaxDstH];

static int  s_lastZoomPercent = -1;
static bool s_lastFullScreen  = false;
static int  s_lastSrcW        = 0;
static int  s_lastSrcH        = 0;
static int  s_lastLcdW        = 0;
static int  s_lastLcdH        = 0;

static bool s_nightMode = true; // orange night-mode warm filter; default ON

static uint16_t* s_prev = nullptr;
static uint16_t* s_blend = nullptr;
static int s_blendPixels = 0;
static bool s_prevValid = false;

static const int kCuratedDmg[] = {
  GB_PALETTE_LCD,
  GB_PALETTE_GREY,
  GB_PALETTE_POCKET,
  GB_PALETTE_LIGHT,
  GB_PALETTE_DMG,
  GB_PALETTE_MGB0,
  GB_PALETTE_MGB1,
  GB_PALETTE_SGB,
};
static constexpr int kCuratedDmgCount = (int)(sizeof(kCuratedDmg) / sizeof(kCuratedDmg[0]));

static inline uint16_t gbc_warm_rgb565(uint16_t c)
{
  // RGB565: RRRRR GGGGGG BBBBB — lift R, attenuate G/B toward orange
  uint16_t r = (c >> 11) & 0x1F;
  uint16_t g = (c >> 5)  & 0x3F;
  uint16_t b =  c        & 0x1F;
  uint16_t r2 = r + (r >> 3);
  if (r2 > 31) r2 = 31;
  uint16_t g2 = (uint16_t)((g * 55) / 64);
  uint16_t b2 = (uint16_t)((b * 40) / 64);
  return (uint16_t)((r2 << 11) | (g2 << 5) | b2);
}

extern "C" void gbc_display_set_night_mode(int on)
{
  s_nightMode = (on != 0);
}

extern "C" void gbc_display_toggle_night_mode(void)
{
  s_nightMode = !s_nightMode;
}

extern "C" int gbc_display_night_mode(void)
{
  return s_nightMode ? 1 : 0;
}

static inline int clampi(int v, int lo, int hi)
{
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Bresenham / error-diffusion: even duplicate (or skip) distribution across src.
static void build_bresenham_map(uint16_t *map, int dst, int src)
{
  if (dst <= 0 || src <= 0) return;
  int acc = 0;
  int srcIdx = 0;
  for (int i = 0; i < dst; ++i) {
    map[i] = (uint16_t)clampi(srcIdx, 0, src - 1);
    acc += src;
    while (acc >= dst) {
      acc -= dst;
      if (srcIdx < src - 1) {
        ++srcIdx;
      }
    }
  }
}

static void gbc_display_transform(int srcW, int srcH)
{
  int lcdW = externalDisplay.width();
  int lcdH = externalDisplay.height();

  if (lcdW <= 0 || lcdH <= 0 || srcW <= 0 || srcH <= 0) {
    return;
  }
  if (lcdW > kMaxDstW) lcdW = kMaxDstW;
  if (lcdH > kMaxDstH) lcdH = kMaxDstH;

  bool full = gbcFullScreen;
  int  zoom = (gbcZoomPercent > 0) ? gbcZoomPercent : 100;

  if (zoom == s_lastZoomPercent &&
      full == s_lastFullScreen &&
      srcW == s_lastSrcW &&
      srcH == s_lastSrcH &&
      lcdW == s_lastLcdW &&
      lcdH == s_lastLcdH) {
    return;
  }

  // Clear bars whenever the transform changes (letterbox / pillarbox).
  externalDisplay.fillScreen(TFT_BLACK);

  s_lastZoomPercent = zoom;
  s_lastFullScreen  = full;
  s_lastSrcW        = srcW;
  s_lastSrcH        = srcH;
  s_lastLcdW        = lcdW;
  s_lastLcdH        = lcdH;
  s_transform.intScale = 0;

  // Non-100% zoom: keep float-built maps (rare path). Prefer 100% fully integer.
  if (zoom != 100) {
    float zoomFactor = zoom / 100.0f;
    if (zoomFactor <= 0.0f) zoomFactor = 1.0f;

    int dstW, dstH, xOffset, yOffset;
    float baseInvScaleX, baseInvScaleY;

    if (!full) {
      float scaleX = (float)lcdW / (float)srcW;
      float scaleY = (float)lcdH / (float)srcH;
      float scale  = (scaleX < scaleY) ? scaleX : scaleY;
      if (scale <= 0.0f) scale = 1.0f;

      dstW = (int)(srcW * scale);
      dstH = (int)(srcH * scale);
      if (dstW < 1) dstW = 1;
      if (dstH < 1) dstH = 1;
      if (dstW > lcdW) dstW = lcdW;
      if (dstH > lcdH) dstH = lcdH;

      xOffset = (lcdW - dstW) / 2;
      yOffset = (lcdH - dstH) / 2;
      baseInvScaleX = (float)srcW / (float)dstW;
      baseInvScaleY = (float)srcH / (float)dstH;
    } else {
      dstW = lcdW;
      dstH = lcdH;
      xOffset = 0;
      yOffset = 0;
      baseInvScaleX = (float)srcW / (float)dstW;
      baseInvScaleY = (float)srcH / (float)dstH;
    }

    float invScaleX = baseInvScaleX / zoomFactor;
    float invScaleY = baseInvScaleY / zoomFactor;
    float srcCX = (float)srcW * 0.5f;
    float srcCY = (float)srcH * 0.5f;
    float dstCX = (float)(dstW - 1) * 0.5f;
    float dstCY = (float)(dstH - 1) * 0.5f;

    s_transform.dstW    = dstW;
    s_transform.dstH    = dstH;
    s_transform.xOffset = xOffset;
    s_transform.yOffset = yOffset;

    for (int x = 0; x < dstW; ++x) {
      int srcX = (int)(srcCX + ((float)x - dstCX) * invScaleX);
      s_xMap[x] = (uint16_t)clampi(srcX, 0, srcW - 1);
    }
    for (int y = 0; y < dstH; ++y) {
      int srcY = (int)(srcCY + ((float)y - dstCY) * invScaleY);
      s_yMap[y] = (uint16_t)clampi(srcY, 0, srcH - 1);
    }
    return;
  }

  // Integer scale: every source pixel is an N×N block, centred, black border.
  if (!full) {
    int iscaleX = lcdW / srcW;
    int iscaleY = lcdH / srcH;
    int iscale  = (iscaleX < iscaleY) ? iscaleX : iscaleY;
    if (iscale < 1) iscale = 1;

    int dstW = srcW * iscale;
    int dstH = srcH * iscale;
    if (dstW > lcdW) dstW = lcdW;
    if (dstH > lcdH) dstH = lcdH;

    s_transform.dstW     = dstW;
    s_transform.dstH     = dstH;
    s_transform.xOffset  = (lcdW - dstW) / 2;
    s_transform.yOffset  = (lcdH - dstH) / 2;
    s_transform.intScale = iscale;
    return;
  }

  // Fill: aspect-correct nearest neighbour, centred, black bars.
  {
    float scaleX = (float)lcdW / (float)srcW;
    float scaleY = (float)lcdH / (float)srcH;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale <= 0.0f) scale = 1.0f;

    int dstW = (int)(srcW * scale);
    int dstH = (int)(srcH * scale);
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;
    if (dstW > lcdW) dstW = lcdW;
    if (dstH > lcdH) dstH = lcdH;

    s_transform.dstW    = dstW;
    s_transform.dstH    = dstH;
    s_transform.xOffset = (lcdW - dstW) / 2;
    s_transform.yOffset = (lcdH - dstH) / 2;

    build_bresenham_map(s_xMap, dstW, srcW);
    build_bresenham_map(s_yMap, dstH, srcH);
  }
}

static inline uint16_t avg565(uint16_t a, uint16_t b)
{
  uint32_t ar = a & 0xF81F;
  uint32_t br = b & 0xF81F;
  uint32_t ag = a & 0x07E0;
  uint32_t bg = b & 0x07E0;
  return (uint16_t)((((ar + br) >> 1) & 0xF81F) | (((ag + bg) >> 1) & 0x07E0));
}

static bool ensure_blend(int pixels)
{
  if (pixels <= 0) return false;
  if (s_blendPixels == pixels && s_prev && s_blend) return true;
  if (s_prev) { free(s_prev); s_prev = nullptr; }
  if (s_blend) { free(s_blend); s_blend = nullptr; }
  s_blendPixels = 0;
  s_prevValid = false;
  size_t bytes = (size_t)pixels * sizeof(uint16_t);
  s_prev = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s_blend = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_prev || !s_blend) {
    if (s_prev) { free(s_prev); s_prev = nullptr; }
    if (s_blend) { free(s_blend); s_blend = nullptr; }
    s_prev = (uint16_t*)malloc(bytes);
    s_blend = (uint16_t*)malloc(bytes);
  }
  if (!s_prev || !s_blend) {
    if (s_prev) { free(s_prev); s_prev = nullptr; }
    if (s_blend) { free(s_blend); s_blend = nullptr; }
    return false;
  }
  s_blendPixels = pixels;
  return true;
}

static const uint16_t* blend_frame(const uint16_t* fb, int pitch, int w, int h)
{
  if (!gbc_frame_blend || w <= 0 || h <= 0) {
    s_prevValid = false;
    return fb;
  }
  if (!ensure_blend(w * h)) return fb;

  if (pitch == w) {
    memcpy(s_blend, fb, (size_t)w * (size_t)h * sizeof(uint16_t));
  } else {
    for (int y = 0; y < h; ++y) {
      memcpy(s_blend + y * w, fb + y * pitch, (size_t)w * sizeof(uint16_t));
    }
  }
  if (!s_prevValid) {
    memcpy(s_prev, s_blend, (size_t)w * (size_t)h * sizeof(uint16_t));
    s_prevValid = true;
    return s_blend;
  }
  const int n = w * h;
  for (int i = 0; i < n; ++i) {
    uint16_t cur = s_blend[i];
    s_blend[i] = avg565(s_prev[i], cur);
    s_prev[i] = cur;
  }
  return s_blend;
}

static void paint_pixel(uint16_t* dst, uint16_t src)
{
  *dst = s_nightMode ? gbc_warm_rgb565(src) : src;
}

static void gbc_display_task(void *arg)
{
  (void)arg;

  for (;;) {
    GbcFrameMsg msg;
    if (xQueueReceive(s_frameQ, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (!msg.fb || msg.width <= 0 || msg.height <= 0 || msg.pitch <= 0) {
      continue;
    }

    const int srcW  = msg.width;
    const int srcH  = msg.height;
    const int pitch = msg.pitch;

    gbc_display_transform(srcW, srcH);

    const int dstW    = s_transform.dstW;
    const int dstH    = s_transform.dstH;
    const int xOffset = s_transform.xOffset;
    const int yOffset = s_transform.yOffset;
    const int iscale  = s_transform.intScale;

    if (dstW <= 0 || dstH <= 0) {
      continue;
    }

    const uint16_t* frame = blend_frame(msg.fb, pitch, srcW, srcH);
    const int framePitch = (frame == msg.fb) ? pitch : srcW;

    // Always use small DMA line batches — never a full-frame SPIRAM/DMA buffer.
    const int batchLines = kBatchLines;
    const int need = dstW * batchLines;

    if (need > s_lineCap) {
      if (s_lineBuf) {
        free(s_lineBuf);
        s_lineBuf = nullptr;
        s_lineCap = 0;
      }
      s_lineBuf = (uint16_t*)heap_caps_malloc(
          (size_t)need * sizeof(uint16_t),
          MALLOC_CAP_DMA | MALLOC_CAP_8BIT
      );
      if (!s_lineBuf) {
        s_lineBuf = (uint16_t*)heap_caps_malloc(
            (size_t)need * sizeof(uint16_t),
            MALLOC_CAP_8BIT
        );
      }
      s_lineCap = s_lineBuf ? need : 0;
    }

    if (!s_lineBuf || s_lineCap < need) {
      continue;
    }

    // Do not touch shared SPI while SaveTask holds the bus for SD.
    if (share::gameIsSaving()) {
      continue;
    }

    s_displayBusBusy = true;
    externalDisplay.startWrite();

    for (int y0 = 0; y0 < dstH; y0 += batchLines) {
      const int rows = (y0 + batchLines <= dstH) ? batchLines : (dstH - y0);
      for (int row = 0; row < rows; ++row) {
        const int y = y0 + row;
        uint16_t *dstLine = s_lineBuf + row * dstW;
        if (iscale > 0) {
          const uint16_t *srcLine = frame + (y / iscale) * framePitch;
          if (iscale == 1 && !s_nightMode) {
            memcpy(dstLine, srcLine, (size_t)dstW * sizeof(uint16_t));
          } else if (iscale == 1) {
            for (int x = 0; x < dstW; ++x) paint_pixel(&dstLine[x], srcLine[x]);
          } else {
            for (int x = 0; x < dstW; ++x) paint_pixel(&dstLine[x], srcLine[x / iscale]);
          }
        } else {
          const uint16_t *srcLine = frame + (int)s_yMap[y] * framePitch;
          if (s_nightMode) {
            for (int x = 0; x < dstW; ++x) {
              dstLine[x] = gbc_warm_rgb565(srcLine[s_xMap[x]]);
            }
          } else {
            for (int x = 0; x < dstW; ++x) {
              dstLine[x] = srcLine[s_xMap[x]];
            }
          }
        }
      }
      externalDisplay.setAddrWindow(xOffset, yOffset + y0, dstW, rows);
      externalDisplay.writePixels(s_lineBuf, dstW * rows);
      // No taskYIELD() inside startWrite — SaveTask must not grab SPI3 mid-frame.
    }

    externalDisplay.endWrite();
    s_displayBusBusy = false;
    taskYIELD(); // OK after releasing the bus
  }
}

extern "C" void gbc_display_init(void)
{
  externalDisplay.setSwapBytes(true);
  externalDisplay.fillScreen(TFT_BLACK);

  if (!s_frameQ) {
    // Length 1 + overwrite => always show newest frame (less text/scene lag)
    s_frameQ = xQueueCreate(1, sizeof(GbcFrameMsg));
    if (!s_frameQ) {
      EMU_LOG("[GBC-DISP] queue create failed\n");
    }
  }
}

extern "C" void gbc_display_start(void)
{
  if (!s_frameQ) {
    gbc_display_init();
  }
  if (!s_task && s_frameQ) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      gbc_display_task,
      "GbcDisp",
      4096,
      nullptr,
      5,   // below audio (prio 6) so blit never starves sound
      &s_task,
      0    // core 0
    );
    if (ok != pdPASS) {
      EMU_LOG("[GBC-DISP] task create failed\n");
      if (s_task) {
        vTaskDelete(s_task);
      }
      s_task = nullptr;
    }
  }
}

extern "C" void gbc_display_stop(void)
{
  if (s_task) {
    vTaskDelete(s_task);
    s_task = nullptr;
  }
  s_displayBusBusy = false;
  if (s_frameQ) {
    vQueueDelete(s_frameQ);
    s_frameQ = nullptr;
  }
  if (s_lineBuf) {
    free(s_lineBuf);
    s_lineBuf = nullptr;
    s_lineCap = 0;
  }
  if (s_prev) { free(s_prev); s_prev = nullptr; }
  if (s_blend) { free(s_blend); s_blend = nullptr; }
  s_blendPixels = 0;
  s_prevValid = false;
}

extern "C" const char* gbc_scale_name(void)
{
  return gbcFullScreen ? "Fill" : "Integer";
}

extern "C" const char* gbc_dmg_palette_name(void)
{
  int pal = (gbPalette >= 0) ? gbPalette : gbcDmgPalette;
  switch (pal) {
    case GB_PALETTE_DMG: return "BIOS DMG";
    case GB_PALETTE_MGB0: return "MGB 0";
    case GB_PALETTE_MGB1: return "MGB 1";
    case GB_PALETTE_CGB: return "CGB";
    case GB_PALETTE_SGB: return "SGB";
    case GB_PALETTE_POCKET: return "Pocket";
    case GB_PALETTE_GREY: return "Grey";
    case GB_PALETTE_LCD: return "LCD Green";
    case GB_PALETTE_LIGHT: return "GB Light";
    default: {
      static char buf[20];
      snprintf(buf, sizeof(buf), "Colorizer %d", pal);
      return buf;
    }
  }
}

static void gbc_hud(const char* text)
{
  auto& id = M5Cardputer.Display;
  id.fillScreen(TFT_BLACK);
  id.setTextDatum(middle_center);
  id.setTextColor(TFT_WHITE, TFT_BLACK);
  id.setTextSize(1);
  id.drawString(text, id.width() / 2, id.height() / 2);
}

extern "C" void gbc_toggle_scale_mode(void)
{
  gbcFullScreen = !gbcFullScreen;
  gbcZoomPercent = 100;
  gbc_settings_save();
  gbc_hud(gbcFullScreen ? "Scale: Fill" : "Scale: Integer");
}

extern "C" void gbc_toggle_frame_blend(void)
{
  gbc_frame_blend = gbc_frame_blend ? 0 : 1;
  s_prevValid = false;
  gbc_settings_save();
  gbc_hud(gbc_frame_blend ? "Blend: On" : "Blend: Off");
}

extern "C" void gbc_toggle_colour_correct(void)
{
  gbc_colour_correct = gbc_colour_correct ? 0 : 1;
  gb_lcd_pal_dirty();
  gbc_settings_save();
  gbc_hud(gbc_colour_correct ? "GBC color: On" : "GBC color: Off");
}

extern "C" void gbc_cycle_dmg_palette(void)
{
  int cur = (gbPalette >= 0) ? gbPalette : gbcDmgPalette;
  int idx = 0;
  for (int i = 0; i < kCuratedDmgCount; ++i) {
    if (kCuratedDmg[i] == cur) { idx = (i + 1) % kCuratedDmgCount; break; }
  }
  gbcDmgPalette = kCuratedDmg[idx];
  if (gbPalette >= 0) {
    gbPalette = gbcDmgPalette;
    gnuboy_set_palette((gb_palette_t)gbPalette);
  }
  gbc_settings_save();
  char msg[32];
  snprintf(msg, sizeof(msg), "DMG: %s", gbc_dmg_palette_name());
  gbc_hud(msg);
}


extern "C" int gbc_display_bus_busy(void)
{
  return s_displayBusBusy ? 1 : 0;
}

extern "C" void gbc_display_wait_idle(void)
{
  // Wait until GbcDisp finishes any in-flight startWrite..endWrite (~500ms max).
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
  while (s_displayBusBusy) {
    if (xTaskGetTickCount() >= deadline) {
      EMU_LOG("[GBC-DISP] wait_idle timeout (bus still busy)\n");
      break;
    }
    vTaskDelay(1);
  }
}

extern "C" void gbc_display_submit_frame(const uint16_t *fb,
                                         int pitch,
                                         int width,
                                         int height)
{
  if (!s_frameQ || !fb) return;

  GbcFrameMsg msg;
  msg.fb     = fb;
  msg.pitch  = pitch;
  msg.width  = width;
  msg.height = height;

  // Always keep the latest frame (drops stale ones instead of queuing lag)
  xQueueOverwrite(s_frameQ, &msg);
}
