#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "app_config.h"
#include "http_server.h"
#include "light_effects.h"
#include "light_types.h"
#include "scenario_store.h"

static const char *TAG = "scenario_store";

static scenario_t s_scenarios[SCENARIO_MAX_COUNT];
static uint8_t s_scenario_count;

esp_err_t load_scenarios_from_nvs(void)
{
    s_scenario_count = 0U;
    memset(s_scenarios, 0, sizeof(s_scenarios));

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(SCENARIO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open scenario namespace failed");

    uint8_t stored_count = 0U;
    ret = nvs_get_u8(handle, SCENARIO_NVS_COUNT_KEY, &stored_count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        nvs_close(handle);
        ESP_RETURN_ON_ERROR(ret, TAG, "read scenario count failed");
    }

    if (stored_count > SCENARIO_MAX_COUNT) {
        stored_count = SCENARIO_MAX_COUNT;
    }

    for (uint8_t index = 0U; index < stored_count; ++index) {
        char key[12];
        snprintf(key, sizeof(key), "s%u", (unsigned) index);
        size_t size = sizeof(scenario_t);
        if (nvs_get_blob(handle, key, &s_scenarios[s_scenario_count], &size) == ESP_OK) {
            s_scenarios[s_scenario_count].name[SCENARIO_NAME_LENGTH - 1U] = '\0';
            s_scenarios[s_scenario_count].payload[SCENARIO_PAYLOAD_LENGTH - 1U] = '\0';
            s_scenario_count++;
        }
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t save_scenarios_to_nvs(void)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(SCENARIO_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open scenario namespace failed");

    esp_err_t ret = nvs_set_u8(handle, SCENARIO_NVS_COUNT_KEY, s_scenario_count);
    for (uint8_t index = 0U; ret == ESP_OK && index < s_scenario_count; ++index) {
        char key[12];
        snprintf(key, sizeof(key), "s%u", (unsigned) index);
        ret = nvs_set_blob(handle, key, &s_scenarios[index], sizeof(scenario_t));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}
static cJSON *build_scenarios_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "items");

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    uint8_t count = s_scenario_count;
    for (uint8_t index = 0U; index < count; ++index) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", index);
        cJSON_AddStringToObject(item, "name", s_scenarios[index].name);
        cJSON *state = cJSON_Parse(s_scenarios[index].payload);
        if (state == NULL) {
            state = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(item, "state", state);
        cJSON_AddItemToArray(items, item);
    }
    xSemaphoreGive(s_state_lock);

    return root;
}

esp_err_t json_scenarios_get_handler(httpd_req_t *req)
{
    cJSON *scenarios = build_scenarios_json();
    esp_err_t ret = send_json_response(req, scenarios);
    cJSON_Delete(scenarios);
    return ret;
}

esp_err_t json_scenarios_post_handler(httpd_req_t *req)
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

    cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (cJSON_IsString(action) && action->valuestring != NULL && strcmp(action->valuestring, "delete") == 0) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (!cJSON_IsNumber(id) || id->valueint < 0) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "Scenario id is required");
        }

        bool removed = false;
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        if (id->valueint < s_scenario_count) {
            for (uint8_t index = (uint8_t) id->valueint; index + 1U < s_scenario_count; ++index) {
                s_scenarios[index] = s_scenarios[index + 1U];
            }
            s_scenario_count--;
            memset(&s_scenarios[s_scenario_count], 0, sizeof(scenario_t));
            removed = true;
        }
        xSemaphoreGive(s_state_lock);

        cJSON_Delete(root);
        if (!removed) {
            httpd_resp_set_status(req, "404 Not Found");
            return httpd_resp_sendstr(req, "Scenario not found");
        }
        if (save_scenarios_to_nvs() != ESP_OK) {
            return httpd_resp_send_500(req);
        }

        cJSON *response = build_scenarios_json();
        esp_err_t ret = send_json_response(req, response);
        cJSON_Delete(response);
        return ret;
    }

    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsObject(state)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Scenario state is required");
    }

    char *payload = cJSON_PrintUnformatted(state);
    if (payload == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    if (strlen(payload) >= SCENARIO_PAYLOAD_LENGTH) {
        cJSON_free(payload);
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Scenario state is too large");
    }

    scenario_t entry;
    memset(&entry, 0, sizeof(entry));
    if (cJSON_IsString(name) && name->valuestring != NULL && name->valuestring[0] != '\0') {
        strlcpy(entry.name, name->valuestring, sizeof(entry.name));
    } else {
        strlcpy(entry.name, "Scenario", sizeof(entry.name));
    }
    strlcpy(entry.payload, payload, sizeof(entry.payload));
    cJSON_free(payload);
    cJSON_Delete(root);

    bool full = false;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_scenario_count >= SCENARIO_MAX_COUNT) {
        full = true;
    } else {
        s_scenarios[s_scenario_count++] = entry;
    }
    xSemaphoreGive(s_state_lock);

    if (full) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Scenario storage is full");
    }

    if (save_scenarios_to_nvs() != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    cJSON *response = build_scenarios_json();
    esp_err_t ret = send_json_response(req, response);
    cJSON_Delete(response);
    return ret;
}

cJSON *export_scenarios_json(void)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        return NULL;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    uint8_t count = s_scenario_count;
    for (uint8_t index = 0U; index < count; ++index) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", s_scenarios[index].name);
        cJSON *state = cJSON_Parse(s_scenarios[index].payload);
        if (state == NULL) {
            state = cJSON_CreateObject();
        }
        cJSON_AddItemToObject(item, "state", state);
        cJSON_AddItemToArray(array, item);
    }
    xSemaphoreGive(s_state_lock);

    return array;
}

esp_err_t import_scenarios_json(const cJSON *array)
{
    if (!cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }

    scenario_t imported[SCENARIO_MAX_COUNT];
    memset(imported, 0, sizeof(imported));
    uint8_t imported_count = 0U;

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, array) {
        if (imported_count >= SCENARIO_MAX_COUNT) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }

        cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        cJSON *state = cJSON_GetObjectItemCaseSensitive(entry, "state");
        if (!cJSON_IsObject(state)) {
            continue;
        }

        char *payload = cJSON_PrintUnformatted(state);
        if (payload == NULL) {
            continue;
        }
        if (strlen(payload) >= SCENARIO_PAYLOAD_LENGTH) {
            cJSON_free(payload);
            continue;
        }

        scenario_t item;
        memset(&item, 0, sizeof(item));
        if (cJSON_IsString(name) && name->valuestring != NULL && name->valuestring[0] != '\0') {
            strlcpy(item.name, name->valuestring, sizeof(item.name));
        } else {
            strlcpy(item.name, "Scenario", sizeof(item.name));
        }
        strlcpy(item.payload, payload, sizeof(item.payload));
        cJSON_free(payload);
        imported[imported_count++] = item;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    memcpy(s_scenarios, imported, sizeof(s_scenarios));
    s_scenario_count = imported_count;
    xSemaphoreGive(s_state_lock);

    return save_scenarios_to_nvs();
}

