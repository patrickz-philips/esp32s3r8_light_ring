#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

char s_device_name[32];
char s_ap_ssid[33];
char s_sta_ip[16];
bool s_sta_connected;
esp_ip4_addr_t s_ap_ip;

void build_device_identity(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_device_name, sizeof(s_device_name), "WLED-S3R8-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", s_device_name);
}

static bool has_sta_credentials(void)
{
    return strlen(CONFIG_LIGHT_RING_STA_SSID) > 0U;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void) arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && has_sta_credentials()) {
        ESP_LOGI(TAG, "Connecting to station SSID: %s", CONFIG_LIGHT_RING_STA_SSID);
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        s_sta_ip[0] = '\0';
        if (has_sta_credentials()) {
            ESP_LOGW(TAG, "STA disconnected, retrying");
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip_event = (ip_event_got_ip_t *) event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&got_ip_event->ip_info.ip));
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA connected with IP %s", s_sta_ip);
    }
}

void build_ap_root_url(char *buffer, size_t length)
{
    if (s_ap_ip.addr != 0U) {
        snprintf(buffer, length, "http://" IPSTR "/", IP2STR(&s_ap_ip));
    } else {
        strlcpy(buffer, "http://192.168.4.1/", length);
    }
}

esp_err_t init_wifi(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(ap_netif != NULL, ESP_FAIL, TAG, "create AP netif failed");

    esp_netif_t *sta_netif = NULL;
    if (has_sta_credentials()) {
        sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(sta_netif != NULL, ESP_FAIL, TAG, "create STA netif failed");
        ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, s_device_name));
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL), TAG, "register WIFI handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL), TAG, "register IP handler failed");

    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = AP_MAX_STA_CONNECTIONS,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    strlcpy((char *) ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *) ap_config.ap.password, CONFIG_LIGHT_RING_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.authmode = strlen(CONFIG_LIGHT_RING_AP_PASSWORD) >= 8U ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (ap_config.ap.authmode == WIFI_AUTH_OPEN) {
        ap_config.ap.password[0] = '\0';
    }

    wifi_mode_t mode = has_sta_credentials() ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "esp_wifi_set_config AP failed");

    if (has_sta_credentials()) {
        wifi_config_t sta_config = {
            .sta = {
                .failure_retry_cnt = 5,
                .scan_method = WIFI_ALL_CHANNEL_SCAN,
                .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {
                    .capable = true,
                    .required = false,
                },
            },
        };

        strlcpy((char *) sta_config.sta.ssid, CONFIG_LIGHT_RING_STA_SSID, sizeof(sta_config.sta.ssid));
        strlcpy((char *) sta_config.sta.password, CONFIG_LIGHT_RING_STA_PASSWORD, sizeof(sta_config.sta.password));
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "esp_wifi_set_config STA failed");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable Wi-Fi power save failed");

    esp_netif_ip_info_t ap_ip_info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap_netif, &ap_ip_info), TAG, "read AP IP info failed");
    s_ap_ip = ap_ip_info.ip;

    ESP_LOGI(TAG, "Access point ready: SSID=%s", s_ap_ssid);
    ESP_LOGI(TAG, "Access point IP: " IPSTR, IP2STR(&s_ap_ip));
    return ESP_OK;
}

esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        ret = nvs_flash_init();
    }
    return ret;
}

