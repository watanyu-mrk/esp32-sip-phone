/*
 * NV3041A QSPI panel driver (esp_lcd panel interface).
 *
 * Callers must create the panel IO with:
 *   lcd_cmd_bits   = 32
 *   lcd_param_bits = 8
 *   flags.quad_mode = 1
 *   dc_gpio_num    = -1
 * because every command and every pixel burst is prefixed by a 32-bit QSPI
 * opcode word on this panel.
 */
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nv3041_panel.h"

static const char *TAG = "lcd.nv3041";

#define LCD_OPCODE_WRITE_CMD   0x02
#define LCD_OPCODE_WRITE_COLOR 0x32

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t madctl_val;
    uint8_t colmod_val;
    uint8_t fb_bits_per_pixel;
} nv3041_panel_t;

static esp_err_t panel_nv3041_del(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041_init(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                          int x_end, int y_end, const void *color_data);
static esp_err_t panel_nv3041_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_nv3041_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_nv3041_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_nv3041_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_nv3041_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

/* QSPI command words. */
static esp_err_t tx_param(nv3041_panel_t *panel, int lcd_cmd, const void *param, size_t param_size)
{
    int cmd = ((lcd_cmd & 0xFF) << 8) | (LCD_OPCODE_WRITE_CMD << 24);
    return esp_lcd_panel_io_tx_param(panel->io, cmd, param, param_size);
}

static esp_err_t tx_color(nv3041_panel_t *panel, int lcd_cmd, const void *color, size_t color_size)
{
    int cmd = ((lcd_cmd & 0xFF) << 8) | (LCD_OPCODE_WRITE_COLOR << 24);
    return esp_lcd_panel_io_tx_color(panel->io, cmd, color, color_size);
}

/* JC4827W543 vendor sequence. 0xFF/0x36/0x3A are sent separately so the
 * driver can honour the requested RGB element order. */
static const nv3041_lcd_init_cmd_t s_init_cmds[] = {
    {0x41, (uint8_t[]){0x03}, 1, 0},
    {0x44, (uint8_t[]){0x15}, 1, 0},
    {0x45, (uint8_t[]){0x15}, 1, 0},
    {0x7d, (uint8_t[]){0x03}, 1, 0},
    {0xc1, (uint8_t[]){0xbb}, 1, 0},
    {0xc2, (uint8_t[]){0x05}, 1, 0},
    {0xc3, (uint8_t[]){0x10}, 1, 0},
    {0xc6, (uint8_t[]){0x3e}, 1, 0},
    {0xc7, (uint8_t[]){0x25}, 1, 0},
    {0xc8, (uint8_t[]){0x11}, 1, 0},
    {0x7a, (uint8_t[]){0x5f}, 1, 0},
    {0x6f, (uint8_t[]){0x44}, 1, 0},
    {0x78, (uint8_t[]){0x70}, 1, 0},
    {0xc9, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x21}, 1, 0},
    {0x51, (uint8_t[]){0x0a}, 1, 0},
    {0x52, (uint8_t[]){0x76}, 1, 0},
    {0x53, (uint8_t[]){0x0a}, 1, 0},
    {0x54, (uint8_t[]){0x76}, 1, 0},
    {0x46, (uint8_t[]){0x0a}, 1, 0},
    {0x47, (uint8_t[]){0x2a}, 1, 0},
    {0x48, (uint8_t[]){0x0a}, 1, 0},
    {0x49, (uint8_t[]){0x1a}, 1, 0},
    {0x56, (uint8_t[]){0x43}, 1, 0},
    {0x57, (uint8_t[]){0x42}, 1, 0},
    {0x58, (uint8_t[]){0x3c}, 1, 0},
    {0x59, (uint8_t[]){0x64}, 1, 0},
    {0x5a, (uint8_t[]){0x41}, 1, 0},
    {0x5b, (uint8_t[]){0x3c}, 1, 0},
    {0x5c, (uint8_t[]){0x02}, 1, 0},
    {0x5d, (uint8_t[]){0x3c}, 1, 0},
    {0x5e, (uint8_t[]){0x1f}, 1, 0},
    {0x60, (uint8_t[]){0x80}, 1, 0},
    {0x61, (uint8_t[]){0x3f}, 1, 0},
    {0x62, (uint8_t[]){0x21}, 1, 0},
    {0x63, (uint8_t[]){0x07}, 1, 0},
    {0x64, (uint8_t[]){0xe0}, 1, 0},
    {0x65, (uint8_t[]){0x02}, 1, 0},
    {0xca, (uint8_t[]){0x20}, 1, 0},
    {0xcb, (uint8_t[]){0x52}, 1, 0},
    {0xcc, (uint8_t[]){0x10}, 1, 0},
    {0xcd, (uint8_t[]){0x42}, 1, 0},
    {0xd0, (uint8_t[]){0x20}, 1, 0},
    {0xd1, (uint8_t[]){0x52}, 1, 0},
    {0xd2, (uint8_t[]){0x10}, 1, 0},
    {0xd3, (uint8_t[]){0x42}, 1, 0},
    {0xd4, (uint8_t[]){0x0a}, 1, 0},
    {0xd5, (uint8_t[]){0x32}, 1, 0},
    {0x80, (uint8_t[]){0x00}, 1, 0},
    {0xa0, (uint8_t[]){0x00}, 1, 0},
    {0x81, (uint8_t[]){0x07}, 1, 0},
    {0xa1, (uint8_t[]){0x06}, 1, 0},
    {0x82, (uint8_t[]){0x02}, 1, 0},
    {0xa2, (uint8_t[]){0x01}, 1, 0},
    {0x86, (uint8_t[]){0x11}, 1, 0},
    {0xa6, (uint8_t[]){0x10}, 1, 0},
    {0x87, (uint8_t[]){0x27}, 1, 0},
    {0xa7, (uint8_t[]){0x27}, 1, 0},
    {0x83, (uint8_t[]){0x37}, 1, 0},
    {0xa3, (uint8_t[]){0x37}, 1, 0},
    {0x84, (uint8_t[]){0x35}, 1, 0},
    {0xa4, (uint8_t[]){0x35}, 1, 0},
    {0x85, (uint8_t[]){0x3f}, 1, 0},
    {0xa5, (uint8_t[]){0x3f}, 1, 0},
    {0x88, (uint8_t[]){0x0b}, 1, 0},
    {0xa8, (uint8_t[]){0x0b}, 1, 0},
    {0x89, (uint8_t[]){0x14}, 1, 0},
    {0xa9, (uint8_t[]){0x14}, 1, 0},
    {0x8a, (uint8_t[]){0x1a}, 1, 0},
    {0xaa, (uint8_t[]){0x1a}, 1, 0},
    {0x8b, (uint8_t[]){0x0a}, 1, 0},
    {0xab, (uint8_t[]){0x0a}, 1, 0},
    {0x8c, (uint8_t[]){0x14}, 1, 0},
    {0xac, (uint8_t[]){0x08}, 1, 0},
    {0x8d, (uint8_t[]){0x17}, 1, 0},
    {0xad, (uint8_t[]){0x07}, 1, 0},
    {0x8e, (uint8_t[]){0x16}, 1, 0},
    {0xae, (uint8_t[]){0x06}, 1, 0},
    {0x8f, (uint8_t[]){0x1b}, 1, 0},
    {0xaf, (uint8_t[]){0x07}, 1, 0},
    {0x90, (uint8_t[]){0x04}, 1, 0},
    {0xb0, (uint8_t[]){0x04}, 1, 0},
    {0x91, (uint8_t[]){0x0a}, 1, 0},
    {0xb1, (uint8_t[]){0x0a}, 1, 0},
    {0x92, (uint8_t[]){0x16}, 1, 0},
    {0xb2, (uint8_t[]){0x15}, 1, 0},
    {0xff, (uint8_t[]){0x00}, 1, 0},
    {0x11, NULL, 0, 120}, /* sleep out */
    {0x29, NULL, 0, 50},  /* display on */
};

