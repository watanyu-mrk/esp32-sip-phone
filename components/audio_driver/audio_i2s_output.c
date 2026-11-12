/*
 * Onboard I2S amplifier playback (JC4827W543 audio backend).
 *
 * Wiring assumed from the project brief: BCLK=GPIO42, LRC=GPIO2, DOUT=GPIO41.
 * The amplifier part number and these pins are NOT confirmed by a schematic in
 * this repository, so this is the first thing to check during bring-up.
 *
 * The amplifier has no documented enable/mute GPIO on this board, so none is
 * driven here; mute is done by not writing samples (the DMA auto-clears).
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

static i2s_chan_handle_t s_tx;
static StreamBufferHandle_t s_output_sb;
static TaskHandle_t s_output_task;
static volatile bool s_output_running;

static int16_t s_out_buf[AUDIO_FRAME_SAMPLES];
static audio_level_acc_t s_output_level;
static int64_t s_output_meter_us;

static void audio_output_task(void *arg)
{
    (void)arg;

    while (true) {
        if (!s_output_running) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t got_bytes = xStreamBufferReceive(s_output_sb, s_out_buf, sizeof(s_out_buf),
                                                pdMS_TO_TICKS(20));
        if (got_bytes == 0) {
            audio_stats_note_underrun();
            continue;
        }

        size_t samples = got_bytes / sizeof(int16_t);
        audio_process_apply_volume(s_out_buf, samples);
        audio_level_add(&s_output_level, s_out_buf, samples);

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_out_buf, samples * sizeof(int16_t),
                                          &bytes_written,
                                          pdMS_TO_TICKS(AUDIO_I2S_TIMEOUT_MS));
        if (err != ESP_OK || bytes_written < samples * sizeof(int16_t)) {
            audio_stats_note_underrun();
            if (s_output_running) {
                ESP_LOGW(TAG, "[AUDIO] Amplifier I2S write failed/short: %s",
                         esp_err_to_name(err));
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - s_output_meter_us >= AUDIO_METER_PERIOD_US) {
            s_output_meter_us = now_us;
            audio_level_log("Speaker", &s_output_level);
        }
    }
}

esp_err_t audio_output_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 3;
    chan_cfg.dma_frame_num = 160; /* 20 ms per descriptor at 8 kHz */
    /* Do not replay stale DMA contents after an underrun. */
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] Failed to create amplifier TX channel: %s",
                 esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AMP_BCLK,
            .ws = AMP_LRC,
            .dout = AMP_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] Amplifier I2S TX init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }

    s_output_sb = xStreamBufferCreate(AUDIO_OUTPUT_RING_BYTES, sizeof(int16_t));
    if (s_output_sb == NULL) {
        ESP_LOGE(TAG, "[AUDIO] Failed to create output ring buffer");
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(audio_output_task, "audio_tx", AUDIO_TASK_STACK, NULL,
                                AUDIO_TASK_PRIO, &s_output_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "[AUDIO] Failed to create playback task");
        vStreamBufferDelete(s_output_sb);
        s_output_sb = NULL;
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[AUDIO] Amplifier wiring: BCLK=%d LRC=%d DOUT=%d",
             AMP_BCLK, AMP_LRC, AMP_DOUT);
    return ESP_OK;
}

esp_err_t audio_output_start(void)
{
    if (s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_output_running) {
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] Failed to enable amplifier TX: %s", esp_err_to_name(err));
        return err;
    }

    s_output_running = true;
    ESP_LOGI(TAG, "[AUDIO] Speaker: active (I2S %d Hz mono)", AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t audio_output_stop(void)
{
    if (s_tx == NULL || !s_output_running) {
        return ESP_OK;
    }

    s_output_running = false;
    esp_err_t err = i2s_channel_disable(s_tx);
    if (s_output_sb != NULL) {
        xStreamBufferReset(s_output_sb);
    }
    ESP_LOGI(TAG, "[AUDIO] Speaker: stopped");
    return err;
}

esp_err_t audio_write_pcm(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_output_sb == NULL || !s_output_running) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes = sample_count * sizeof(int16_t);
    size_t sent = xStreamBufferSend(s_output_sb, samples, bytes, 0);
    if (sent < bytes) {
        /* Never block the RTP path: drop the remainder and count the overrun. */
        audio_stats_note_overrun();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
