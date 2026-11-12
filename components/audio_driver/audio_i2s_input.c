/*
 * INMP441 I2S MEMS microphone capture (JC4827W543 audio backend).
 *
 * Wiring: SCK=GPIO6, WS=GPIO7, SD=GPIO15, L/R tied to GND (left channel).
 * The mic is clocked at 16 kHz and decimated 2:1 to the 8 kHz PCM contract in
 * audio_driver.h, so RTP framing is unchanged. INMP441 delivers 24-bit data in
 * 32-bit slots; samples are shifted down to 16 bit before the DC blocker.
 *
 * These pins are free on the JC4827W543 according to the board headers, but
 * this backend has NOT been tested on hardware yet. If the mic stays silent,
 * check L/R, the 3.3 V supply and the bit-shift before suspecting the driver.
 */
#include "audio_priv.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO";

static i2s_chan_handle_t s_rx;
static StreamBufferHandle_t s_input_sb;
static TaskHandle_t s_input_task;
static volatile bool s_input_running;

static int32_t s_i2s_buf[AUDIO_I2S_READ_SAMPLES];
static int16_t s_pcm_buf[AUDIO_FRAME_SAMPLES];
static int32_t s_dc_estimate;
static bool s_dc_seeded;

static audio_level_acc_t s_input_level;
static int64_t s_input_meter_us;

static void audio_input_task(void *arg)
{
    (void)arg;

    while (true) {
        if (!s_input_running) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx, s_i2s_buf, sizeof(s_i2s_buf), &bytes_read,
                                         pdMS_TO_TICKS(AUDIO_I2S_TIMEOUT_MS));
        if (err != ESP_OK || bytes_read == 0) {
            /* Disabling the channel during a stop also aborts a pending read. */
            if (s_input_running) {
                ESP_LOGW(TAG, "[AUDIO] INMP441 read failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
        }

        size_t i2s_samples = bytes_read / sizeof(int32_t);
        size_t pcm_samples = i2s_samples / 2;
        if (pcm_samples > AUDIO_FRAME_SAMPLES) {
            pcm_samples = AUDIO_FRAME_SAMPLES;
        }

        /* 16 kHz -> 8 kHz with a two-tap average, then the same slow one-pole
         * DC tracker the XIAO PDM path used. A real anti-alias filter and
         * ESP-SR processing belong to the audio-quality phase. */
        for (size_t i = 0; i < pcm_samples; i++) {
            int32_t a = s_i2s_buf[2 * i] >> 16;
            int32_t b = s_i2s_buf[2 * i + 1] >> 16;
            int32_t mixed = (a + b) / 2;

            if (!s_dc_seeded) {
                s_dc_estimate = mixed;
                s_dc_seeded = true;
            }
            s_dc_estimate += (mixed - s_dc_estimate) / 256;
            s_pcm_buf[i] = (int16_t)(mixed - s_dc_estimate);
        }

        audio_process_apply_gain(s_pcm_buf, pcm_samples);
        audio_level_add(&s_input_level, s_pcm_buf, pcm_samples);

        size_t to_send = pcm_samples * sizeof(int16_t);
        size_t sent = xStreamBufferSend(s_input_sb, s_pcm_buf, to_send, 0);
        if (sent < to_send) {
            audio_stats_note_overrun();
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - s_input_meter_us >= AUDIO_METER_PERIOD_US) {
            s_input_meter_us = now_us;
            audio_level_log("Mic", &s_input_level);
        }
    }
}

esp_err_t audio_input_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 320; /* 20 ms per descriptor at 16 kHz */

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] Failed to create INMP441 RX channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_I2S_MIC_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = INMP441_SCK,
            .ws = INMP441_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = INMP441_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] INMP441 RX init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }

    s_input_sb = xStreamBufferCreate(AUDIO_INPUT_RING_BYTES, sizeof(int16_t));
    if (s_input_sb == NULL) {
        ESP_LOGE(TAG, "[AUDIO] Failed to create input ring buffer");
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(audio_input_task, "audio_rx", AUDIO_TASK_STACK, NULL,
                                AUDIO_TASK_PRIO, &s_input_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "[AUDIO] Failed to create capture task");
        vStreamBufferDelete(s_input_sb);
        s_input_sb = NULL;
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[AUDIO] INMP441 wiring: SCK=%d WS=%d SD=%d (L/R to GND)",
             INMP441_SCK, INMP441_WS, INMP441_SD);
    ESP_LOGW(TAG, "[AUDIO] INMP441 backend is not yet verified on hardware");
    return ESP_OK;
}

esp_err_t audio_input_start(void)
{
    if (s_rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_input_running) {
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] Failed to enable INMP441 RX: %s", esp_err_to_name(err));
        return err;
    }

    s_input_running = true;
    ESP_LOGI(TAG, "[AUDIO] Microphone: active (INMP441 %d Hz -> PCM %d Hz)",
             AUDIO_I2S_MIC_RATE_HZ, AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t audio_input_stop(void)
{
    if (s_rx == NULL || !s_input_running) {
        return ESP_OK;
    }

    s_input_running = false;
    esp_err_t err = i2s_channel_disable(s_rx);
    if (s_input_sb != NULL) {
        xStreamBufferReset(s_input_sb);
    }
    ESP_LOGI(TAG, "[AUDIO] Microphone: stopped");
    return err;
}

esp_err_t audio_read_pcm(int16_t *samples, size_t max_samples, size_t *samples_read)
{
    if (samples == NULL || samples_read == NULL || max_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *samples_read = 0;

    if (s_input_sb == NULL || !s_input_running) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t want_bytes = max_samples * sizeof(int16_t);
    size_t got_bytes = xStreamBufferReceive(s_input_sb, samples, want_bytes,
                                            pdMS_TO_TICKS(20));
    *samples_read = got_bytes / sizeof(int16_t);
    if (*samples_read == 0) {
        audio_stats_note_underrun();
    }
    return ESP_OK;
}
