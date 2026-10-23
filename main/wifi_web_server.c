/*
 * S3 Sense / DrWalls browser interface.
 * The UI is embedded from dashboard.html so it is available directly from the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi_sensing.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_web_server.h"
#include "drwalls_wifi.h"

static const char *TAG = "s3sense_web";
static esp_wifi_sensing_fsm_handle_t s_fsm = NULL;
static uint8_t s_ap_mac[6] = {0};
static volatile bool s_motion_active = false;
static volatile int64_t s_last_motion_us = 0;
static uint32_t s_display_hold_ms = 3000;
static esp_wifi_sensing_fsm_channel_config_t s_channel_cfg;
static uint32_t s_ping_frequency_hz = 2;


/*
 * Update the motion state used by /api/status.
 * Keep the last-motion timestamp so the UI can hold the active state
 * for s_display_hold_ms after motion stops.
 */
void wifi_web_server_set_motion(bool active)
{
    s_motion_active = active;

    if (active) {
        s_last_motion_us = esp_timer_get_time();
    }
}

static const uint8_t s_csi_peer_1[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t s_csi_peer_2[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x01};
#define SETTINGS_NAMESPACE "s3sense_cfg"

extern const char dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const char dashboard_html_end[] asm("_binary_dashboard_html_end");

static void load_saved_settings(void)
{
    if (esp_wifi_sensing_fsm_get_channel_config(s_fsm, s_ap_mac, &s_channel_cfg) != ESP_OK) {
        s_channel_cfg = (esp_wifi_sensing_fsm_channel_config_t)DEFAULT_ESP_WIFI_SENSING_FSM_CHANNEL_CONFIG();
        s_channel_cfg.sensitivity = 0.75f;
    }
    if (esp_wifi_sensing_fsm_get_ping_frequency_hz(s_fsm, &s_ping_frequency_hz) != ESP_OK || s_ping_frequency_hz == 0) {
        s_ping_frequency_hz = 2;
    }

    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint16_t u16 = 0;
    uint32_t u32 = 0;
    if (nvs_get_u16(nvs, "motion_sens", &u16) == ESP_OK) s_channel_cfg.sensitivity = (float)u16 / 1000.0f;
    if (nvs_get_u16(nvs, "presence_sens", &u16) == ESP_OK) s_channel_cfg.presence_sensitivity = (float)u16 / 1000.0f;
    if (nvs_get_u16(nvs, "jitter_min", &u16) == ESP_OK) s_channel_cfg.active_jitter_min = (float)u16 / 1000.0f;
    if (nvs_get_u32(nvs, "filter_ms", &u32) == ESP_OK) s_channel_cfg.active_filter_ms = u32;
    if (nvs_get_u32(nvs, "ping_hz", &u32) == ESP_OK && u32 >= 1 && u32 <= 20) s_ping_frequency_hz = u32;
    if (nvs_get_u32(nvs, "display_ms", &u32) == ESP_OK && u32 <= 30000) s_display_hold_ms = u32;
    nvs_close(nvs);
}

static esp_err_t apply_channel_settings(void)
{
    const uint8_t *peers[] = {s_ap_mac, s_csi_peer_1, s_csi_peer_2};
    for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); ++i) {
        esp_err_t err = esp_wifi_sensing_fsm_set_channel_config(s_fsm, peers[i], &s_channel_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not apply sensing settings to channel %u: %s", (unsigned)i, esp_err_to_name(err));
            if (i == 0) return err;
        }
    }
    esp_err_t ping_err = esp_wifi_sensing_fsm_set_ping_frequency_hz(s_fsm, s_ping_frequency_hz);
    if (ping_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not update ping frequency: %s", esp_err_to_name(ping_err));
        return ping_err;
    }
    return ESP_OK;
}

