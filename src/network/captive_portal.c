#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "captive_portal.h"
#include "http_server.h"
#include "wifi_manager.h"

static const char *TAG = "captive_portal";

static size_t dns_question_end_offset(const uint8_t *packet, size_t length)
{
    size_t offset = 12U;
    while (offset < length) {
        uint8_t label_len = packet[offset++];
        if (label_len == 0U) {
            break;
        }
        if ((label_len & 0xC0U) != 0U || (offset + label_len) > length) {
            return 0U;
        }
        offset += label_len;
    }

    if ((offset + 4U) > length) {
        return 0U;
    }
    return offset + 4U;
}

static ssize_t build_dns_response_packet(const uint8_t *query, size_t query_len, uint8_t *response, size_t response_len)
{
    if (query_len < 12U || response_len < 12U) {
        return -1;
    }

    uint16_t question_count = (uint16_t) ((query[4] << 8) | query[5]);
    if (question_count == 0U) {
        return -1;
    }

    size_t question_end = dns_question_end_offset(query, query_len);
    if (question_end == 0U || question_end > response_len) {
        return -1;
    }

    uint16_t question_type = (uint16_t) ((query[question_end - 4U] << 8) | query[question_end - 3U]);
    bool answer_ipv4 = (question_type == 1U || question_type == 255U);

    size_t required = question_end + (answer_ipv4 ? 16U : 0U);
    if (required > response_len) {
        return -1;
    }

    memcpy(response, query, question_end);
    response[2] = 0x81;
    response[3] = 0x80;
    response[6] = 0x00;
    response[7] = answer_ipv4 ? 0x01 : 0x00;
    response[8] = 0x00;
    response[9] = 0x00;
    response[10] = 0x00;
    response[11] = 0x00;

    if (!answer_ipv4) {
        return (ssize_t) question_end;
    }

    size_t offset = question_end;
    response[offset++] = 0xC0;
    response[offset++] = 0x0C;
    response[offset++] = 0x00;
    response[offset++] = 0x01;
    response[offset++] = 0x00;
    response[offset++] = 0x01;
    response[offset++] = 0x00;
    response[offset++] = 0x00;
    response[offset++] = 0x00;
    response[offset++] = 0x3C;
    response[offset++] = 0x00;
    response[offset++] = 0x04;
    uint32_t ap_ip_host = ntohl(s_ap_ip.addr);
    response[offset++] = (uint8_t) ((ap_ip_host >> 24) & 0xFFU);
    response[offset++] = (uint8_t) ((ap_ip_host >> 16) & 0xFFU);
    response[offset++] = (uint8_t) ((ap_ip_host >> 8) & 0xFFU);
    response[offset++] = (uint8_t) (ap_ip_host & 0xFFU);

    return (ssize_t) offset;
}

static void captive_dns_task(void *arg)
{
    (void) arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Captive DNS socket create failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *) &bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Captive DNS bind failed: errno=%d", errno);
        lwip_close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive DNS ready on " IPSTR ":%d", IP2STR(&s_ap_ip), DNS_SERVER_PORT);

    uint8_t query[DNS_PACKET_MAX_SIZE];
    uint8_t response[DNS_PACKET_MAX_SIZE];
    while (true) {
        struct sockaddr_in source_addr;
        socklen_t source_len = sizeof(source_addr);
        ssize_t received = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *) &source_addr, &source_len);
        if (received <= 0) {
            continue;
        }

        ssize_t response_size = build_dns_response_packet(query, (size_t) received, response, sizeof(response));
        if (response_size <= 0) {
            continue;
        }

        sendto(sock, response, (size_t) response_size, 0, (struct sockaddr *) &source_addr, source_len);
    }
}

esp_err_t start_captive_dns_server(void)
{
    BaseType_t task_created = xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG, "create captive dns task failed");
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    char portal_url[48];
    build_ap_root_url(portal_url, sizeof(portal_url));

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Redirecting to captive portal");
}

static esp_err_t captive_204_handler(httpd_req_t *req)
{
    char portal_url[48];
    build_ap_root_url(portal_url, sizeof(portal_url));

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Captive portal");
}

static esp_err_t captive_hotspot_detect_handler(httpd_req_t *req)
{
    char portal_url[48];
    build_ap_root_url(portal_url, sizeof(portal_url));

    set_common_response_headers(req, "text/html; charset=utf-8");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_hdr(req, "Refresh", "0; url=/");

    char body[192];
    snprintf(body, sizeof(body),
             "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>"
             "Success <A HREF=\"%s\">Continue</A>."
             "</BODY></HTML>",
             portal_url);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t captive_generate_204_handler(httpd_req_t *req)
{
    return captive_204_handler(req);
}

static esp_err_t captive_ncsi_handler(httpd_req_t *req)
{
    return captive_redirect_handler(req);
}

static esp_err_t captive_connecttest_handler(httpd_req_t *req)
{
    return captive_redirect_handler(req);
}

static esp_err_t captive_mobile_connect_handler(httpd_req_t *req)
{
    return captive_redirect_handler(req);
}

static esp_err_t captive_root_fallback_handler(httpd_req_t *req)
{
    return captive_redirect_handler(req);
}

esp_err_t captive_portal_register_handlers(httpd_handle_t server)
{
    const httpd_uri_t captive_hotspot_detect = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = captive_hotspot_detect_handler,
    };
    const httpd_uri_t captive_generate_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = captive_generate_204_handler,
    };
    const httpd_uri_t captive_gen_204 = {
        .uri = "/gen_204",
        .method = HTTP_GET,
        .handler = captive_generate_204_handler,
    };
    const httpd_uri_t captive_ncsi = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = captive_ncsi_handler,
    };
    const httpd_uri_t captive_connecttest = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = captive_connecttest_handler,
    };
    const httpd_uri_t captive_mobile_connect = {
        .uri = "/mobile/status.php",
        .method = HTTP_GET,
        .handler = captive_mobile_connect_handler,
    };
    const httpd_uri_t captive_root_fallback = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = captive_root_fallback_handler,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_hotspot_detect), TAG, "register captive hotspot handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_generate_204), TAG, "register captive generate_204 handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_gen_204), TAG, "register captive gen_204 handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_ncsi), TAG, "register captive ncsi handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_connecttest), TAG, "register captive connecttest handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_mobile_connect), TAG, "register captive mobile status handler failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_root_fallback), TAG, "register captive fallback handler failed");

    return ESP_OK;
}