esp_err_t esp_lcd_new_panel_nv3041(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    nv3041_panel_t *panel = NULL;
    gpio_config_t io_conf = {0};

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG,
                      "invalid argument");
    panel = calloc(1, sizeof(nv3041_panel_t));
    ESP_GOTO_ON_FALSE(panel, ESP_ERR_NO_MEM, err, TAG, "no memory for panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure reset GPIO failed");
    }

    /* Portrait 272x480: the native raster is 480x272, so MV is required to
     * swap the axes. MY then keeps the image the right way up, matching the
     * 0xC0 (MX|MY) setting that was verified upright in landscape. */
    panel->madctl_val = 0xA0; /* MV | MY: portrait */
    if (panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR) {
        panel->madctl_val |= LCD_CMD_BGR_BIT;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        panel->colmod_val = 0x01; /* RGB565 */
        panel->fb_bits_per_pixel = 16;
        break;
    case 18:
        panel->colmod_val = 0x00; /* RGB666 */
        panel->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
                          "unsupported pixel width %d", panel_dev_config->bits_per_pixel);
    }

    panel->io = io;
    panel->reset_gpio_num = panel_dev_config->reset_gpio_num;
    panel->reset_level = panel_dev_config->flags.reset_active_high;

    panel->base.del = panel_nv3041_del;
    panel->base.reset = panel_nv3041_reset;
    panel->base.init = panel_nv3041_init;
    panel->base.draw_bitmap = panel_nv3041_draw_bitmap;
    panel->base.invert_color = panel_nv3041_invert_color;
    panel->base.set_gap = panel_nv3041_set_gap;
    panel->base.mirror = panel_nv3041_mirror;
    panel->base.swap_xy = panel_nv3041_swap_xy;
    panel->base.disp_on_off = panel_nv3041_disp_on_off;

    *ret_panel = &panel->base;
    ESP_LOGD(TAG, "new NV3041A panel @%p", panel);
    return ESP_OK;

