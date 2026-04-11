/*
 * SPDX-FileCopyrightText: 2026 Michael Teeuw
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Internal declarations shared between component source files.
 * Not part of the public API.
 */

#pragma once

#include "wifi_provisioner.h"
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "esp_netif.h"
#include "esp_log.h"

#include <string.h>

/* ── NVS store ──────────────────────────────────────────────────────── */

esp_err_t nvs_store_load(char *ssid, size_t ssid_len,
                         char *password, size_t pass_len);
esp_err_t nvs_store_save(const char *ssid, const char *password);
esp_err_t nvs_store_erase(void);

/* ── WiFi STA ───────────────────────────────────────────────────────── */

/**
 * Callback invoked from the event-loop task when the async connection
 * attempt finishes.  connected=true on success, false on failure.
 */
typedef void (*wifi_sta_result_cb_t)(bool connected);

/**
 * Begin an async STA connection in the current WiFi mode.
 * Registers event handlers and calls esp_wifi_connect(); returns
 * immediately without blocking.  result_cb is called once on outcome.
 * max_retries=0 means a single attempt (no retries).
 */
esp_err_t wifi_sta_connect_async(const char *ssid, const char *password,
                                  uint8_t max_retries,
                                  wifi_sta_result_cb_t result_cb);

/**
 * Cancel any pending async connection attempt.
 * Unregisters event handlers; result_cb will NOT be called.
 */
void wifi_sta_cancel(void);

/* ── WiFi AP ────────────────────────────────────────────────────────── */

esp_err_t wifi_ap_start(const wifi_prov_config_t *config);
esp_err_t wifi_ap_stop(void);
esp_netif_t *wifi_ap_take_sta_netif(void); /* transfer STA netif ownership away from AP module */

/* ── DNS server ─────────────────────────────────────────────────────── */

esp_err_t dns_server_start(void);
esp_err_t dns_server_stop(void);

/* ── HTTP server ────────────────────────────────────────────────────── */

esp_err_t http_server_start(uint16_t port, const wifi_prov_config_t *config);
esp_err_t http_server_stop(void);
