// gbc_sound.cpp
#include "gbc_sound.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "share/emu_log_cpp.h"

static constexpr int kChannel = 0;

// ================== ETAT AUDIO ==================

static bool   s_inited     = false;
int    gbc_sampleRate = 44100;

static TaskHandle_t s_audioTask  = nullptr;
static volatile bool s_running   = false;

/* ~11 frames at 44100 Hz / 60. Emulator feeds, speaker task drains. */
static constexpr int kRingSamples = 8192;
static constexpr int kChunk       = 1024;
static constexpr int kSlots       = 3;
static constexpr int kPrefill     = 3000;
static int16_t* s_ring            = nullptr;
static int      s_ringSize        = 0;
static int      s_ringRead        = 0;
static int      s_ringWrite       = 0;
static int      s_ringCount       = 0;
// playRaw keeps the pointer. Three slots, and we only fill one when
// isPlaying() < 2, so the buffer handed to the speaker is never reused
// while that slot is still queued.
static int16_t* s_slot[kSlots]    = {};
static int      s_fill            = 0;
static int16_t  s_lastSample      = 0;
static bool     s_primed          = false;
static bool     s_inGap           = false;
static int      s_gapSamples      = 0;
static volatile int s_underruns   = 0;
static volatile int s_fillMinPct  = 100;
static volatile int s_fillMaxPct  = 0;
static volatile int s_fillShown   = 100;
static uint32_t s_playRate        = 44100;

// protection ring
static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

// ================== TASK AUDIO ==================

static void note_fill(int fill)
{
    int pct = 0;
    if (s_ringSize > 0) {
        if (fill < 0) fill = 0;
        if (fill > s_ringSize) fill = s_ringSize;
        pct = (fill * 100) / s_ringSize;
    }
    if (pct < s_fillMinPct) s_fillMinPct = pct;
    if (pct > s_fillMaxPct) s_fillMaxPct = pct;
}

/* Proportional trim around the prefill level, slewed a few hertz per chunk.
 * A ±0.5% step at the old 25/75 thresholds is an audible pitch jump; the
 * speaker applies sample_rate as a per-chunk resample ratio. */
static uint32_t paced_rate(int fill)
{
    const int nom = (gbc_sampleRate > 0) ? gbc_sampleRate : 44100;
    if (s_ringSize <= 0) return (uint32_t)nom;
    int err = fill - kPrefill;
    int goal = nom + (int)((int64_t)err * nom / (s_ringSize * 250));
    const int lo = nom - (nom / 250);
    const int hi = nom + (nom / 250);
    if (goal < lo) goal = lo;
    if (goal > hi) goal = hi;
    int cur = (int)s_playRate;
    if (cur < 8000 || cur > 96000) cur = nom;
    if (goal > cur + 4) goal = cur + 4;
    else if (goal < cur - 4) goal = cur - 4;
    s_playRate = (uint32_t)goal;
    return s_playRate;
}

/* Hold the last sample so a short gap stays continuous with the next real
 * sample. Ease off only after ~80 ms so a dead stream does not sit on DC. */
static void hold_last(int16_t* dst, int n)
{
    int32_t s = s_lastSample;
    for (int i = 0; i < n; ++i) {
        dst[i] = (int16_t)s;
        if (++s_gapSamples > 3500) s = (s * 15) / 16;
    }
    s_lastSample = (int16_t)s;
}

static void gbc_audio_task(void* arg)
{
    (void)arg;

    while (s_running) {
        if (!s_slot[0]) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int fill = s_ringCount;
        note_fill(fill);
        if (!s_primed) {
            if (fill < kPrefill) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            s_primed = true;
        }

        int queued = M5Cardputer.Speaker.isPlaying(kChannel);
        if (queued >= 2) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int16_t* dst = s_slot[s_fill];
        int toRead = 0;
        portENTER_CRITICAL(&s_ringMux);
        fill = s_ringCount;
        if (s_ringCount > 0) {
            int want = (queued == 0) ? s_ringCount : kChunk;
            if (want > kChunk) want = kChunk;
            toRead = (s_ringCount < want) ? s_ringCount : want;
            for (int i = 0; i < toRead; ++i) {
                dst[i] = s_ring[s_ringRead];
                s_ringRead++;
                if (s_ringRead >= s_ringSize) s_ringRead = 0;
            }
            s_ringCount -= toRead;
            s_lastSample = dst[toRead - 1];
            fill = s_ringCount;
        }
        note_fill(fill);
        portEXIT_CRITICAL(&s_ringMux);

        uint32_t rate = paced_rate(fill);
        if (toRead <= 0) {
            if (!s_inGap) {
                s_underruns++;
                s_inGap = true;
            }
            hold_last(dst, 256);
            toRead = 256;
        } else {
            s_inGap = false;
            s_gapSamples = 0;
        }

        M5Cardputer.Speaker.playRaw(
            dst,
            (size_t)toRead,
            rate,
            false, 1, kChannel, false
        );
        s_fill++;
        if (s_fill >= kSlots) s_fill = 0;
    }

    vTaskDelete(nullptr);
}


