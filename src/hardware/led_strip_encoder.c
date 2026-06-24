#include <stdlib.h>

#include "esp_check.h"
#include "led_strip_encoder.h"

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t bytes_encoder;
    rmt_encoder_handle_t reset_encoder;
    rmt_symbol_word_t reset_symbol;
    int phase;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder,
                            rmt_channel_handle_t channel,
                            const void *primary_data,
                            size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *strip_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t encode_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    if (strip_encoder->phase == 0) {
        encoded_symbols += strip_encoder->bytes_encoder->encode(
            strip_encoder->bytes_encoder,
            channel,
            primary_data,
            data_size,
            &encode_state);

        if (encode_state & RMT_ENCODING_COMPLETE) {
            strip_encoder->phase = 1;
        }
        if (encode_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            *ret_state = state;
            return encoded_symbols;
        }
    }

    encoded_symbols += strip_encoder->reset_encoder->encode(
        strip_encoder->reset_encoder,
        channel,
        &strip_encoder->reset_symbol,
        sizeof(strip_encoder->reset_symbol),
        &encode_state);

    if (encode_state & RMT_ENCODING_COMPLETE) {
        strip_encoder->phase = 0;
        state |= RMT_ENCODING_COMPLETE;
    }
    if (encode_state & RMT_ENCODING_MEM_FULL) {
        state |= RMT_ENCODING_MEM_FULL;
    }

    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t ws2812_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *strip_encoder = __containerof(encoder, ws2812_encoder_t, base);
    if (strip_encoder->bytes_encoder != NULL) {
        rmt_del_encoder(strip_encoder->bytes_encoder);
    }
    if (strip_encoder->reset_encoder != NULL) {
        rmt_del_encoder(strip_encoder->reset_encoder);
    }
    free(strip_encoder);
    return ESP_OK;
}

static esp_err_t ws2812_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *strip_encoder = __containerof(encoder, ws2812_encoder_t, base);
    ESP_RETURN_ON_ERROR(rmt_encoder_reset(strip_encoder->bytes_encoder), "ws2812_encoder", "reset bytes encoder failed");
    ESP_RETURN_ON_ERROR(rmt_encoder_reset(strip_encoder->reset_encoder), "ws2812_encoder", "reset reset encoder failed");
    strip_encoder->phase = 0;
    return ESP_OK;
}

esp_err_t rmt_new_led_strip_encoder(const led_strip_encoder_config_t *config,
                                    rmt_encoder_handle_t *ret_encoder)
{
    const char *tag = "ws2812_encoder";
    ws2812_encoder_t *strip_encoder = NULL;

    ESP_RETURN_ON_FALSE(config != NULL && ret_encoder != NULL, ESP_ERR_INVALID_ARG, tag, "invalid argument");

    strip_encoder = rmt_alloc_encoder_mem(sizeof(ws2812_encoder_t));
    ESP_RETURN_ON_FALSE(strip_encoder != NULL, ESP_ERR_NO_MEM, tag, "out of memory");

    strip_encoder->base.encode = ws2812_encode;
    strip_encoder->base.del = ws2812_del;
    strip_encoder->base.reset = ws2812_reset;
    strip_encoder->phase = 0;

    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = config->resolution_hz / 3333333,
            .level1 = 0,
            .duration1 = (config->resolution_hz * 9) / 10000000,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = (config->resolution_hz * 9) / 10000000,
            .level1 = 0,
            .duration1 = config->resolution_hz / 3333333,
        },
        .flags.msb_first = 1,
    };

    esp_err_t ret = rmt_new_bytes_encoder(&bytes_config, &strip_encoder->bytes_encoder);
    if (ret != ESP_OK) {
        free(strip_encoder);
        return ret;
    }

    rmt_copy_encoder_config_t copy_config = {};
    ret = rmt_new_copy_encoder(&copy_config, &strip_encoder->reset_encoder);
    if (ret != ESP_OK) {
        rmt_del_encoder(strip_encoder->bytes_encoder);
        free(strip_encoder);
        return ret;
    }

    uint32_t reset_ticks = (config->resolution_hz / 1000000U) * 25U;
    strip_encoder->reset_symbol = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };

    *ret_encoder = &strip_encoder->base;
    return ESP_OK;
}