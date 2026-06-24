#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "app_config.h"
#include "captive_portal.h"
#include "http_server.h"
#include "light_effects.h"
#include "light_types.h"
#include "palette_store.h"
#include "scenario_store.h"
#include "ui_page.h"
#include "wifi_manager.h"

static const char *TAG = "http_server";

static httpd_handle_t s_http_server;

void set_common_response_headers(httpd_req_t *req, const char *content_type)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    /* Close each connection after the response so sockets are returned to the
       LWIP pool promptly instead of lingering as keep-alive connections, which
       otherwise exhaust the descriptor pool during captive-portal bursts. */
    httpd_resp_set_hdr(req, "Connection", "close");
}

esp_err_t send_json_response(httpd_req_t *req, cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_common_response_headers(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, payload);
    free(payload);
    return ret;
}

static cJSON *build_info_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *led = cJSON_AddObjectToObject(root, "led");
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON *api = cJSON_AddArrayToObject(root, "api");

    cJSON_AddStringToObject(root, "name", s_device_name);
    cJSON_AddStringToObject(root, "brand", "WLED-style ESP-IDF");
    cJSON_AddStringToObject(root, "board", "esp32s3r8");
    cJSON_AddNumberToObject(led, "count", CONFIG_LIGHT_RING_LED_COUNT);
    cJSON_AddNumberToObject(led, "gpio", CONFIG_LIGHT_RING_LED_GPIO);
    cJSON_AddStringToObject(wifi, "ap_ssid", s_ap_ssid);
    cJSON_AddBoolToObject(wifi, "sta_connected", s_sta_connected);
    cJSON_AddStringToObject(wifi, "sta_ssid", CONFIG_LIGHT_RING_STA_SSID);
    cJSON_AddStringToObject(wifi, "sta_ip", s_sta_connected ? s_sta_ip : "");

    cJSON_AddItemToArray(api, cJSON_CreateString("/json/info"));
    cJSON_AddItemToArray(api, cJSON_CreateString("/json/state"));
    cJSON_AddItemToArray(api, cJSON_CreateString("/json/palettes"));
    cJSON_AddItemToArray(api, cJSON_CreateString("/win"));

    return root;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return ui_page_send(req);
}

static esp_err_t json_info_get_handler(httpd_req_t *req)
{
    cJSON *info = build_info_json();
    esp_err_t ret = send_json_response(req, info);
    cJSON_Delete(info);
    return ret;
}

static esp_err_t json_state_get_handler(httpd_req_t *req)
{
    cJSON *state = build_state_json();
    esp_err_t ret = send_json_response(req, state);
    cJSON_Delete(state);
    return ret;
}

static esp_err_t json_state_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= HTTP_RECV_BUFFER_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid body length");
    }

    char body[HTTP_RECV_BUFFER_SIZE];
    int total_read = 0;
    while (total_read < req->content_len) {
        int read_now = httpd_req_recv(req, body + total_read, req->content_len - total_read);
        if (read_now <= 0) {
            return httpd_resp_send_500(req);
        }
        total_read += read_now;
    }
    body[total_read] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "JSON parse failed");
    }

    light_state_t state;
    snapshot_light_state(&state);
    apply_json_state(root, &state);
    store_light_state(&state);
    cJSON_Delete(root);

    cJSON *response = build_state_json();
    esp_err_t ret = send_json_response(req, response);
    cJSON_Delete(response);
    return ret;
}

static void apply_query_value(light_state_t *state, const char *key, const char *value)
{
    if (strcmp(key, "T") == 0) {
        int toggle = atoi(value);
        if (toggle == 2) {
            state->on = !state->on;
        } else {
            state->on = toggle != 0;
        }
        return;
    }
    if (strcmp(key, "A") == 0) {
        state->brightness = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "R") == 0) {
        state->red = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "G") == 0) {
        state->green = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "B") == 0) {
        state->blue = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "FX") == 0) {
        state->effect = clamp_effect(atoi(value));
        return;
    }
    if (strcmp(key, "SX") == 0) {
        state->speed = clamp_u8(atoi(value));
        return;
    }
    if (strcmp(key, "FP") == 0) {
        state->palette = clamp_palette(atoi(value));
    }
}

