#include "audio_priv.h"

#include <math.h>

#include "config_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "AUDIO";

static volatile int8_t s_mic_gain_db;
static volatile uint8_t s_volume;
static float s_gain_linear = 1.0f;
static float s_volume_linear = 1.0f;

static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_underruns;
static uint32_t s_overruns;

static float db_to_linear(int8_t db)
{
    return powf(10.0f, (float)db / 20.0f);
}

esp_err_t audio_driver_init(void)
{
    if (config_manager_is_ready()) {
        const phone_config_t *cfg = config_manager_get();
        s_mic_gain_db = cfg->audio.mic_gain_db;
        s_volume = (cfg->audio.speaker_volume > 100) ? 100 : cfg->audio.speaker_volume;
    } else {
        /* Configuration never loaded: fall back to build-time defaults. */
        s_mic_gain_db = AUDIO_DEFAULT_MIC_GAIN_DB;
        s_volume = AUDIO_DEFAULT_VOLUME;
    }
    s_gain_linear = db_to_linear(s_mic_gain_db);
    s_volume_linear = (float)s_volume / 100.0f;

    esp_err_t err = audio_input_init();
    if (err != ESP_OK) {
        return err;
    }

    err = audio_output_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[AUDIO] Output init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "[AUDIO] Drivers ready (mic gain %d dB, volume %u%%)",
             s_mic_gain_db, (unsigned)s_volume);
    return ESP_OK;
}

void audio_process_apply_gain(int16_t *samples, size_t count)
{
    const float gain = s_gain_linear;
    if (gain == 1.0f) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        int32_t v = (int32_t)lrintf((float)samples[i] * gain);
        if (v > INT16_MAX) {
            v = INT16_MAX;
        } else if (v < INT16_MIN) {
            v = INT16_MIN;
        }
        samples[i] = (int16_t)v;
    }
}

void audio_process_apply_volume(int16_t *samples, size_t count)
{
    const float scale = s_volume_linear;
    if (scale == 1.0f) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        int32_t v = (int32_t)lrintf((float)samples[i] * scale);
        if (v > INT16_MAX) {
            v = INT16_MAX;
        } else if (v < INT16_MIN) {
            v = INT16_MIN;
        }
        samples[i] = (int16_t)v;
    }
}

esp_err_t audio_set_mic_gain_db(int8_t gain_db)
{
    if (gain_db < -24 || gain_db > 24) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mic_gain_db = gain_db;
    s_gain_linear = db_to_linear(gain_db);
    ESP_LOGI(TAG, "[AUDIO] Mic gain set to %d dB", gain_db);
    return ESP_OK;
}

esp_err_t audio_set_volume(uint8_t volume_0_100)
{
    if (volume_0_100 > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_volume = volume_0_100;
    s_volume_linear = (float)volume_0_100 / 100.0f;
    ESP_LOGI(TAG, "[AUDIO] Volume set to %u%%", (unsigned)volume_0_100);
    return ESP_OK;
}

void audio_level_add(audio_level_acc_t *acc, const int16_t *samples, size_t count)
{
    if (acc == NULL || samples == NULL || count == 0) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        int32_t v = samples[i];
        acc->sum_sq += (uint64_t)((int64_t)v * v);

        uint32_t peak = (uint32_t)((v < 0) ? -v : v);
        if (peak > acc->peak) {
            acc->peak = peak;
        }
    }
    acc->count += (uint32_t)count;
}

static int level_to_dbfs(float level)
{
    if (level <= 0.0f) {
        return -90;
    }

    float db = 20.0f * log10f(level / 32768.0f);
    if (db < -90.0f) {
        db = -90.0f;
    }
    return (int)lrintf(db);
}

static void format_bar(char *out, size_t out_len, int dbfs)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    const int bars = AUDIO_METER_BARS;
    int filled = 0;
    if (dbfs > -60) {
        filled = (dbfs + 60) * bars / 60;
        if (filled < 1) {
            filled = 1;
        }
        if (filled > bars) {
            filled = bars;
        }
    }

    size_t i = 0;
    for (; i < (size_t)filled && i + 1 < out_len; i++) {
        out[i] = '=';
    }
    for (; i + 1 < out_len; i++) {
        out[i] = ' ';
    }
    out[i] = '\0';
}

void audio_level_log(const char *label, audio_level_acc_t *acc)
{
    if (label == NULL || acc == NULL || acc->count == 0) {
        return;
    }

    float mean_sq = (float)acc->sum_sq / (float)acc->count;
    int rms_dbfs = level_to_dbfs(sqrtf(mean_sq));
    int peak_dbfs = level_to_dbfs((float)acc->peak);

    char bar[AUDIO_METER_BARS + 1];
    format_bar(bar, sizeof(bar), rms_dbfs);

    ESP_LOGI(TAG, "[AUDIO] %-7s [%s] %4d dBFS (peak %4d dBFS)",
             label, bar, rms_dbfs, peak_dbfs);

    acc->sum_sq = 0;
    acc->count = 0;
    acc->peak = 0;
}

void audio_stats_note_underrun(void)
{
    portENTER_CRITICAL(&s_stats_mux);
    s_underruns++;
    portEXIT_CRITICAL(&s_stats_mux);
}

void audio_stats_note_overrun(void)
{
    portENTER_CRITICAL(&s_stats_mux);
    s_overruns++;
    portEXIT_CRITICAL(&s_stats_mux);
}

esp_err_t audio_get_buffer_stats(uint32_t *underruns, uint32_t *overruns)
{
    portENTER_CRITICAL(&s_stats_mux);
    if (underruns != NULL) {
        *underruns = s_underruns;
    }
    if (overruns != NULL) {
        *overruns = s_overruns;
    }
    portEXIT_CRITICAL(&s_stats_mux);
    return ESP_OK;
}
