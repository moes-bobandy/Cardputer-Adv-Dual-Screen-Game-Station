// gbc_sound.h
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int gbc_sampleRate;

void gbc_sound_init(int sample_rate);
void gbc_sound_set_volume(uint8_t vol);
void gbc_sound_shutdown(void);
void gbc_sound_submit(const int16_t* samples, size_t sample_count);
void gbc_audio_callback(void *buffer, size_t length);

#ifdef __cplusplus
}
#endif