static esp_err_t win_get_handler(httpd_req_t *req)
{
    int query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char query[256];
        if (query_len < (int) sizeof(query) && httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            light_state_t state;
            snapshot_light_state(&state);

            const char *keys[] = {"T", "A", "R", "G", "B", "FX", "SX", "FP"};
            char value[32];
            for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
                if (httpd_query_key_value(query, keys[index], value, sizeof(value)) == ESP_OK) {
                    apply_query_value(&state, keys[index], value);
                }
            }

            store_light_state(&state);
        }
    }

    cJSON *response = build_state_json();
    esp_err_t ret = send_json_response(req, response);
    cJSON_Delete(response);
    return ret;
}

static esp_err_t json_backup_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddItemToObject(root, "palettes", export_custom_palettes_json());
    cJSON_AddItemToObject(root, "scenarios", export_scenarios_json());

    esp_err_t ret = send_json_response(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t json_backup_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= BACKUP_MAX_BODY_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid body length");
    }

    char *body = malloc((size_t) req->content_len + 1U);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }

    int total_read = 0;
    while (total_read < req->content_len) {
        int read_now = httpd_req_recv(req, body + total_read, req->content_len - total_read);
        if (read_now <= 0) {
            free(body);
            return httpd_resp_send_500(req);
        }
        total_read += read_now;
    }
    body[total_read] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "JSON parse failed");
    }

    cJSON *palettes = cJSON_GetObjectItemCaseSensitive(root, "palettes");
    cJSON *scenarios = cJSON_GetObjectItemCaseSensitive(root, "scenarios");

    esp_err_t palette_ret = ESP_OK;
    esp_err_t scenario_ret = ESP_OK;
    if (cJSON_IsArray(palettes)) {
        palette_ret = import_custom_palettes_json(palettes);
    }
    if (cJSON_IsArray(scenarios)) {
        scenario_ret = import_scenarios_json(scenarios);
    }
    cJSON_Delete(root);

    if (palette_ret != ESP_OK || scenario_ret != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    esp_err_t ret = send_json_response(req, response);
    cJSON_Delete(response);
    return ret;
}

static esp_err_t register_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    const httpd_uri_t descriptor = {
        .uri = uri,
        .method = method,
        .handler = handler,
    };
    return httpd_register_uri_handler(s_http_server, &descriptor);
}

esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;
    config.max_uri_handlers = 24;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "httpd_start failed");

    ESP_RETURN_ON_ERROR(register_uri("/", HTTP_GET, root_get_handler), TAG, "register root handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/json/info", HTTP_GET, json_info_get_handler), TAG, "register info handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/json/state", HTTP_GET, json_state_get_handler), TAG, "register state GET handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/json/state", HTTP_POST, json_state_post_handler), TAG, "register state POST handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/json/palettes", HTTP_GET, json_palettes_get_handler), TAG, "register palettes GET handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/json/palettes", HTTP_POST, json_palettes_post_handler), TAG, "register palettes POST handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/json/scenarios", HTTP_GET, json_scenarios_get_handler), TAG, "register scenarios GET handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/json/scenarios", HTTP_POST, json_scenarios_post_handler), TAG, "register scenarios POST handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/json/backup", HTTP_GET, json_backup_get_handler), TAG, "register backup GET handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/json/backup", HTTP_POST, json_backup_post_handler), TAG, "register backup POST handler failed");
    ESP_RETURN_ON_ERROR(register_uri("/win", HTTP_GET, win_get_handler), TAG, "register win handler failed");

    ESP_RETURN_ON_ERROR(captive_portal_register_handlers(s_http_server), TAG, "register captive handlers failed");

    return ESP_OK;
}
