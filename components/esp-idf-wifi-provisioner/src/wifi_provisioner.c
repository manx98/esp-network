/*
 * SPDX-FileCopyrightText: 2026 Michael Teeuw
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Main orchestration: boot flow, connect-or-provision logic.
 * All WiFi connection attempts are fully asynchronous — no blocking waits.
 */

#include "wifi_prov_internal.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_prov";

static wifi_prov_config_t  s_config;
static esp_netif_t        *s_sta_netif         = NULL;
static bool                s_connected         = false;
static bool                s_initialized       = false;

/* Credentials stashed from a portal submission, saved to NVS on success. */
static char  s_pending_ssid[33];
static char  s_pending_pass[65];
static bool  s_has_pending_creds = false;

/* Event base declared/defined in http_server.c */
ESP_EVENT_DECLARE_BASE(WIFI_PROV_EVENT);
enum { WIFI_PROV_EVENT_CREDENTIALS_SET };

typedef struct {
    char ssid[33];
    char password[65];
} wifi_prov_creds_t;

/* ── STA connection result (called from event-loop task) ─────────────── */

static void on_sta_result(bool connected)
{
    if (connected) {
        /* Only save credentials that came from a live portal submission. */
        if (s_has_pending_creds) {
            nvs_store_save(s_pending_ssid, s_pending_pass);
            s_has_pending_creds = false;
        }

        /* Tear down portal services now that we have a STA connection. */
        http_server_stop();
        dns_server_stop();

        /* Switch from APSTA to STA-only and take ownership of the STA netif. */
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_sta_netif = wifi_ap_take_sta_netif();

        s_connected = true;
        ESP_LOGI(TAG, "STA connected – portal torn down");
        if (s_config.on_connected) {
            s_config.on_connected();
        }
    } else {
        s_has_pending_creds = false;
        ESP_LOGW(TAG, "Connection failed – portal remains active");
    }
}

/* ── Portal credential event ─────────────────────────────────────────── */

static void on_credentials_set(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    wifi_prov_creds_t *creds = (wifi_prov_creds_t *)data;
    ESP_LOGI(TAG, "Credentials received via portal – SSID: \"%s\"", creds->ssid);

    /* Stash for NVS save when connection succeeds. */
    strncpy(s_pending_ssid, creds->ssid, sizeof(s_pending_ssid) - 1);
    strncpy(s_pending_pass, creds->password, sizeof(s_pending_pass) - 1);
    s_has_pending_creds = true;

    /* Single attempt (0 retries) — the user can resubmit if it fails. */
    wifi_sta_connect_async(creds->ssid, creds->password, 0, on_sta_result);
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t wifi_prov_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_prov_start(const wifi_prov_config_t *config)
{
    ESP_ERROR_CHECK(wifi_prov_init());

    s_config            = *config;
    s_connected         = false;
    s_has_pending_creds = false;

    /* If WiFi is already running (connecting or connected), stop it cleanly. */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi already active (mode=%d) – stopping before portal start …",
                 (int)mode);
        wifi_sta_cancel();
        esp_wifi_disconnect();
        esp_wifi_stop();
        wifi_ap_stop();
        if (s_sta_netif) {
            esp_netif_destroy_default_wifi(s_sta_netif);
            s_sta_netif = NULL;
        }
        esp_wifi_deinit();
    }

    /* Re-register (safe to call even if already registered). */
    esp_event_handler_unregister(WIFI_PROV_EVENT,
                                 WIFI_PROV_EVENT_CREDENTIALS_SET,
                                 on_credentials_set);
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_PROV_EVENT, WIFI_PROV_EVENT_CREDENTIALS_SET,
        on_credentials_set, NULL));

    /* Start AP + captive portal. */
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));

    wifi_ap_start(&s_config);
    dns_server_start();
    http_server_start(s_config.http_port, &s_config);

    if (s_config.on_portal_start) {
        s_config.on_portal_start();
    }

    return ESP_OK;
}

esp_err_t wifi_prov_connect(void)
{
    char ssid[33]     = {0};
    char password[65] = {0};
    esp_err_t err = nvs_store_load(ssid, sizeof(ssid), password, sizeof(password));

    if (err != ESP_OK || ssid[0] == '\0') {
        ESP_LOGI(TAG, "No stored credentials found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Stored credentials found – connecting to \"%s\" …", ssid);
    /* s_has_pending_creds is NOT set here; NVS is already up to date. */
    return wifi_sta_connect_async(ssid, password, s_config.max_retries, on_sta_result);
}

esp_err_t wifi_prov_stop(void)
{
    http_server_stop();
    dns_server_stop();
    wifi_sta_cancel();

    esp_wifi_disconnect();
    esp_wifi_stop();
    wifi_ap_stop();
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    esp_wifi_deinit();

    esp_event_handler_unregister(WIFI_PROV_EVENT,
                                 WIFI_PROV_EVENT_CREDENTIALS_SET,
                                 on_credentials_set);

    s_connected         = false;
    s_has_pending_creds = false;
    return ESP_OK;
}

esp_err_t wifi_prov_erase_credentials(void)
{
    ESP_ERROR_CHECK(wifi_prov_init());
    return nvs_store_erase();
}

bool wifi_prov_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_prov_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    if (!s_connected || !s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_get_ip_info(s_sta_netif, ip_info);
}
