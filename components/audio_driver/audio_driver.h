/*
 * Audio capture (PDM mic) and playback (I2S TX) for G.711 narrowband audio.
 *
 * Implemented behaviour:
 *   - PDM RX at 16 kHz on PDM_DATA/PDM_CLK (GPIO41/42), decimated 2:1 to the
 *     8 kHz PCM the RTP layer will encode.
 *   - I2S standard TX at 8 kHz, mono 16-bit, on I2S_BCLK/LRC/DOUT (GPIO8/9/7).
 *   - Capture and playback each run in a FreeRTOS task with a stream-buffer
 *     ring; every I2S read/write is bounded so SIP work cannot stall audio.
 *   - Software mic gain and speaker volume, plus a 1 Hz serial meter showing
 *     RMS and peak dBFS per stream.
 *   - Never start the SD SPI driver: GPIO7/8/9 are shared with the microSD.
 *
 * audio_driver_init() creates both channels; audio_input_start() and
 * audio_output_start() enable them. audio_read_pcm() waits at most 20 ms for a
 * frame, audio_write_pcm() never blocks and drops frames when the ring is full.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Board wiring, fixed by the project requirements. */
#define I2S_DOUT 7
#define I2S_BCLK 8
#define I2S_LRC  9

#define PDM_DATA 41
#define PDM_CLK  42

/* G.711 framing: 8 kHz mono, 20 ms packets. */
#define AUDIO_SAMPLE_RATE_HZ  8000
#define AUDIO_CHANNELS        1
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_PACKET_MS       20
#define AUDIO_FRAME_SAMPLES   ((AUDIO_SAMPLE_RATE_HZ / 1000) * AUDIO_PACKET_MS) /* 160 */
#define AUDIO_FRAME_BYTES     (AUDIO_FRAME_SAMPLES) /* G.711: one byte per sample */

#define AUDIO_RING_BUFFER_FRAMES  8
#define AUDIO_DEFAULT_MIC_GAIN_DB 0
#define AUDIO_DEFAULT_VOLUME      80

typedef enum {
    AUDIO_CODEC_PCMU = 0,
    AUDIO_CODEC_PCMA,
} audio_codec_t;

esp_err_t audio_driver_init(void);

/* Capture path (PDM -> PCM) */
esp_err_t audio_input_start(void);
esp_err_t audio_input_stop(void);

/* Playback path (PCM -> I2S) */
esp_err_t audio_output_start(void);
esp_err_t audio_output_stop(void);

/* Reads up to max_samples mono samples at AUDIO_SAMPLE_RATE_HZ. */
esp_err_t audio_read_pcm(int16_t *samples, size_t max_samples, size_t *samples_read);

/* Writes mono samples to the I2S output ring buffer without blocking. */
esp_err_t audio_write_pcm(const int16_t *samples, size_t sample_count);

esp_err_t audio_set_mic_gain_db(int8_t gain_db);
esp_err_t audio_set_volume(uint8_t volume_0_100);

/* Diagnostics for the serial log / web dashboard. */
esp_err_t audio_get_buffer_stats(uint32_t *underruns, uint32_t *overruns);
