/*
 * Internal audio driver plumbing shared by audio_input.c, audio_output.c and
 * audio_process.c. Not part of the public API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_driver.h"

/* Worker task sizing: audio outranks SIP/network so the I2S DMA never starves. */
#define AUDIO_TASK_STACK 4096
#define AUDIO_TASK_PRIO  8

/* 20 ms of PDM samples at 16 kHz = 320 mono samples. */
#define AUDIO_PDM_SAMPLE_RATE_HZ (AUDIO_SAMPLE_RATE_HZ * 2)
#define AUDIO_PDM_READ_SAMPLES   (AUDIO_FRAME_SAMPLES * 2)

/* Ring depths in 20 ms frames. */
#define AUDIO_INPUT_RING_BYTES \
    (AUDIO_FRAME_SAMPLES * sizeof(int16_t) * AUDIO_RING_BUFFER_FRAMES)
#define AUDIO_OUTPUT_RING_BYTES \
    (AUDIO_FRAME_SAMPLES * sizeof(int16_t) * AUDIO_RING_BUFFER_FRAMES)

/* Every I2S wait is bounded: audio must never block SIP or the network. */
#define AUDIO_I2S_TIMEOUT_MS 50

/* One meter line per second per stream. */
#define AUDIO_METER_PERIOD_US 1000000LL
#define AUDIO_METER_BARS      20

/* Accumulates RMS/peak between meter lines. */
typedef struct {
    uint64_t sum_sq;
    uint32_t count;
    uint32_t peak;
} audio_level_acc_t;

esp_err_t audio_input_init(void);
esp_err_t audio_output_init(void);

void audio_process_apply_gain(int16_t *samples, size_t count);
void audio_process_apply_volume(int16_t *samples, size_t count);

void audio_level_add(audio_level_acc_t *acc, const int16_t *samples, size_t count);
/* Logs RMS/peak dBFS with a bar graph, then resets the accumulator. */
void audio_level_log(const char *label, audio_level_acc_t *acc);

void audio_stats_note_underrun(void);
void audio_stats_note_overrun(void);
