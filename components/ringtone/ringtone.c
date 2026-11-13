#include "ringtone.h"

#include <math.h>
#include <string.h>

#include "audio_driver.h"
#include "config_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RING";

#define RING_FRAME_MS     20
#define RING_RAMP_MS      6
#define RING_AMPLITUDE    0.55f
#define RING_TASK_STACK   4096
#define RING_TASK_PRIO    6

/* One cadence step: two tones (one may be 0) or silence when both are 0. */
typedef struct {
    uint16_t f1;
    uint16_t f2;
    uint16_t ms;
} ring_seg_t;

typedef struct {
    const ring_seg_t *segs;
    uint8_t count;
} ring_pattern_t;

static const ring_seg_t s_classic_segs[] = {
    {440, 480, 2000},
    {0, 0, 4000},
};

static const ring_seg_t s_double_segs[] = {
    {400, 450, 400},
    {0, 0, 200},
    {400, 450, 400},
    {0, 0, 2000},
};

static const ring_seg_t s_bell_segs[] = {
    {1050, 1400, 700},
    {0, 0, 1800},
};

static const ring_seg_t s_chime_segs[] = {
    {660, 0, 160},
    {880, 0, 160},
    {1175, 0, 260},
    {0, 0, 2600},
};

static const ring_seg_t s_pulse_segs[] = {
    {900, 0, 90},
    {0, 0, 120},
    {900, 0, 90},
    {0, 0, 120},
    {900, 0, 90},
    {0, 0, 2000},
};

static const ring_pattern_t s_patterns[RINGTONE_COUNT] = {
    {s_classic_segs, sizeof(s_classic_segs) / sizeof(s_classic_segs[0])},
    {s_double_segs, sizeof(s_double_segs) / sizeof(s_double_segs[0])},
    {s_bell_segs, sizeof(s_bell_segs) / sizeof(s_bell_segs[0])},
    {s_chime_segs, sizeof(s_chime_segs) / sizeof(s_chime_segs[0])},
    {s_pulse_segs, sizeof(s_pulse_segs) / sizeof(s_pulse_segs[0])},
};

static const char *const s_names[RINGTONE_COUNT] = {
    "Classic 440/480",
    "Double ring",
    "Bell warble",
    "Chime",
    "Pulse beeps",
};

static TaskHandle_t s_task;
static ringtone_gate_fn s_gate;
static ringtone_busy_fn s_busy;

static volatile bool s_playing;
static bool s_loop;
static bool s_key_mode;
static bool s_use_gate;
static bool s_finished;
static const ring_pattern_t *s_pattern;
static uint32_t s_seg;
static uint32_t s_seg_pos;
static uint32_t s_phase1;
static uint32_t s_phase2;

/* One mutable segment stands in for a dialpad key, so the same generator and
 * the same speaker ownership serve both features. */
static ring_seg_t s_key_seg;
static const ring_pattern_t s_key_pattern = {&s_key_seg, 1};

static const char s_dtmf_keys[4][4] = {"123", "456", "789", "*0#"};
static const uint16_t s_dtmf_low_hz[4] = {697, 770, 852, 941};
static const uint16_t s_dtmf_high_hz[3] = {1209, 1336, 1477};

static inline float phase_to_rad(uint32_t phase)
{
    return (float)phase * (6.28318530718f / 4294967296.0f);
}

static inline uint32_t phase_step(uint16_t hz)
{
    return (uint32_t)(((uint64_t)hz << 32) / AUDIO_SAMPLE_RATE_HZ);
}

/* Fills one frame from the pattern. Sets s_finished when a one-shot pattern
 * runs out, so the task can shut the speaker down afterwards. */
static void fill_frame(int16_t *out, size_t count)
{
    size_t i = 0;

    while (i < count) {
        if (s_seg >= s_pattern->count) {
            if (s_loop) {
                s_seg = 0;
            } else {
                break;
            }
        }

        const ring_seg_t *seg = &s_pattern->segs[s_seg];
        uint32_t total = (uint32_t)seg->ms * AUDIO_SAMPLE_RATE_HZ / 1000;

        if (s_seg_pos >= total) {
            s_seg++;
            s_seg_pos = 0;
            continue;
        }

        if (seg->f1 == 0 && seg->f2 == 0) {
            out[i++] = 0;
            s_seg_pos++;
            continue;
        }

        float amp = RING_AMPLITUDE;
        if (seg->f1 != 0 && seg->f2 != 0) {
            amp *= 0.5f; /* two tones sum, so halve to stay below full scale */
        }

        /* Ramp the edges: a hard start on a sine is an audible click. */
        uint32_t ramp = (uint32_t)RING_RAMP_MS * AUDIO_SAMPLE_RATE_HZ / 1000;
        if (s_seg_pos < ramp) {
            amp *= (float)s_seg_pos / (float)ramp;
        } else if (s_seg_pos + ramp >= total) {
            amp *= (float)(total - s_seg_pos) / (float)ramp;
        }

        float sample = 0.0f;
        if (seg->f1 != 0) {
            s_phase1 += phase_step(seg->f1);
            sample += sinf(phase_to_rad(s_phase1));
        }
        if (seg->f2 != 0) {
            s_phase2 += phase_step(seg->f2);
            sample += sinf(phase_to_rad(s_phase2));
        }

        int32_t v = (int32_t)lrintf(sample * amp * 32767.0f);
        if (v > INT16_MAX) {
            v = INT16_MAX;
        } else if (v < INT16_MIN) {
            v = INT16_MIN;
        }
        out[i++] = (int16_t)v;
        s_seg_pos++;
    }

    if (i < count) {
        memset(out + i, 0, (count - i) * sizeof(int16_t));
        s_finished = true;
    }
}

