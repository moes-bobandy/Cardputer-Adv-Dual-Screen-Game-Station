// gbc_sound.cpp
#include "gbc_sound.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "share/emu_log_cpp.h"

static constexpr int kChannel = 0;

// M5Unified Speaker::playRaw does not copy PCM. It keeps the pointer until
// that slot finishes (two slots per channel). The old feeder passed stack
// buffers and reused them on the next chunk, so the I2S task read torn
// memory at every chunk boundary (~11 ms, about once a frame). It also
// popped the ring before checking the slot count and dropped the chunk
// when both slots were full, then spun at a priority above the speaker
// task so the DMA could not be refilled.

static bool   s_inited     = false;
int    gbc_sampleRate = 44100;

static TaskHandle_t s_audioTask  = nullptr;
static volatile bool s_running   = false;

static constexpr int kRingSamples = 4096;
static constexpr int kChunk = 1024;
static constexpr int kSlots = 3;
static int16_t* s_ring            = nullptr;
static int      s_ringSize        = 0;
static int      s_ringRead        = 0;
static int      s_ringWrite       = 0;
static int      s_ringCount       = 0;
static int16_t* s_slot[kSlots]    = {};
static int      s_fill            = 0;

static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

static int ring_count(void) {
  portENTER_CRITICAL(&s_ringMux);
  int n = s_ringCount;
  portEXIT_CRITICAL(&s_ringMux);
  return n;
}

static int ring_read(int16_t* dst, int maxn) {
  portENTER_CRITICAL(&s_ringMux);
  int n = s_ringCount < maxn ? s_ringCount : maxn;
  if (n > 0 && s_ring) {
    int first = s_ringSize - s_ringRead;
    if (first > n) first = n;
    memcpy(dst, s_ring + s_ringRead, (size_t)first * sizeof(int16_t));
    if (n > first) {
      memcpy(dst + first, s_ring, (size_t)(n - first) * sizeof(int16_t));
    }
    s_ringRead += n;
    if (s_ringRead >= s_ringSize) s_ringRead -= s_ringSize;
    s_ringCount -= n;
  } else {
    n = 0;
  }
  portEXIT_CRITICAL(&s_ringMux);
  return n;
}

static void gbc_audio_task(void* arg)
{
  (void)arg;

  while (s_running) {
    // 0 = idle, 1 = one slot free, 2 = both speaker slots busy.
    size_t queued = M5Cardputer.Speaker.isPlaying(kChannel);
    int have = ring_count();

    int want = 0;
    if (queued >= 2 || have <= 0) {
      want = 0;
    } else if (queued == 0 || have >= 256) {
      want = have < kChunk ? have : kChunk;
    }

    if (want <= 0) {
      // Block so the lower-priority speaker task (and the display task)
      // can run. A notify from submit wakes this as soon as samples land.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(4));
      continue;
    }

    int16_t* dst = s_slot[s_fill];
    int got = ring_read(dst, want);
    if (got <= 0) continue;

    M5Cardputer.Speaker.playRaw(
        dst,
        (size_t)got,
        (uint32_t)gbc_sampleRate,
        false, 1, kChannel, false);
    s_fill++;
    if (s_fill >= kSlots) s_fill = 0;
  }

  s_audioTask = nullptr;
  vTaskDelete(nullptr);
}

