#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip4_addr.h"

#include "app_config.h"
#include "board_led.h"
#include "captive_portal.h"
#include "http_server.h"
#include "light_effects.h"
#include "palette_store.h"
#include "scenario_store.h"
#include "wifi_manager.h"

static const char *TAG = "light_ring";

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());

    s_state_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_state_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(load_custom_palettes_from_nvs());
    ESP_ERROR_CHECK(load_scenarios_from_nvs());

    build_device_identity();

    ESP_LOGI(TAG, "Booting %s", s_device_name);
    ESP_LOGI(TAG, "Light ring: %d LEDs on GPIO%d", CONFIG_LIGHT_RING_LED_COUNT, CONFIG_LIGHT_RING_LED_GPIO);

    ESP_ERROR_CHECK(board_led_init());
    ESP_ERROR_CHECK(light_effects_start());
    ESP_ERROR_CHECK(init_wifi());
    ESP_ERROR_CHECK(start_captive_dns_server());
    ESP_ERROR_CHECK(start_http_server());

    ESP_LOGI(TAG, "Control UI ready at http://" IPSTR "/", IP2STR(&s_ap_ip));
}