static void stop_internal(bool stop_output)
{
    if (!s_playing) {
        return;
    }

    bool was_key = s_key_mode;
    s_playing = false;
    s_key_mode = false;
    s_use_gate = false;

    const phone_config_t *cfg = config_manager_get();
    uint8_t volume = (cfg != NULL) ? cfg->audio.speaker_volume : AUDIO_DEFAULT_VOLUME;
    audio_set_volume(volume);

    /* A call that started while the tone was playing owns the speaker now. */
    bool others_use_output = (s_busy != NULL) && s_busy();
    if (stop_output && !others_use_output) {
        audio_output_stop();
    }

    if (was_key) {
        ESP_LOGD(TAG, "[RING] Key tone finished");
    } else {
        ESP_LOGI(TAG, "[RING] Ringtone stopped, call volume %u%%", (unsigned)volume);
    }
}

static void ringtone_task(void *arg)
{
    (void)arg;

    int16_t frame[AUDIO_FRAME_SAMPLES];
    TickType_t last = xTaskGetTickCount();

    while (true) {
        if (s_playing) {
            if (s_use_gate && s_gate != NULL && !s_gate()) {
                /* The call was answered or cancelled: the call audio owns the
                 * speaker now, so leave the output path alone. */
                stop_internal(false);
            } else {
                fill_frame(frame, AUDIO_FRAME_SAMPLES);
                audio_write_pcm(frame, AUDIO_FRAME_SAMPLES);
                if (s_finished) {
                    s_finished = false;
                    stop_internal(true);
                }
            }
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RING_FRAME_MS));
    }
}

esp_err_t ringtone_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(ringtone_task, "ringtone", RING_TASK_STACK, NULL,
                                RING_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "[RING] Failed to create the ringtone task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[RING] Ringtone engine ready (%d patterns)", RINGTONE_COUNT);
    return ESP_OK;
}

const char *ringtone_name(int id)
{
    if (id < 0 || id >= RINGTONE_COUNT) {
        return s_names[RINGTONE_DEFAULT];
    }
    return s_names[id];
}

bool ringtone_is_playing(void)
{
    return s_playing;
}

bool ringtone_is_ringing(void)
{
    return s_playing && s_loop;
}

void ringtone_set_gate(ringtone_gate_fn gate)
{
    s_gate = gate;
}

void ringtone_set_busy_fn(ringtone_busy_fn busy)
{
    s_busy = busy;
}

static bool dtmf_frequencies(char key, uint16_t *low, uint16_t *high)
{
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 3; col++) {
            if (s_dtmf_keys[row][col] == key) {
                *low = s_dtmf_low_hz[row];
                *high = s_dtmf_high_hz[col];
                return true;
            }
        }
    }
    return false;
}

esp_err_t ringtone_play_key(char key, uint8_t volume)
{
    uint16_t low = 0;
    uint16_t high = 0;

    if (s_task == NULL || !dtmf_frequencies(key, &low, &high)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (volume == 0) {
        return ESP_OK; /* muted */
    }
    if (s_playing && !s_key_mode) {
        /* A ring or a preview is using the speaker: it has priority. */
        return ESP_OK;
    }

    esp_err_t err = audio_output_start();
    if (err != ESP_OK) {
        return err;
    }

    s_key_seg.f1 = low;
    s_key_seg.f2 = high;
    s_key_seg.ms = KEY_TONE_MS;
    s_pattern = &s_key_pattern;
    s_seg = 0;
    s_seg_pos = 0;
    s_phase1 = 0;
    s_phase2 = 0;
    s_finished = false;
    s_loop = false;
    s_use_gate = false;
    s_key_mode = true;

    audio_set_volume(volume);
    s_playing = true;

    ESP_LOGI(TAG, "[RING] Key tone '%c' %u/%u Hz at %u%%", key, (unsigned)low,
             (unsigned)high, (unsigned)volume);
    return ESP_OK;
}

static esp_err_t play(int id, bool loop, uint8_t volume)
{
    if (id < 0 || id >= RINGTONE_COUNT) {
        id = RINGTONE_DEFAULT;
    }
    if (volume > 100) {
        volume = 100;
    }

    esp_err_t err = audio_output_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[RING] Speaker start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_pattern = &s_patterns[id];
    s_seg = 0;
    s_seg_pos = 0;
    s_phase1 = 0;
    s_phase2 = 0;
    s_finished = false;
    s_loop = loop;
    s_use_gate = loop;
    s_key_mode = false;

    audio_set_volume(volume);
    s_playing = true;

    ESP_LOGI(TAG, "[RING] '%s' %s at %u%%", s_names[id], loop ? "ringing" : "preview",
             (unsigned)volume);
    return ESP_OK;
}

esp_err_t ringtone_start(int id, uint8_t volume)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_playing) {
        return ESP_OK;
    }
    return play(id, true, volume);
}

esp_err_t ringtone_preview(int id, uint8_t volume)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_playing) {
        /* A ring or an earlier preview already uses the speaker. */
        return ESP_ERR_INVALID_STATE;
    }
    return play(id, false, volume);
}

void ringtone_stop(bool stop_output)
{
    stop_internal(stop_output);
}
