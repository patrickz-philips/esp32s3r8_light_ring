#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_netif.h"

extern char s_device_name[32];
extern char s_ap_ssid[33];
extern char s_sta_ip[16];
extern bool s_sta_connected;
extern esp_ip4_addr_t s_ap_ip;

esp_err_t init_nvs(void);
void build_device_identity(void);
esp_err_t init_wifi(void);
void build_ap_root_url(char *buffer, size_t length);
