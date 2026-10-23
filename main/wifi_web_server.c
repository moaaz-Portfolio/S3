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
