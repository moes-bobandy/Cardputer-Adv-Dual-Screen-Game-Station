#include "gbc_display.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include "esp_heap_caps.h"
extern "C" {
  #include "gnuboy/gnuboy.h"
#include "share/emu_log_cpp.h"
}

#include "../cardputer/CardputerView.h"
#include "share/game_save.h"
#include "gbc_sound.h"

extern LGFX_ILI9341 externalDisplay;

// Globals
bool gbcFullScreen = true;
int  gbcZoomPercent = 100;
int gbPalette = 36; // DMG default palette

typedef struct {
  const uint16_t *fb;
  int pitch;
  int width;
  int height;
} GbcFrameMsg;

static QueueHandle_t s_frameQ  = nullptr;
static TaskHandle_t  s_task    = nullptr;
static volatile bool s_displayBusBusy = false;
static const uint16_t* volatile s_reading = nullptr;
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

extern "C" volatile int gbc_fps_overlay = 1;
extern "C" volatile int gbc_fps_value = -1;

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

extern "C" void gbc_fps_toggle(void)
{
  gbc_fps_overlay = gbc_fps_overlay ? 0 : 1;
}

// 3x5 digits. Stamped into the existing line buffer (no extra SPI transaction).
static const uint8_t kFpsGlyph[10][5] = {
  {0x7,0x5,0x5,0x5,0x7},
  {0x2,0x6,0x2,0x2,0x7},
  {0x7,0x1,0x7,0x4,0x7},
  {0x7,0x1,0x7,0x1,0x7},
  {0x5,0x5,0x7,0x1,0x1},
  {0x7,0x4,0x7,0x1,0x7},
  {0x7,0x4,0x7,0x5,0x7},
  {0x7,0x1,0x1,0x1,0x1},
  {0x7,0x5,0x7,0x5,0x7},
  {0x7,0x5,0x7,0x1,0x7},
};

static const uint8_t kGlyphF[5] = {0x7,0x4,0x6,0x4,0x4};
static const uint8_t kGlyphU[5] = {0x5,0x5,0x5,0x5,0x7};
static const uint8_t kGlyphR[5] = {0x7,0x5,0x7,0x5,0x5};

static int paint_bits(uint16_t* line, int dstW, int x0, uint8_t bits)
{
  for (int b = 0; b < 3; ++b) {
    int x = x0 + b;
    if ((unsigned)x < (unsigned)dstW && (bits & (4 >> b))) line[x] = 0xFFFF;
  }
  return x0 + 4;
}

static void stamp_fps(uint16_t* line, int dstW, int y)
{
  if (y < 0 || y > 6 || dstW <= 0) return;
  int v = gbc_fps_value;
  if (v < 0) return;
  if (v > 999) v = 999;
  int u = gbc_audio_underrun_count();
  if (u < 0) u = 0;
  if (u > 999) u = 999;
  int r = gbc_audio_ring_min_pct();
  if (r < 0) r = 0;
  if (r > 100) r = 100;
  int nf = (v >= 100) ? 3 : (v >= 10) ? 2 : 1;
  int nu = (u >= 100) ? 3 : (u >= 10) ? 2 : 1;
  int nr = (r >= 100) ? 3 : (r >= 10) ? 2 : 1;
  int boxW = 4 * (1 + nf + 1 + 1 + nu + 1 + 1 + nr) + 1;
  if (boxW > dstW) boxW = dstW;
  for (int x = 0; x < boxW; ++x) line[x] = 0x0000;
  if (y < 1 || y > 5) return;
  int gy = y - 1;
  int x0 = 1;
  x0 = paint_bits(line, dstW, x0, kGlyphF[gy]);
  int fd[3] = { v / 100, (v / 10) % 10, v % 10 };
  for (int d = 3 - nf; d < 3; ++d) x0 = paint_bits(line, dstW, x0, kFpsGlyph[fd[d]][gy]);
  x0 += 4;
  x0 = paint_bits(line, dstW, x0, kGlyphU[gy]);
  int ud[3] = { u / 100, (u / 10) % 10, u % 10 };
  for (int d = 3 - nu; d < 3; ++d) x0 = paint_bits(line, dstW, x0, kFpsGlyph[ud[d]][gy]);
  x0 += 4;
  x0 = paint_bits(line, dstW, x0, kGlyphR[gy]);
  int rd[3] = { r / 100, (r / 10) % 10, r % 10 };
  for (int d = 3 - nr; d < 3; ++d) x0 = paint_bits(line, dstW, x0, kFpsGlyph[rd[d]][gy]);
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

  // ---- 100% zoom: aspect-correct letterbox, Bresenham maps ----
  // Fullscreen and windowed both show the entire 160x144 frame (no crop).
  // scale = min(lcdW/srcW, lcdH/srcH); center with black bars.
  {
    float scaleX = (float)lcdW / (float)srcW;
    float scaleY = (float)lcdH / (float)srcH;
    float scale  = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale <= 0.0f) scale = 1.0f;

    // Non-fullscreen: keep integer max scale (usually 1x) when it fits.
    if (!full) {
      int iscaleX = lcdW / srcW;
      int iscaleY = lcdH / srcH;
      int iscale  = (iscaleX < iscaleY) ? iscaleX : iscaleY;
      if (iscale < 1) iscale = 1;
      scale = (float)iscale;
    }

    int dstW = (int)(srcW * scale);
    int dstH = (int)(srcH * scale);
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;
    if (dstW > lcdW) dstW = lcdW;
    if (dstH > lcdH) dstH = lcdH;

    const int xOffset = (lcdW - dstW) / 2;
    const int yOffset = (lcdH - dstH) / 2;

    s_transform.dstW    = dstW;
    s_transform.dstH    = dstH;
    s_transform.xOffset = xOffset;
    s_transform.yOffset = yOffset;

    build_bresenham_map(s_xMap, dstW, srcW);
    build_bresenham_map(s_yMap, dstH, srcH);
  }
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

    if (dstW <= 0 || dstH <= 0) {
      continue;
    }

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

    /* Held for the whole push, including writePixels waits that let the
     * emulator run the next frame on this core. */
    s_reading = msg.fb;
    s_displayBusBusy = true;
    externalDisplay.startWrite();

    for (int y0 = 0; y0 < dstH; y0 += batchLines) {
      const int rows = (y0 + batchLines <= dstH) ? batchLines : (dstH - y0);
      for (int row = 0; row < rows; ++row) {
        const int y = y0 + row;
        const uint16_t *srcLine = msg.fb + (int)s_yMap[y] * pitch;
        uint16_t *dstLine = s_lineBuf + row * dstW;
        if (s_nightMode) {
          for (int x = 0; x < dstW; ++x) {
            dstLine[x] = gbc_warm_rgb565(srcLine[s_xMap[x]]);
          }
        } else {
          for (int x = 0; x < dstW; ++x) {
            dstLine[x] = srcLine[s_xMap[x]];
          }
        }
        if (gbc_fps_overlay && y < 7) stamp_fps(dstLine, dstW, y);
      }
      externalDisplay.setAddrWindow(xOffset, yOffset + y0, dstW, rows);
      externalDisplay.writePixels(s_lineBuf, dstW * rows);
      // No taskYIELD() inside startWrite — SaveTask must not grab SPI3 mid-frame.
    }

    externalDisplay.endWrite();
    s_reading = nullptr;
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
}


extern "C" const uint16_t* gbc_display_reading(void)
{
  return s_reading;
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
