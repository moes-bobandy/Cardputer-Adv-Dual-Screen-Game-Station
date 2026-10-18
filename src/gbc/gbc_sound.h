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
/* Times the speaker needed samples and the ring was empty. */
int gbc_audio_underrun_count(void);
/* Lowest ring fill percent in the previous one-second window (0..100). */
int gbc_audio_ring_min_pct(void);
/* Read min/max fill since the previous take, then open a new window. */
void gbc_audio_fill_take(int* min_pct, int* max_pct);

#ifdef __cplusplus
}
#endif
