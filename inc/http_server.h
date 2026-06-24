#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t start_http_server(void);
void set_common_response_headers(httpd_req_t *req, const char *content_type);
esp_err_t send_json_response(httpd_req_t *req, cJSON *json);
