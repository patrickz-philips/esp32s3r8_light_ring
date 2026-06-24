#include <stdint.h>
#include <string.h>

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "board_led.h"
#include "led_strip_encoder.h"

static const char *TAG = "board_led";

static rmt_channel_handle_t s_led_channel;
static rmt_encoder_handle_t s_led_encoder;
static uint8_t s_led_pixels[CONFIG_LIGHT_RING_LED_COUNT * 3];

void set_pixel_rgb(uint16_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    size_t offset = index * 3U;
    s_led_pixels[offset + 0] = red;
    s_led_pixels[offset + 1] = green;
    s_led_pixels[offset + 2] = blue;
}

void clear_pixels(void)
{
    memset(s_led_pixels, 0, sizeof(s_led_pixels));
}

void fill_pixels_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint16_t index = 0; index < CONFIG_LIGHT_RING_LED_COUNT; ++index) {
        set_pixel_rgb(index, red, green, blue);
    }
}

esp_err_t transmit_pixels(void)
{
    const rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    ESP_RETURN_ON_ERROR(rmt_transmit(s_led_channel, s_led_encoder, s_led_pixels, sizeof(s_led_pixels), &tx_config), TAG, "LED transmit failed");
    return rmt_tx_wait_all_done(s_led_channel, portMAX_DELAY);
}

esp_err_t board_led_init(void)
{
    rmt_tx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = CONFIG_LIGHT_RING_LED_GPIO,
        .mem_block_symbols = LED_STRIP_MEM_BLOCK_SYMBOLS,
        .resolution_hz = LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &s_led_channel), TAG, "create RMT channel failed");

    led_strip_encoder_config_t encoder_config = {
        .resolution_hz = LED_STRIP_RESOLUTION_HZ,
    };
    ESP_RETURN_ON_ERROR(rmt_new_led_strip_encoder(&encoder_config, &s_led_encoder), TAG, "create LED encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_channel), TAG, "enable RMT channel failed");

    return ESP_OK;
}
