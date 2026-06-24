#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t start_captive_dns_server(void);
esp_err_t captive_portal_register_handlers(httpd_handle_t server);
