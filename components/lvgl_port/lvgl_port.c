/*
 * LVGL 9.3 port for the JC4827W543.
 *
 * Rendering uses two partial draw buffers in internal DMA-capable RAM. Each
 * flush is handed to esp_lcd and completed before LVGL reuses the buffer, so
 * a slow panel can never corrupt a frame.
 */
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "esp_lcd_panel_io.h"

#include "display_driver.h"
#include "lvgl_port.h"
#include "touch_driver.h"

static const char *TAG = "LVGL";

#define LVGL_DRAW_LINES 20
#define LVGL_DRAW_BUF_BYTES (LCD_H_RES * LVGL_DRAW_LINES * sizeof(uint16_t))
#define LVGL_FLUSH_TIMEOUT_MS 200

static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_flush_done;
static esp_timer_handle_t s_tick_timer;
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_display;
static TaskHandle_t s_task;

static bool flush_done_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata,
                          void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    int x_start = area->x1;
    int y_start = area->y1;
    int x_end = area->x2 + 1;
    int y_end = area->y2 + 1;

    /* Drop any completion left over from a previous frame so the wait below
     * cannot succeed before this transfer has actually finished. */
    while (xSemaphoreTake(s_flush_done, 0) == pdTRUE) {
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, px_map);
    if (err == ESP_OK) {
        if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(LVGL_FLUSH_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "Flush timeout at (%d,%d)-(%d,%d)", x_start, y_start, x_end, y_end);
        }
    } else {
        ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
    }

    lv_display_flush_ready(display);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    touch_point_t point = {0};

    if (touch_driver_read(&point) && point.pressed) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void lvgl_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t delay_ms = 10;
        if (lvgl_port_lock(portMAX_DELAY)) {
            delay_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (delay_ms < 5) {
            delay_ms = 5;
        } else if (delay_ms > 20) {
            delay_ms = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    if (s_mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t lvgl_port_init(esp_lcd_panel_handle_t panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "no panel handle");
    s_panel = panel;

    /* The flush path waits for the QSPI transfer to finish so the draw buffer
     * is not reused while DMA still reads it. */
    esp_lcd_panel_io_handle_t io = display_driver_panel_io();
    if (io != NULL) {
        esp_lcd_panel_io_callbacks_t cbs = {
            .on_color_trans_done = flush_done_cb,
        };
        esp_err_t cb_err = esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL);
        if (cb_err != ESP_OK) {
            ESP_LOGW(TAG, "Flush callback registration failed: %s (frames may tear)",
                     esp_err_to_name(cb_err));
        }
    }

    ESP_LOGI(TAG, "Initializing LVGL...");
    lv_init();

    s_mutex = xSemaphoreCreateMutex();
    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_mutex && s_flush_done, ESP_ERR_NO_MEM, TAG, "sync primitives failed");

    s_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    ESP_RETURN_ON_FALSE(s_display, ESP_ERR_NO_MEM, TAG, "display create failed");
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, flush_cb);

    void *buf1 = heap_caps_malloc(LVGL_DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(LVGL_DRAW_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        free(buf1);
        free(buf2);
        ESP_LOGE(TAG, "Not enough internal DMA memory for draw buffers (%u bytes each)",
                 (unsigned)LVGL_DRAW_BUF_BYTES);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_display, buf1, buf2, LVGL_DRAW_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(indev, ESP_ERR_NO_MEM, TAG, "indev create failed");
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, s_display);

    esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &s_tick_timer), TAG, "tick timer failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_tick_timer, 1000), TAG, "tick start failed");

    BaseType_t created = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL,
                                     LVGL_TASK_PRIORITY, &s_task);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "LVGL task create failed");

    ESP_LOGI(TAG, "UI Ready");
    return ESP_OK;
}
