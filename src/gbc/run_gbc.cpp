#include <Arduino.h>
extern "C" {
  #include "gnuboy/gnuboy.h"
  #include "gnuboy/lcd.h"
}

#include "gbc_display.h"
#include "gbc_sound.h"
#include "gbc_input.h"
#include "gbc_save.h"
#include "gbc_cheats.h"
#include "share/utils.h"
#include "share/game_save.h"
#include "share/emu_log_cpp.h"

static uint16_t* s_gbFramebuf = nullptr;
static uint16_t* s_fb[2] = {nullptr, nullptr};
static int16_t* s_audioBuf = nullptr;
extern int gbc_sampleRate;

// callback video GNUBOY
static void gbc_video_callback(void *buffer)
{
    uint16_t* shown = (uint16_t*)buffer;
    gbc_display_submit_frame(shown, GB_WIDTH, GB_WIDTH, GB_HEIGHT);
    if (s_fb[0] && s_fb[1]) {
        uint16_t* next = (shown == s_fb[0]) ? s_fb[1] : s_fb[0];
        gnuboy_set_framebuffer(next);
    }
}

// callback audio GNUBOY
void gbc_audio_callback(void *buffer, size_t length)
{
    if (!buffer || length == 0) return;

    gbc_sound_submit((const int16_t*)buffer, length);
}

void gbc_allocate_buffers() {
    size_t fbSize = GB_WIDTH * GB_HEIGHT * sizeof(uint16_t);
    for (int i = 0; i < 2; ++i) {
        s_fb[i] = (uint16_t*)heap_caps_malloc(
            fbSize,
            MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
        );
        if (!s_fb[i]) s_fb[i] = (uint16_t*)malloc(fbSize);
        if (!s_fb[i]) {
            EMU_LOG("[GBC] FATAL: framebuffer alloc failed\n");
            abort();
        }
        memset(s_fb[i], 0, fbSize);
    }
    s_gbFramebuf = s_fb[0];

    // buffer audio GNUBOY
    s_audioBuf = (int16_t*)heap_caps_malloc(
        2048 * sizeof(int16_t),
        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL
    );
    if (!s_audioBuf) {
        EMU_LOG("[GBC] FATAL: audio buffer alloc failed\n");
        abort();
    }
    memset(s_audioBuf, 0, 2048 * sizeof(int16_t));

}

bool isGbcGame(const char* filename) {
    if (!filename) return false;

    size_t len = strlen(filename);
    if (len < 4) return false;

    const char* ext = filename + (len - 4);

    return (strcasecmp(ext, ".gbc") == 0);
}

void run_gbc(const uint8_t* romData, size_t romLen, const char* romPathOrName) {
    // Audio Video — init LCD early for fillScreen; start GbcDisp task after SRAM load
    // so the first SD save load cannot race shared SPI3 with the display task.
    gbc_display_init();
    gbc_sound_init(gbc_sampleRate);
    gbc_allocate_buffers();
    
    // init core GNUBOY
    int ret = gnuboy_init(
        gbc_sampleRate,
        GB_AUDIO_MONO_S16,
        GB_PIXEL_565_LE,
        &gbc_video_callback,
        &gbc_audio_callback
    );
    EMU_LOG("[GBC] gnuboy_init => %d\n", ret);
    if (ret < 0) return;

    // buffers
    gnuboy_set_framebuffer(s_gbFramebuf);
    gnuboy_set_soundbuffer(s_audioBuf, 2048);

    // load ROM
    ret = gnuboy_load_rom(romData, romLen);
    EMU_LOG("[GBC] gnuboy_load_rom => %d\n", ret);
    if (ret < 0) return;

    gnuboy_reset(true);
    gb_lcd_rebuild_palette();
    gnuboy_set_pad(0);
    gbc_save_init(romPathOrName);
    gbc_save_load();  // SPI-safe; GbcDisp not started yet
    gbc_cheats_init(romPathOrName);
    gbc_cheats_load();  // optional /sd/gbc_cheats/<base>.cht; OK if missing
    gbc_display_start();
    
    // FPS counter
    uint32_t frameCount = 0;
    uint32_t lastFpsMs  = millis();

    // Pacing 60 Hz
    const int targetFps       = 60;
    const uint32_t frame_us   = 1000000u / (uint32_t)targetFps; // 16.6 ms
    uint64_t next_frame_us    = esp_timer_get_time();
    bool drawFrame            = true; 
    bool gbcGame              = isGbcGame(romPathOrName);
    gbPalette                 = gbcGame ? -1 : gbPalette;
    int lastPalette           = -1;

    EMU_LOG("Palette initiale: %d\n", gbPalette);

    EMU_LOG("[GBC] starting main loop @ %d FPS\n", targetFps);

    while (true) {
        if (share::gameIsSaving()) {
            delay(1);
            next_frame_us = esp_timer_get_time();
            continue;
        }

        if (!gbcGame && lastPalette != gbPalette) {
            lastPalette = gbPalette;
            gnuboy_set_palette((gb_palette_t)gbPalette);
        }

        // Cheat menu pauses emu (no gnuboy_run while open)
        if (gbc_cheats_menu_requested()) {
            /* Persist any pending in-game SRAM before a long pause. */
            gbc_save_force_flush();
            gbc_cheats_run_menu();
            next_frame_us = esp_timer_get_time();
            continue;
        }

        // Always emulate. drawFrame only skips the LCD callback, and only
        // after the loop has fallen more than one frame behind.
        gnuboy_run(drawFrame);

        gbc_cheats_apply();

        int pad = gbc_input_poll();
        if (pad >= 0) {
            gnuboy_set_pad(pad);
        }

        gbc_save_tick();

        // FPS log
        frameCount++;
        uint32_t nowMs = millis();
        if (nowMs - lastFpsMs >= 1000) {
            float fps = (frameCount * 1000.0f) / (nowMs - lastFpsMs);
            int shown = (int)(fps + 0.5f);
            if (shown < 0) shown = 0;
            if (shown > 999) shown = 999;
            gbc_fps_value = shown;
            EMU_LOG("[GBC] FPS: %.2f | HEAP %u\n", fps, esp_get_free_heap_size());
            frameCount = 0;
            lastFpsMs  = nowMs;
        }

        // Pacing
        next_frame_us += frame_us;
        int64_t now = (int64_t)esp_timer_get_time();
        int64_t lateness = now - (int64_t)next_frame_us;

        if (lateness > (int64_t)frame_us) {
            // More than one frame late: skip the next LCD push only.
            drawFrame = false;
            if (lateness > (int64_t)frame_us * 4) {
                next_frame_us = (uint64_t)now;
            }
        } else if (lateness > 0) {
            // Behind, but under one frame: keep drawing, do not sleep.
            drawFrame = true;
        } else {
            drawFrame = true;
            share::sleep_until_us(next_frame_us);
        }
    }
}