static esp_err_t save_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    uint16_t motion_sens = (uint16_t)(s_channel_cfg.sensitivity * 1000.0f + 0.5f);
    uint16_t presence_sens = (uint16_t)(s_channel_cfg.presence_sensitivity * 1000.0f + 0.5f);
    uint16_t jitter_min = (uint16_t)(s_channel_cfg.active_jitter_min * 1000.0f + 0.5f);
    err = nvs_set_u16(nvs, "motion_sens", motion_sens);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "presence_sens", presence_sens);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "jitter_min", jitter_min);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "filter_ms", s_channel_cfg.active_filter_ms);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "ping_hz", s_ping_frequency_hz);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "display_ms", s_display_hold_ms);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, dashboard_html_start, dashboard_html_end - dashboard_html_start);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char json[128];
    int64_t now = esp_timer_get_time();
    bool display_active = s_motion_active || (s_last_motion_us > 0 && (now - s_last_motion_us) < ((int64_t)s_display_hold_ms * 1000LL));
    snprintf(json, sizeof(json), "{\"motion\":%s,\"display_hold_ms\":%lu}",
             display_active ? "true" : "false", (unsigned long)s_display_hold_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"sensitivity\":%.3f,\"presence_sensitivity\":%.3f,\"active_jitter_min\":%.3f,\"active_filter_ms\":%lu,\"ping_frequency_hz\":%lu,\"display_hold_ms\":%lu}",
             (double)s_channel_cfg.sensitivity,
             (double)s_channel_cfg.presence_sensitivity,
             (double)s_channel_cfg.active_jitter_min,
             (unsigned long)s_channel_cfg.active_filter_ms,
             (unsigned long)s_ping_frequency_hz,
             (unsigned long)s_display_hold_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static bool query_float(const char *query, const char *key, float *out)
{
    char value[32] = {0};
    char *end = NULL;
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) return false;
    float parsed = strtof(value, &end);
    if (end == value || *end != '\0' || parsed != parsed) return false;
    *out = parsed;
    return true;
}

static bool query_u32(const char *query, const char *key, uint32_t *out)
{
    char value[32] = {0};
    char *end = NULL;
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) return false;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > 30000UL) return false;
    *out = (uint32_t)parsed;
    return true;
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing settings");
        return ESP_FAIL;
    }

    esp_wifi_sensing_fsm_channel_config_t next = s_channel_cfg;
    uint32_t ping_hz = s_ping_frequency_hz;
    uint32_t display_ms = s_display_hold_ms;
    if (!query_float(query, "sensitivity", &next.sensitivity) ||
        !query_float(query, "presence_sensitivity", &next.presence_sensitivity) ||
        !query_float(query, "active_jitter_min", &next.active_jitter_min) ||
        !query_u32(query, "active_filter_ms", &next.active_filter_ms) ||
        !query_u32(query, "ping_frequency_hz", &ping_hz) ||
        !query_u32(query, "display_hold_ms", &display_ms) ||
        next.sensitivity < 0.001f || next.sensitivity > 1.0f ||
        next.presence_sensitivity < 0.0f || next.presence_sensitivity > 1.0f ||
        next.active_jitter_min < 0.0f || next.active_jitter_min > 1.0f ||
        next.active_filter_ms > 10000 || ping_hz < 1 || ping_hz > 20 || display_ms > 30000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Settings out of range");
        return ESP_FAIL;
    }

    esp_wifi_sensing_fsm_channel_config_t old_cfg = s_channel_cfg;
    uint32_t old_ping = s_ping_frequency_hz;
    uint32_t old_display = s_display_hold_ms;
    s_channel_cfg = next;
    s_ping_frequency_hz = ping_hz;
    s_display_hold_ms = display_ms;
    esp_err_t err = apply_channel_settings();
    if (err == ESP_OK) err = save_settings();
    if (err != ESP_OK) {
        s_channel_cfg = old_cfg;
        s_ping_frequency_hz = old_ping;
        s_display_hold_ms = old_display;
        (void)apply_channel_settings();
        ESP_LOGE(TAG, "Failed to apply/save settings: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");
        return err;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    ESP_LOGI(TAG, "Settings saved: motion=%.3f presence=%.3f jitter=%.3f filter=%lu ping=%lu hold=%lu",
             (double)s_channel_cfg.sensitivity, (double)s_channel_cfg.presence_sensitivity,
             (double)s_channel_cfg.active_jitter_min, (unsigned long)s_channel_cfg.active_filter_ms,
             (unsigned long)s_ping_frequency_hz, (unsigned long)s_display_hold_ms);
    return ESP_OK;
}

