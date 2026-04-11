/*
 * SPDX-FileCopyrightText: 2026 Michael Teeuw
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Station (STA) async connect logic.
 * No blocking waits — results are delivered via wifi_sta_result_cb_t.
 */

#include "wifi_prov_internal.h"
#include "esp_wifi.h"
#include "esp_event.h"

static const char *TAG = "wifi_prov_sta";

static wifi_sta_result_cb_t              s_result_cb   = NULL;
static esp_event_handler_instance_t      s_wifi_handler = NULL;
static esp_event_handler_instance_t      s_ip_handler   = NULL;
static uint8_t  s_retries;
static uint8_t  s_max_retries;
static uint32_t s_connect_gen  = 0;   /* generation counter — detects stale events */

/* ── Handler cleanup ─────────────────────────────────────────────────── */

static void cleanup_handlers(void)
{
    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT,
            IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }
}

/* ── WiFi / IP event handler ─────────────────────────────────────────── */

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    /* Ignore events from a previous connection generation (stale disconnect
       events left in the event queue after stop_wifi + reinit). */
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    if (gen != s_connect_gen) {
        ESP_LOGD(TAG, "Ignoring stale event (gen %lu vs current %lu)",
                 (unsigned long)gen, (unsigned long)s_connect_gen);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < s_max_retries) {
            s_retries++;
            ESP_LOGI(TAG, "Retry %d/%d …", s_retries, s_max_retries);
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_wifi_connect() failed on retry: %s",
                         esp_err_to_name(err));
                /* Fall through to failure path below */
            } else {
                return;
            }
        }
        ESP_LOGW(TAG, "Connection failed after %d retries", s_max_retries);
        cleanup_handlers();
        wifi_sta_result_cb_t cb = s_result_cb;
        s_result_cb = NULL;
        if (cb) cb(false);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected – IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retries = 0;
        cleanup_handlers();
        wifi_sta_result_cb_t cb = s_result_cb;
        s_result_cb = NULL;
        if (cb) cb(true);
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t wifi_sta_connect_async(const char *ssid, const char *password,
                                  uint8_t max_retries,
                                  wifi_sta_result_cb_t result_cb)
{
    s_retries     = 0;
    s_max_retries = max_retries;
    s_result_cb   = result_cb;
    s_connect_gen++;          /* new generation — stale events will be ignored */

    void *gen_arg = (void *)(uintptr_t)s_connect_gen;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
        event_handler, gen_arg, &s_wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        event_handler, gen_arg, &s_ip_handler));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password,
            sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Connecting to \"%s\" (max_retries=%d) …", ssid, max_retries);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        cleanup_handlers();
        s_result_cb = NULL;
        if (result_cb) result_cb(false);
        return err;
    }
    return ESP_OK;
}

void wifi_sta_cancel(void)
{
    cleanup_handlers();
    s_result_cb = NULL;
}
