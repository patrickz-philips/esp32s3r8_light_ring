#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t load_scenarios_from_nvs(void);
esp_err_t save_scenarios_to_nvs(void);
esp_err_t json_scenarios_get_handler(httpd_req_t *req);
esp_err_t json_scenarios_post_handler(httpd_req_t *req);

cJSON *export_scenarios_json(void);
esp_err_t import_scenarios_json(const cJSON *array);
