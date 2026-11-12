/*
 * Display bring-up for the GUITION JC4827W543.
 *
 * QSPI bus -> esp_lcd panel IO (quad mode) -> NV3041A panel -> PWM backlight.
 * Any failure is reported to the caller; no placeholder handle is returned.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "display_driver.h"
#include "nv3041_panel.h"

static const char *TAG = "DISPLAY";

#define LCD_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define LCD_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BACKLIGHT_LEDC_RES LEDC_TIMER_10_BIT
#define LCD_BACKLIGHT_PWM_HZ 5000
#define LCD_BACKLIGHT_DEFAULT_PERCENT 80

static esp_lcd_panel_handle_t s_panel;
static uint8_t s_backlight_percent;

esp_err_t display_driver_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    uint32_t duty = ((uint32_t)percent * ((1 << LCD_BACKLIGHT_LEDC_RES) - 1)) / 100;
    esp_err_t err = ledc_set_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(LCD_BACKLIGHT_LEDC_MODE, LCD_BACKLIGHT_LEDC_CHANNEL);
    }
    if (err == ESP_OK) {
        s_backlight_percent = percent;
    }
    return err;
}

uint8_t display_driver_get_backlight(void)
{
    return s_backlight_percent;
}

esp_lcd_panel_handle_t display_driver_panel(void)
{
    return s_panel;
}

static esp_err_t backlight_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "backlight GPIO config failed");
    /* Keep the panel dark until the first frame is ready. */
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_BACKLIGHT_GPIO, 0), TAG, "backlight off failed");

    ledc_timer_config_t timer = {
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .duty_resolution = LCD_BACKLIGHT_LEDC_RES,
        .timer_num = LCD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = LCD_BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer config failed");

    ledc_channel_config_t channel = {
        .gpio_num = LCD_BACKLIGHT_GPIO,
        .speed_mode = LCD_BACKLIGHT_LEDC_MODE,
        .channel = LCD_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel);
}

esp_err_t display_driver_init(void)
{
    ESP_LOGI(TAG, "Initializing Display...");
    ESP_LOGI(TAG, "Controller: NV3041A (QSPI)");
    ESP_LOGI(TAG, "Resolution: %dx%d, RGB565", LCD_H_RES, LCD_V_RES);

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");

    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_QSPI_SCK_GPIO,
        .data0_io_num = LCD_QSPI_D0_GPIO,
        .data1_io_num = LCD_QSPI_D1_GPIO,
        .data2_io_num = LCD_QSPI_D2_GPIO,
        .data3_io_num = LCD_QSPI_D3_GPIO,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG,
                        "QSPI bus init failed");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_QSPI_CS_GPIO,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_QSPI_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config,
                                                 &io),
                        TAG, "panel IO init failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1, /* no dedicated reset line on this board */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_nv3041(io, &panel_config, &s_panel), TAG,
                        "panel driver init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    /* The JC4827W543 panel is wired with inverted colour order. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");

    ESP_LOGI(TAG, "Initialization Complete");

    esp_err_t bl_err = display_driver_set_backlight(LCD_BACKLIGHT_DEFAULT_PERCENT);
    if (bl_err != ESP_OK) {
        ESP_LOGW(TAG, "Backlight on failed: %s", esp_err_to_name(bl_err));
    }
    return ESP_OK;
}
