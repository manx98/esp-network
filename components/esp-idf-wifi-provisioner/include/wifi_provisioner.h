/*
 * SPDX-FileCopyrightText: 2026 Michael Teeuw
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback fired when the device successfully connects as a station.
 */
typedef void (*wifi_prov_on_connected_cb_t)(void);

/**
 * Callback fired when a WiFi connection attempt fails (all retries exhausted).
 * The portal remains active so the user can try different credentials.
 */
typedef void (*wifi_prov_on_connect_failed_cb_t)(void);

/**
 * Callback fired when the captive portal AP is started.
 */
typedef void (*wifi_prov_on_portal_start_cb_t)(void);

/**
 * Provisioner configuration.
 * Use WIFI_PROV_DEFAULT_CONFIG() to initialise with Kconfig defaults.
 */
typedef struct {
    const char *ap_ssid;
    const char *ap_password;
    uint8_t     ap_channel;
    uint8_t     ap_max_connections;
    uint8_t     max_retries;
    uint16_t    portal_timeout;          /* seconds, 0 = no timeout */
    uint16_t    http_port;
    const char *page_title;
    const char *portal_header;
    const char *portal_subheader;
    const char *connected_header;
    const char *connected_subheader;
    const char *page_footer;
    wifi_prov_on_connected_cb_t      on_connected;
    wifi_prov_on_connect_failed_cb_t on_connect_failed;
    wifi_prov_on_portal_start_cb_t   on_portal_start;
} wifi_prov_config_t;

#define WIFI_PROV_DEFAULT_CONFIG() {                                        \
    .ap_ssid           = CONFIG_WIFI_PROV_AP_SSID,                          \
    .ap_password       = CONFIG_WIFI_PROV_AP_PASSWORD,                      \
    .ap_channel        = CONFIG_WIFI_PROV_AP_CHANNEL,                       \
    .ap_max_connections = CONFIG_WIFI_PROV_AP_MAX_CONNECTIONS,               \
    .max_retries       = CONFIG_WIFI_PROV_STA_MAX_RETRIES,                  \
    .portal_timeout    = CONFIG_WIFI_PROV_PORTAL_TIMEOUT,                   \
    .http_port         = CONFIG_WIFI_PROV_HTTP_PORT,                        \
    .page_title        = CONFIG_WIFI_PROV_PAGE_TITLE,                      \
    .portal_header     = CONFIG_WIFI_PROV_PORTAL_HEADER,                   \
    .portal_subheader  = CONFIG_WIFI_PROV_PORTAL_SUBHEADER,                \
    .connected_header  = CONFIG_WIFI_PROV_CONNECTED_HEADER,                \
    .connected_subheader = CONFIG_WIFI_PROV_CONNECTED_SUBHEADER,           \
    .page_footer       = CONFIG_WIFI_PROV_PAGE_FOOTER,                     \
    .on_connected      = NULL,                                              \
    .on_connect_failed = NULL,                                              \
    .on_portal_start   = NULL,                                              \
}

/**
 * Initialise NVS, netif and the default event loop.
 * Called automatically by wifi_prov_start() and wifi_prov_erase_credentials().
 * Safe to call multiple times (idempotent).
 */
esp_err_t wifi_prov_init(void);

/**
 * Start the captive-portal AP.
 *
 * Calls wifi_prov_init() automatically if not already done.
 * If WiFi is already active (connecting or connected) it is stopped first.
 * Does NOT attempt a WiFi connection — call wifi_prov_connect() for that.
 */
esp_err_t wifi_prov_start(const wifi_prov_config_t *config);

/**
 * Attempt an async WiFi connection using stored NVS credentials.
 *
 * Must be called after wifi_prov_start().  Returns immediately without
 * blocking.  On success the portal is torn down automatically and the
 * on_connected callback is fired.  On failure the portal remains active.
 *
 * @return ESP_OK          – connection attempt started.
 * @return ESP_ERR_NOT_FOUND – no stored credentials found.
 */
esp_err_t wifi_prov_connect(void);

/**
 * Stop the WiFi provisioner and release all resources.
 */
esp_err_t wifi_prov_stop(void);

/**
 * Erase stored WiFi credentials from NVS.
 */
esp_err_t wifi_prov_erase_credentials(void);

/**
 * Check whether the device is currently connected as a station.
 */
bool wifi_prov_is_connected(void);

/**
 * Retrieve the current station IP information.
 */
esp_err_t wifi_prov_get_ip_info(esp_netif_ip_info_t *ip_info);

#ifdef __cplusplus
}
#endif