err:
    if (panel) {
        free(panel);
    }
    return ret;
}

static esp_err_t panel_nv3041_del(esp_lcd_panel_t *panel)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    if (nv3041->reset_gpio_num >= 0) {
        gpio_reset_pin(nv3041->reset_gpio_num);
    }
    free(nv3041);
    return ESP_OK;
}

static esp_err_t panel_nv3041_reset(esp_lcd_panel_t *panel)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);

    if (nv3041->reset_gpio_num >= 0) {
        gpio_set_level(nv3041->reset_gpio_num, nv3041->reset_level);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(nv3041->reset_gpio_num, !nv3041->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(tx_param(nv3041, LCD_CMD_SWRESET, NULL, 0), TAG,
                            "software reset failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_nv3041_init(esp_lcd_panel_t *panel)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);

    /* Unlock vendor registers, then program MADCTL/COLMOD ourselves. */
    ESP_RETURN_ON_ERROR(tx_param(nv3041, 0xFF, (uint8_t[]){0xA5}, 1), TAG, "unlock failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041, LCD_CMD_MADCTL, &nv3041->madctl_val, 1), TAG,
                        "MADCTL failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041, LCD_CMD_COLMOD, &nv3041->colmod_val, 1), TAG,
                        "COLMOD failed");

    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(tx_param(nv3041, s_init_cmds[i].cmd, s_init_cmds[i].data,
                                     s_init_cmds[i].data_bytes),
                            TAG, "init command 0x%02x failed", s_init_cmds[i].cmd);
        if (s_init_cmds[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(s_init_cmds[i].delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t panel_nv3041_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                          int x_end, int y_end, const void *color_data)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);

    x_start += nv3041->x_gap;
    x_end += nv3041->x_gap;
    y_start += nv3041->y_gap;
    y_end += nv3041->y_gap;

    ESP_RETURN_ON_FALSE((x_start < x_end) && (y_start < y_end), ESP_ERR_INVALID_ARG, TAG,
                        "invalid draw area");

    uint8_t caset[4] = {
        (uint8_t)(x_start >> 8), (uint8_t)(x_start & 0xFF),
        (uint8_t)((x_end - 1) >> 8), (uint8_t)((x_end - 1) & 0xFF),
    };
    uint8_t raset[4] = {
        (uint8_t)(y_start >> 8), (uint8_t)(y_start & 0xFF),
        (uint8_t)((y_end - 1) >> 8), (uint8_t)((y_end - 1) & 0xFF),
    };
    ESP_RETURN_ON_ERROR(tx_param(nv3041, LCD_CMD_CASET, caset, sizeof(caset)), TAG,
                        "CASET failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041, LCD_CMD_RASET, raset, sizeof(raset)), TAG,
                        "RASET failed");

    size_t len = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) *
                 nv3041->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(tx_color(nv3041, LCD_CMD_RAMWR, color_data, len), TAG,
                        "GRAM write failed");
    return ESP_OK;
}

static esp_err_t panel_nv3041_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    return tx_param(nv3041, invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_nv3041_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);

    if (mirror_x) {
        nv3041->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        nv3041->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        nv3041->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        nv3041->madctl_val &= ~LCD_CMD_MY_BIT;
    }

    /* Vendor registers must be unlocked before MADCTL is accepted again. */
    ESP_RETURN_ON_ERROR(tx_param(nv3041, 0xFF, (uint8_t[]){0xA5}, 1), TAG, "unlock failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041, LCD_CMD_MADCTL, &nv3041->madctl_val, 1), TAG,
                        "MADCTL failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041, 0xFF, (uint8_t[]){0x00}, 1), TAG, "lock failed");
    return ESP_OK;
}

static esp_err_t panel_nv3041_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);

    if (swap_axes) {
        nv3041->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        nv3041->madctl_val &= ~LCD_CMD_MV_BIT;
    }

    ESP_RETURN_ON_ERROR(tx_param(nv3041, 0xFF, (uint8_t[]){0xA5}, 1), TAG, "unlock failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041, LCD_CMD_MADCTL, &nv3041->madctl_val, 1), TAG,
                        "MADCTL failed");
    ESP_RETURN_ON_ERROR(tx_param(nv3041, 0xFF, (uint8_t[]){0x00}, 1), TAG, "lock failed");
    return ESP_OK;
}

static esp_err_t panel_nv3041_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    nv3041->x_gap = x_gap;
    nv3041->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_nv3041_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    nv3041_panel_t *nv3041 = __containerof(panel, nv3041_panel_t, base);
    return tx_param(nv3041, on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}