static esp_err_t reset_baseline_handler(httpd_req_t *req)
{
    esp_err_t err = esp_wifi_sensing_fsm_control(s_fsm, ESP_WIFI_SENSING_FSM_CTRL_RESET_BASELINE, NULL);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not reset baseline");
        return err;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"success\":true}");
}

static esp_err_t calibration_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char action[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "action", action, sizeof(action)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use action=start|stop|remove");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    float wander = 0.0f, jitter = 0.0f;
    if (strcmp(action, "start") == 0) {
        err = esp_wifi_sensing_fsm_train_start(s_fsm, s_ap_mac);
    } else if (strcmp(action, "stop") == 0) {
        err = esp_wifi_sensing_fsm_train_stop(s_fsm, s_ap_mac, &wander, &jitter);
    } else if (strcmp(action, "remove") == 0) {
        err = esp_wifi_sensing_fsm_train_remove(s_fsm, s_ap_mac);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown calibration action");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return err;
    }
    char response[128];
    snprintf(response, sizeof(response), "{\"success\":true,\"action\":\"%s\",\"wander_threshold\":%.5f,\"jitter_threshold\":%.5f}", action, (double)wander, (double)jitter);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reset_wifi_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Wi-Fi reset requested");
    esp_err_t err = drwalls_wifi_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase Wi-Fi credentials: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to reset Wi-Fi");
        return err;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

#define REGISTER_ROUTE(server_handle, route_name) do { \
    err = httpd_register_uri_handler((server_handle), &(route_name)); \
    if (err != ESP_OK) { ESP_LOGE(TAG, "Failed to register %s: %s", (route_name).uri, esp_err_to_name(err)); return err; } \
} while (0)

esp_err_t wifi_web_server_start(esp_wifi_sensing_fsm_handle_t fsm, const uint8_t ap_mac[6])
{
    s_fsm = fsm;
    memcpy(s_ap_mac, ap_mac, sizeof(s_ap_mac));
    load_saved_settings();
    (void)apply_channel_settings();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_LOGI(TAG, "Starting S3 Sense HTTP server");
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL};
    static const httpd_uri_t status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL};
    static const httpd_uri_t settings_get = {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler, .user_ctx = NULL};
    static const httpd_uri_t settings_post = {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler, .user_ctx = NULL};
    static const httpd_uri_t reset_baseline = {.uri = "/api/reset-baseline", .method = HTTP_POST, .handler = reset_baseline_handler, .user_ctx = NULL};
    static const httpd_uri_t calibration = {.uri = "/api/calibration", .method = HTTP_POST, .handler = calibration_handler, .user_ctx = NULL};
    static const httpd_uri_t reset_wifi = {.uri = "/api/reset-wifi", .method = HTTP_POST, .handler = reset_wifi_handler, .user_ctx = NULL};

    REGISTER_ROUTE(server, root);
    REGISTER_ROUTE(server, status);
    REGISTER_ROUTE(server, settings_get);
    REGISTER_ROUTE(server, settings_post);
    REGISTER_ROUTE(server, reset_baseline);
    REGISTER_ROUTE(server, calibration);
    REGISTER_ROUTE(server, reset_wifi);

    ESP_LOGI(TAG, "S3 Sense interface ready at http://drwalls.local");
    return ESP_OK;
