/*
 * GT911 polling driver.
 *
 * Register map used here:
 *   0x8140  product ID ("911")
 *   0x814E  buffer status: bit7 = ready, low nibble = touch count
 *   0x8150  first touch point: id, x lo/hi, y lo/hi, size lo/hi
 * Reading 0x814E and then writing 0 to it releases the controller buffer.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "touch_driver.h"

static const char *TAG = "TOUCH";

#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS     0x814E
#define GT911_REG_POINT1     0x8150

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

static esp_err_t gt911_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), buf, len, 50);
}

static esp_err_t gt911_write_u8(uint16_t reg, uint8_t value)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static esp_err_t gt911_hw_reset(bool int_high_for_alternate_addr)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_RST_GPIO) | (1ULL << TOUCH_INT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "reset GPIO config failed");

    gpio_set_level(TOUCH_RST_GPIO, 0);
    gpio_set_level(TOUCH_INT_GPIO, int_high_for_alternate_addr ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TOUCH_INT_GPIO, int_high_for_alternate_addr ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t gt911_add_device(uint8_t address)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = TOUCH_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
}

static bool gt911_check_product(void)
{
    uint8_t id[4] = {0};
    if (gt911_read(GT911_REG_PRODUCT_ID, id, sizeof(id)) != ESP_OK) {
        return false;
    }
    ESP_LOGI(TAG, "Controller ID: %c%c%c", id[0], id[1], id[2]);
    return id[0] == '9' && id[1] == '1' && id[2] == '1';
}

static void gt911_int_to_input(void)
{
    gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << TOUCH_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);
}

esp_err_t touch_driver_init(void)
{
    ESP_LOGI(TAG, "Initializing Touch Controller...");

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_I2C_SDA_GPIO,
        .scl_io_num = TOUCH_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus init failed");

    /* 0x5D is selected when INT is low while reset is released. */
    ESP_RETURN_ON_ERROR(gt911_hw_reset(false), TAG, "reset failed");
    esp_err_t err = gt911_add_device(TOUCH_I2C_ADDR_PRIMARY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!gt911_check_product()) {
        /* Try the alternate strap (INT high during reset). */
        ESP_LOGW(TAG, "No answer at 0x%02X, trying 0x%02X", TOUCH_I2C_ADDR_PRIMARY,
                 TOUCH_I2C_ADDR_ALTERNATE);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        ESP_RETURN_ON_ERROR(gt911_hw_reset(true), TAG, "alternate reset failed");
        ESP_RETURN_ON_ERROR(gt911_add_device(TOUCH_I2C_ADDR_ALTERNATE), TAG,
                            "alternate I2C device add failed");
        if (!gt911_check_product()) {
            ESP_LOGE(TAG, "GT911 not responding");
            return ESP_ERR_NOT_FOUND;
        }
    }

    gt911_int_to_input();
    s_ready = true;
    ESP_LOGI(TAG, "Touch Ready");
    return ESP_OK;
}

bool touch_driver_is_ready(void)
{
    return s_ready;
}

bool touch_driver_read(touch_point_t *point)
{
    if (!s_ready || point == NULL) {
        return false;
    }

    uint8_t status = 0;
    if (gt911_read(GT911_REG_STATUS, &status, 1) != ESP_OK) {
        return false;
    }
    if ((status & 0x80) == 0) {
        return false; /* controller has no new frame */
    }

    uint8_t count = status & 0x0F;
    bool pressed = false;
    uint16_t x = 0;
    uint16_t y = 0;

    if (count > 0) {
        uint8_t data[8] = {0};
        if (gt911_read(GT911_REG_POINT1, data, sizeof(data)) == ESP_OK) {
            x = (uint16_t)(data[1] | (data[2] << 8));
            y = (uint16_t)(data[3] | (data[4] << 8));
            pressed = true;
        }
    }

    /* Release the buffer even when no point was reported. */
    gt911_write_u8(GT911_REG_STATUS, 0);

    if (pressed) {
#if TOUCH_SWAP_XY
        uint16_t tmp = x;
        x = y;
        y = tmp;
#endif
#if TOUCH_MIRROR_X
        x = (uint16_t)(TOUCH_H_RES - 1 - (x < TOUCH_H_RES ? x : TOUCH_H_RES - 1));
#endif
#if TOUCH_MIRROR_Y
        y = (uint16_t)(TOUCH_V_RES - 1 - (y < TOUCH_V_RES ? y : TOUCH_V_RES - 1));
#endif
        if (x >= TOUCH_H_RES) {
            x = TOUCH_H_RES - 1;
        }
        if (y >= TOUCH_V_RES) {
            y = TOUCH_V_RES - 1;
        }
    }

    point->x = x;
    point->y = y;
    point->pressed = pressed;
    return true;
}
