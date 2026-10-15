// Host timing of the EXT gather paths and a discrete model of the old
// audio feeder. This is not a Cardputer run: it times the same pixel work
// the display task does, on this machine, so before/after can be compared.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static constexpr int SRC_W = 160;
static constexpr int SRC_H = 144;
static constexpr int LCD_W = 320;
static constexpr int LCD_H = 240;

static uint16_t src[SRC_W * SRC_H];
static uint16_t dst[LCD_W * LCD_H];
static uint16_t xmap[LCD_W];
static uint16_t ymap[LCD_H];

static void bresenham(uint16_t* map, int d, int s) {
  int acc = 0, idx = 0;
  for (int i = 0; i < d; ++i) {
    map[i] = (uint16_t)idx;
    acc += s;
    while (acc >= d) {
      acc -= d;
      if (idx < s - 1) ++idx;
    }
  }
}

static void gather_map(int dstW, int dstH) {
  for (int y = 0; y < dstH; ++y) {
    const uint16_t* sl = src + (int)ymap[y] * SRC_W;
    uint16_t* dl = dst + y * dstW;
    for (int x = 0; x < dstW; ++x) dl[x] = sl[xmap[x]];
  }
}

static void gather_integer() {
  for (int y = 0; y < SRC_H; ++y) {
    memcpy(dst + y * SRC_W, src + y * SRC_W, SRC_W * sizeof(uint16_t));
  }
}

static inline uint16_t avg565(uint16_t a, uint16_t b) {
  uint32_t ar = a & 0xF81F, br = b & 0xF81F;
  uint32_t ag = a & 0x07E0, bg = b & 0x07E0;
  return (uint16_t)((((ar + br) >> 1) & 0xF81F) | (((ag + bg) >> 1) & 0x07E0));
}

static uint16_t prevf[SRC_W * SRC_H];
static uint16_t blend[SRC_W * SRC_H];

static void blend_frames() {
  for (int i = 0; i < SRC_W * SRC_H; ++i) {
    uint16_t c = src[i];
    blend[i] = avg565(prevf[i], c);
    prevf[i] = c;
  }
}

static void pal_plain(uint16_t* out, const uint16_t* colors) {
  for (int i = 0; i < 64; ++i) {
    int c = colors[i];
    int r = c & 0x1f;
    int g = (c >> 5) & 0x1f;
    int b = (c >> 10) & 0x1f;
    out[i] = (uint16_t)((r << 11) | (g << 6) | b);
  }
}

extern "C" const uint16_t gbc_cc_lut[32768];
static void pal_lut(uint16_t* out, const uint16_t* colors) {
  for (int i = 0; i < 64; ++i) out[i] = gbc_cc_lut[colors[i] & 0x7FFF];
}

static double ms_now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int uniform_blocks() {
  // 1x integer: each source pixel is exactly one dest pixel, centred.
  const int dstW = SRC_W, dstH = SRC_H;
  const int x0 = (LCD_W - dstW) / 2;
  const int y0 = (LCD_H - dstH) / 2;
  if (x0 != 80 || y0 != 48) return 1;
  if ((LCD_W - dstW) % 2 || (LCD_H - dstH) % 2) return 2;
  // Fit map must be nearest-neighbour and centred.
  float scale = (float)LCD_H / (float)SRC_H;
  int fw = (int)(SRC_W * scale);
  int fh = (int)(SRC_H * scale);
  if (fw != 266 || fh != 240) return 3;
  if (((LCD_W - fw) / 2) * 2 + fw != LCD_W && (LCD_W - fw) % 2) return 4;
  bresenham(xmap, fw, SRC_W);
  int counts[SRC_W];
  memset(counts, 0, sizeof(counts));
  for (int x = 0; x < fw; ++x) counts[xmap[x]]++;
  int minc = 99, maxc = 0;
  for (int i = 0; i < SRC_W; ++i) {
    if (counts[i] < minc) minc = counts[i];
    if (counts[i] > maxc) maxc = counts[i];
  }
  // Near-fullscreen NN duplicates some columns. Integer mode must not.
  printf("fit column span %d..%d (NN, not integer)\n", minc, maxc);
  if (minc < 1 || maxc > 3) return 5;
  return 0;
}

int main() {
  for (int i = 0; i < SRC_W * SRC_H; ++i) src[i] = (uint16_t)i;
  float scale = (float)LCD_H / (float)SRC_H; // limiting axis, old fullscreen
  int fw = (int)(SRC_W * scale);
  int fh = (int)(SRC_H * scale);
  bresenham(xmap, fw, SRC_W);
  bresenham(ymap, fh, SRC_H);

  const int N = 400;
  double t0 = ms_now();
  for (int i = 0; i < N; ++i) gather_map(fw, fh);
  double t_old = ms_now() - t0;

  t0 = ms_now();
  for (int i = 0; i < N; ++i) gather_integer();
  double t_int = ms_now() - t0;

  t0 = ms_now();
  for (int i = 0; i < N; ++i) gather_map(fw, fh);
  double t_fit = ms_now() - t0;

  t0 = ms_now();
  for (int i = 0; i < N; ++i) blend_frames();
  double t_blend = ms_now() - t0;

  uint16_t colors[64], out[64];
  for (int i = 0; i < 64; ++i) colors[i] = (uint16_t)(i * 500);
  const int P = 20000;
  t0 = ms_now();
  for (int i = 0; i < P; ++i) pal_plain(out, colors);
  double t_pal_off = ms_now() - t0;
  uint16_t sink = out[3];
  t0 = ms_now();
  for (int i = 0; i < P; ++i) pal_lut(out, colors);
  double t_pal_on = ms_now() - t0;
  sink ^= out[3];

  const double frame_ms = 1000.0 / 60.0;
  printf("pixels old fullscreen NN: %d\n", fw * fh);
  printf("pixels integer default:   %d\n", SRC_W * SRC_H);
  printf("pixels fill NN:           %d\n", fw * fh);
  printf("gather old NN:    %.4f ms/frame\n", t_old / N);
  printf("gather integer:   %.4f ms/frame\n", t_int / N);
  printf("gather fill NN:   %.4f ms/frame\n", t_fit / N);
  printf("blend (off path is 0): %.4f ms/frame when on\n", t_blend / N);
  printf("palette plain: %.4f us/sync\n", (t_pal_off / P) * 1000.0);
  printf("palette lut:   %.4f us/sync\n", (t_pal_on / P) * 1000.0);
  printf("frame budget:  %.3f ms\n", frame_ms);
  printf("integer gather is %.2fx the old fullscreen gather\n", (t_int / N) / (t_old / N));
  printf("geom %d sink %u\n", uniform_blocks(), sink);
  printf("lut black %04x white %04x\n", gbc_cc_lut[0], gbc_cc_lut[0x7FFF]);
  return uniform_blocks();
}
