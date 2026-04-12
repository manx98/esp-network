#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define WIFI_MGR_SSID_MAX  32
#define WIFI_MGR_PASS_MAX  64
#define WIFI_MGR_SCAN_MAX  20

typedef enum {
    WIFI_MGR_STATE_DISCONNECTED = 0,
    WIFI_MGR_STATE_CONNECTING,
    WIFI_MGR_STATE_CONNECTED,
    WIFI_MGR_STATE_FAILED,
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    uint8_t          ip[4];   /* IPv4 in network order, valid when CONNECTED */
    int8_t           rssi;    /* signal strength in dBm, valid when CONNECTED */
} wifi_mgr_status_t;

typedef struct {
    char ssid[WIFI_MGR_SSID_MAX + 1];
    char password[WIFI_MGR_PASS_MAX + 1];
} wifi_mgr_config_t;

typedef struct {
    char    ssid[WIFI_MGR_SSID_MAX + 1];
    int8_t  rssi;
    uint8_t authmode;   /* wifi_auth_mode_t value */
} wifi_mgr_ap_info_t;

/**
 * Initialize netif, event loop, and WiFi driver.
 * Call once before any other wifi_mgr_* function.
 * nvs_flash_init() must have been called before this.
 */
esp_err_t wifi_mgr_init(void);

/** Persist SSID + password to NVS. Does not connect. */
esp_err_t wifi_mgr_set_config(const char *ssid, const char *password);

/** Read stored credentials from NVS. Returns ESP_ERR_NVS_NOT_FOUND if not set. */
esp_err_t wifi_mgr_get_config(wifi_mgr_config_t *out);

/** Connect using stored credentials. Non-blocking; check status with wifi_mgr_get_status(). */
esp_err_t wifi_mgr_connect(void);

/** Disconnect from AP. */
esp_err_t wifi_mgr_disconnect(void);

/** Get current connection state and IP address. */
void wifi_mgr_get_status(wifi_mgr_status_t *out);

/**
 * Blocking Wi-Fi scan. Fills ap_list with up to max_count entries.
 * Returns number of APs found, or -1 on error.
 */
int wifi_mgr_scan(wifi_mgr_ap_info_t *ap_list, int max_count);