extern "C" void gbc_sound_init(int sample_rate)
{
  if (s_inited) return;

  if (sample_rate > 0) {
    gbc_sampleRate = sample_rate;
  }

  // begin() ignores config once the task is up. M5Cardputer.begin() already
  // started the speaker at priority 2, which loses to both the emulator
  // (priority 19, core 1) and the display blit (priority 5). Restart so the
  // DMA depth and the core-0 priority actually apply. Volume stays 60.
  if (M5Cardputer.Speaker.isRunning()) {
    M5Cardputer.Speaker.end();
  }

  auto cfg = M5Cardputer.Speaker.config();
  cfg.sample_rate       = gbc_sampleRate;
  cfg.stereo            = false;
  cfg.dma_buf_len       = 512;
  cfg.dma_buf_count     = 8;
  cfg.task_priority     = 6;   // above GbcDisp (5), below this feeder (7)
  cfg.task_pinned_core  = 0;   // not on the emulator core
  M5Cardputer.Speaker.config(cfg);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(60);
  M5Cardputer.Speaker.stop(kChannel);

  if (!s_ring) {
    s_ringSize  = kRingSamples;
    s_ring      = (int16_t*)malloc(s_ringSize * sizeof(int16_t));
    s_ringRead  = 0;
    s_ringWrite = 0;
    s_ringCount = 0;
  }
  bool slotsOk = s_ring != nullptr;
  for (int i = 0; i < kSlots; ++i) {
    if (!s_slot[i]) {
      s_slot[i] = (int16_t*)malloc(kChunk * sizeof(int16_t));
    }
    if (!s_slot[i]) slotsOk = false;
  }
  s_fill = 0;

  s_running = true;
  if (slotsOk && !s_audioTask) {
    BaseType_t ok = xTaskCreatePinnedToCore(
        gbc_audio_task,
        "gbc_audio",
        4096,
        nullptr,
        7,     // above speaker (6) and display (5); blocks when the queue is full
        &s_audioTask,
        0      // core 0 — leave core 1 to the emulator
    );
    if (ok != pdPASS) {
      EMU_LOG("[GBC][AUDIO] task create failed\n");
      s_audioTask = nullptr;
      s_running   = false;
    }
  } else if (!slotsOk) {
    s_running = false;
    EMU_LOG("[GBC][AUDIO] buffer alloc failed\n");
  }

  s_inited = slotsOk && s_running;
  EMU_LOG("[GBC][AUDIO] init: rate=%d mono, ring=%d, slots=%d x %d, dma=%u x %u core0 prio6\n",
         gbc_sampleRate, s_ringSize, kSlots, kChunk,
         (unsigned)cfg.dma_buf_len, (unsigned)cfg.dma_buf_count);
}

extern "C" void gbc_sound_set_volume(uint8_t vol)
{
  M5Cardputer.Speaker.setVolume(vol);
}

extern "C" void gbc_sound_shutdown(void)
{
  if (!s_inited && !s_audioTask) return;

  s_running = false;
  if (s_audioTask) {
    xTaskNotifyGive(s_audioTask);
    for (int i = 0; i < 50 && s_audioTask; ++i) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    s_audioTask = nullptr;
  }

  M5Cardputer.Speaker.stop(kChannel);

  if (s_ring) {
    free(s_ring);
    s_ring = nullptr;
  }
  for (int i = 0; i < kSlots; ++i) {
    if (s_slot[i]) {
      free(s_slot[i]);
      s_slot[i] = nullptr;
    }
  }
  s_ringSize  = 0;
  s_ringRead  = 0;
  s_ringWrite = 0;
  s_ringCount = 0;
  s_fill = 0;
  s_inited = false;
}

extern "C" void gbc_sound_submit(const int16_t* samples, size_t sample_count)
{
  if (!s_inited || !samples || sample_count == 0 || !s_ring) {
    return;
  }

  portENTER_CRITICAL(&s_ringMux);

  int freeSpace = s_ringSize - s_ringCount;
  int toWrite   = (sample_count <= (size_t)freeSpace)
                  ? (int)sample_count
                  : freeSpace;
  if (toWrite > 0) {
    int first = s_ringSize - s_ringWrite;
    if (first > toWrite) first = toWrite;
    memcpy(s_ring + s_ringWrite, samples, (size_t)first * sizeof(int16_t));
    if (toWrite > first) {
      memcpy(s_ring, samples + first, (size_t)(toWrite - first) * sizeof(int16_t));
    }
    s_ringWrite += toWrite;
    if (s_ringWrite >= s_ringSize) s_ringWrite -= s_ringSize;
    s_ringCount += toWrite;
  }

  portEXIT_CRITICAL(&s_ringMux);

  if (s_audioTask) {
    xTaskNotifyGive(s_audioTask);
  }
}
