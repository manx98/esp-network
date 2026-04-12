#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wifi_mgr.h"

static const char *TAG = "wifi_mgr";

#define NVS_NS        "wifi_mgr"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAILED_BIT     BIT1
#define WIFI_SCAN_DONE_BIT  BIT2

static EventGroupHandle_t s_wifi_eg;
static esp_netif_t       *s_sta_netif = NULL;
static volatile wifi_mgr_state_t s_state = WIFI_MGR_STATE_DISCONNECTED;
static uint8_t            s_ip[4] = {0};

/* ── Event handlers ── */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        /* nothing – connect is triggered explicitly */
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA connected");
        s_state = WIFI_MGR_STATE_CONNECTING; /* wait for IP */
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "STA disconnected, reason=%d", ev->reason);
        memset(s_ip, 0, sizeof(s_ip));
        s_state = WIFI_MGR_STATE_DISCONNECTED;
        xEventGroupSetBits(s_wifi_eg, WIFI_FAILED_BIT);
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_wifi_eg, WIFI_SCAN_DONE_BIT);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        uint32_t ip = ev->ip_info.ip.addr;
        s_ip[0] = (ip >>  0) & 0xFF;
        s_ip[1] = (ip >>  8) & 0xFF;
        s_ip[2] = (ip >> 16) & 0xFF;
        s_ip[3] = (ip >> 24) & 0xFF;
        s_state = WIFI_MGR_STATE_CONNECTED;
        ESP_LOGI(TAG, "Got IP: %d.%d.%d.%d",
                 s_ip[0], s_ip[1], s_ip[2], s_ip[3]);
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        memset(s_ip, 0, sizeof(s_ip));
        s_state = WIFI_MGR_STATE_DISCONNECTED;
    }
}

/* ── Public API ── */

esp_err_t wifi_mgr_init(void)
{
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_mgr_set_config(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) > WIFI_MGR_SSID_MAX ||
        !password || strlen(password) > WIFI_MGR_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (ret == ESP_OK) ret = nvs_set_str(nvs, NVS_KEY_PASS, password);
    if (ret == ESP_OK) ret = nvs_commit(nvs);
    nvs_close(nvs);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Config saved: SSID=\"%s\"", ssid);
    }
    return ret;
}

esp_err_t wifi_mgr_get_config(wifi_mgr_config_t *out)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    size_t ssid_len = sizeof(out->ssid);
    size_t pass_len = sizeof(out->password);
    ret = nvs_get_str(nvs, NVS_KEY_SSID, out->ssid, &ssid_len);
    if (ret == ESP_OK) {
        ret = nvs_get_str(nvs, NVS_KEY_PASS, out->password, &pass_len);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t wifi_mgr_connect(void)
{
    wifi_mgr_config_t cfg = {0};
    esp_err_t ret = wifi_mgr_get_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No stored config");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid,     cfg.ssid,     sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, cfg.password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_state = WIFI_MGR_STATE_CONNECTING;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        s_state = WIFI_MGR_STATE_FAILED;
    }
    return ret;
}

esp_err_t wifi_mgr_disconnect(void)
{
    esp_err_t ret = esp_wifi_disconnect();
    if (ret == ESP_OK) {
        s_state = WIFI_MGR_STATE_DISCONNECTED;
        memset(s_ip, 0, sizeof(s_ip));
    }
    return ret;
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    out->state = s_state;
    memcpy(out->ip, s_ip, 4);
    out->rssi = 0;
    if (s_state == WIFI_MGR_STATE_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            out->rssi = ap_info.rssi;
        }
    }
}

int wifi_mgr_scan(wifi_mgr_ap_info_t *ap_list, int max_count)
{
    xEventGroupClearBits(s_wifi_eg, WIFI_SCAN_DONE_BIT);

    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);
    if (ret != ESP_OK) return -1;

    /* Wait for scan done (max 10 s) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_SCAN_DONE_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(10000));
    if (!(bits & WIFI_SCAN_DONE_BIT)) {
        esp_wifi_scan_stop();
        return -1;
    }

    uint16_t count = (uint16_t)max_count;
    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * count);
    if (!records) return -1;

    ret = esp_wifi_scan_get_ap_records(&count, records);
    if (ret != ESP_OK) {
        free(records);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        strncpy(ap_list[i].ssid, (char *)records[i].ssid,
                WIFI_MGR_SSID_MAX);
        ap_list[i].ssid[WIFI_MGR_SSID_MAX] = '\0';
        ap_list[i].rssi     = records[i].rssi;
        ap_list[i].authmode = (uint8_t)records[i].authmode;
    }

    free(records);
    return (int)count;
}