// ================== API ==================

extern "C" void gbc_sound_init(int sample_rate)
{
    if (s_inited) return;

    if (sample_rate > 0) {
        gbc_sampleRate = sample_rate;
    }

    auto cfg = M5Cardputer.Speaker.config();
    cfg.sample_rate       = gbc_sampleRate;
    cfg.stereo            = false;
    cfg.dma_buf_len       = 512;
    cfg.dma_buf_count     = 8;
    cfg.task_priority     = 4;
    cfg.task_pinned_core  = 1;
    M5Cardputer.Speaker.config(cfg);

    if (!M5Cardputer.Speaker.isRunning()) {
        M5Cardputer.Speaker.begin();
    }

    M5Cardputer.Speaker.setVolume(60);
    M5Cardputer.Speaker.stop(kChannel);

    // Ring buffer
    if (!s_ring) {
        s_ringSize  = kRingSamples;
        s_ring      = (int16_t*)malloc(s_ringSize * sizeof(int16_t));
        s_ringRead  = 0;
        s_ringWrite = 0;
        s_ringCount = 0;
    }
    bool slotsOk = (s_ring != nullptr);
    for (int i = 0; i < kSlots; ++i) {
        if (!s_slot[i]) {
            s_slot[i] = (int16_t*)malloc(kChunk * sizeof(int16_t));
        }
        if (!s_slot[i]) slotsOk = false;
    }
    s_fill = 0;
    s_lastSample = 0;
    s_primed = false;
    s_inGap = false;
    s_gapSamples = 0;
    s_underruns = 0;
    s_fillMinPct = 100;
    s_fillMaxPct = 0;
    s_fillShown = 100;
    s_playRate = (uint32_t)((gbc_sampleRate > 0) ? gbc_sampleRate : 44100);

    s_running = true;

    if (!s_audioTask && slotsOk) {
        BaseType_t ok = xTaskCreatePinnedToCore(
            gbc_audio_task,
            "gbc_audio",
            4096,
            nullptr,
            6,     // priority (above display prio 5)
            &s_audioTask,
            1      // core 1 — with Speaker DMA; avoid fighting display blit on core 0
        );
        if (ok != pdPASS) {
            EMU_LOG("[GBC][AUDIO] task create failed\n");
            s_audioTask = nullptr;
            s_running   = false;
        }
    }

    s_inited = slotsOk && (s_audioTask != nullptr);
    EMU_LOG("[GBC][AUDIO] init: rate=%d mono, ring=%d prefill=%d\n",
           gbc_sampleRate, s_ringSize, kPrefill);
}

extern "C" void gbc_sound_set_volume(uint8_t vol)
{
    M5Cardputer.Speaker.setVolume(vol);
}

extern "C" void gbc_sound_shutdown(void)
{
    if (!s_inited) return;

    s_running = false;

    if (s_audioTask) {
        vTaskDelay(pdMS_TO_TICKS(10));
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
    s_fill = 0;
    s_primed = false;
    s_inGap = false;
    s_gapSamples = 0;
    s_ringSize  = 0;
    s_ringRead  = 0;
    s_ringWrite = 0;
    s_ringCount = 0;

    s_inited = false;
}

// for the callback GNUBOY
extern "C" void gbc_sound_submit(const int16_t* samples, size_t sample_count)
{
    if (!s_inited || !samples || sample_count == 0 || !s_ring) {
        return;
    }

    portENTER_CRITICAL(&s_ringMux);

    int freeSpace = s_ringSize - s_ringCount;
    int toWrite   = (int)sample_count;
    if (toWrite > s_ringSize) {
        samples += toWrite - s_ringSize;
        toWrite = s_ringSize;
        freeSpace = s_ringSize;
        s_ringCount = 0;
        s_ringRead = s_ringWrite;
    }
    if (toWrite > freeSpace) {
        int drop = toWrite - freeSpace;
        s_ringRead += drop;
        if (s_ringRead >= s_ringSize) s_ringRead -= s_ringSize;
        s_ringCount -= drop;
    }

    for (int i = 0; i < toWrite; ++i) {
        s_ring[s_ringWrite] = samples[i];
        s_ringWrite++;
        if (s_ringWrite >= s_ringSize) s_ringWrite = 0;
    }
    s_ringCount += toWrite;
    note_fill(s_ringCount);

    portEXIT_CRITICAL(&s_ringMux);

    if (s_audioTask) xTaskNotifyGive(s_audioTask);
}

extern "C" int gbc_audio_underrun_count(void)
{
    return s_underruns;
}

extern "C" int gbc_audio_ring_min_pct(void)
{
    return s_fillShown;
}

extern "C" void gbc_audio_fill_take(int* min_pct, int* max_pct)
{
    int mn = s_fillMinPct;
    int mx = s_fillMaxPct;
    s_fillMinPct = 100;
    s_fillMaxPct = 0;
    s_fillShown = mn;
    if (min_pct) *min_pct = mn;
    if (max_pct) *max_pct = mx;
}