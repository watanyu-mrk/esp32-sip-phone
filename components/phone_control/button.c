#include "button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BUTTON";

#define BUTTON_POLL_MS   10
#define BUTTON_STABLE_N  3

typedef struct {
    int gpio;
    int stable;
    int count;
    int64_t pressed_at;
    bool long_fired;
} button_state_t;

static button_callback_t s_callback;
static TaskHandle_t s_task;
static bool s_running;
static button_state_t s_buttons[2];

static void button_poll(button_id_t id)
{
    button_state_t *b = &s_buttons[id];
    int level = gpio_get_level(b->gpio);

    if (level == b->stable) {
        b->count = 0;
    } else if (++b->count >= BUTTON_STABLE_N) {
        b->stable = level;
        b->count = 0;
        if (level == 0) {
            b->pressed_at = esp_timer_get_time();
            b->long_fired = false;
            if (s_callback != NULL) {
                s_callback(id, BUTTON_EVENT_PRESSED);
            }
        } else if (s_callback != NULL) {
            s_callback(id, BUTTON_EVENT_RELEASED);
        }
    }

    if (b->stable == 0 && !b->long_fired &&
        (esp_timer_get_time() - b->pressed_at) >= (int64_t)BUTTON_LONG_PRESS_MS * 1000) {
        b->long_fired = true;
        if (s_callback != NULL) {
            s_callback(id, BUTTON_EVENT_LONG_PRESS);
        }
    }
}

static void button_task(void *arg)
{
    (void)arg;
    while (s_running) {
        button_poll(BUTTON_ANSWER);
        button_poll(BUTTON_HANGUP);
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
    ESP_LOGI(TAG, "[BUTTON] Task stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t button_init(button_callback_t callback)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_callback = callback;

    /* Both buttons are active-low to ground. GPIO3 is a strapping pin, so only
     * the internal pull-up is enabled: a held button never drives it low at
     * reset unless the user is pressing it, and boot mode selection is intact. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUTTON_ANSWER_GPIO) | (1ULL << BUTTON_HANGUP_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[BUTTON] GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_buttons[BUTTON_ANSWER] = (button_state_t){
        .gpio = BUTTON_ANSWER_GPIO,
        .stable = gpio_get_level(BUTTON_ANSWER_GPIO),
    };
    s_buttons[BUTTON_HANGUP] = (button_state_t){
        .gpio = BUTTON_HANGUP_GPIO,
        .stable = gpio_get_level(BUTTON_HANGUP_GPIO),
    };

    s_running = true;
    if (xTaskCreate(button_task, "buttons", 3072, NULL, 4, &s_task) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "[BUTTON] Failed to create task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[BUTTON] ANSWER=GPIO%d HANGUP=GPIO%d ready",
             BUTTON_ANSWER_GPIO, BUTTON_HANGUP_GPIO);
    return ESP_OK;
}
