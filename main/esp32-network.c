#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "proto.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

/* ── LED ── */
#define LED_GPIO            GPIO_NUM_15
#define LED_TIMER_PERIOD_US (50 * 1000)   /* 50 ms */

static atomic_int s_data_active = 0;

static void led_timer_cb(void *arg)
{
    if (atomic_exchange(&s_data_active, 0)) {
        gpio_set_level(LED_GPIO, 1);
    } else {
        gpio_set_level(LED_GPIO, 0);
    }
}

/* ── CDC ── */
#define CDC_RX_BUF_SIZE    512
#define CMD_STREAM_SIZE    1024   /* StreamBuffer for RX bytes → cmd task */
#define RESP_BUF_SIZE      (PROTO_MAX_PAYLOAD + 32)

static StreamBufferHandle_t s_cmd_stream;

/* Send a response frame on CDC port 0 */
static void cdc_send_response(uint8_t seq, uint8_t cmd,
                              proto_status_t status,
                              const uint8_t *data, size_t data_len)
{
    uint8_t buf[RESP_BUF_SIZE];
    size_t  len = proto_build_response(seq, cmd, status, data, data_len,
                                       buf, sizeof(buf));
    if (len == 0) return;

    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
    atomic_store(&s_data_active, 1);
}

/* ── Command handlers ── */

static void handle_ping(const proto_frame_t *f)
{
    const uint8_t pong[] = "PONG";
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, pong, sizeof(pong) - 1);
}

static void handle_get_dev_info(const proto_frame_t *f)
{
    char info[64];
    int len = snprintf(info, sizeof(info), "esp32-network v1.0 idf:%s",
                       esp_get_idf_version());
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK,
                      (uint8_t *)info, (size_t)len);
}

static void handle_reset(const proto_frame_t *f)
{
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void handle_wifi_set_config(const proto_frame_t *f)
{
    /* Payload: [ssid_len:1][ssid:N][pass_len:1][pass:M] */
    const uint8_t *p = f->payload;
    size_t         remaining = f->payload_len;

    if (remaining < 1) goto bad_arg;
    uint8_t ssid_len = *p++;  remaining--;
    if (remaining < ssid_len + 1) goto bad_arg;

    char ssid[WIFI_MGR_SSID_MAX + 1] = {0};
    memcpy(ssid, p, ssid_len);
    p += ssid_len;  remaining -= ssid_len;

    uint8_t pass_len = *p++;  remaining--;
    if (remaining < pass_len) goto bad_arg;

    char pass[WIFI_MGR_PASS_MAX + 1] = {0};
    memcpy(pass, p, pass_len);

    esp_err_t ret = wifi_mgr_set_config(ssid, pass);
    cdc_send_response(f->seq, f->cmd,
                      ret == ESP_OK ? PROTO_STATUS_OK : PROTO_STATUS_ERROR,
                      NULL, 0);
    return;

bad_arg:
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_INVALID, NULL, 0);
}

static void handle_wifi_get_config(const proto_frame_t *f)
{
    wifi_mgr_config_t cfg = {0};
    esp_err_t ret = wifi_mgr_get_config(&cfg);
    if (ret != ESP_OK) {
        cdc_send_response(f->seq, f->cmd, PROTO_STATUS_ERROR, NULL, 0);
        return;
    }
    /* Mask password */
    uint8_t  buf[WIFI_MGR_SSID_MAX + WIFI_MGR_PASS_MAX + 4];
    size_t   n = 0;
    uint8_t  ssid_len = (uint8_t)strlen(cfg.ssid);
    uint8_t  pass_len = (uint8_t)strlen(cfg.password);
    buf[n++] = ssid_len;
    memcpy(&buf[n], cfg.ssid, ssid_len);   n += ssid_len;
    buf[n++] = pass_len;
    memset(&buf[n], '*', pass_len);        n += pass_len;
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, buf, n);
}

static void handle_wifi_connect(const proto_frame_t *f)
{
    esp_err_t ret = wifi_mgr_connect();
    cdc_send_response(f->seq, f->cmd,
                      ret == ESP_OK ? PROTO_STATUS_OK : PROTO_STATUS_ERROR,
                      NULL, 0);
}

static void handle_wifi_disconnect(const proto_frame_t *f)
{
    esp_err_t ret = wifi_mgr_disconnect();
    cdc_send_response(f->seq, f->cmd,
                      ret == ESP_OK ? PROTO_STATUS_OK : PROTO_STATUS_ERROR,
                      NULL, 0);
}

static void handle_wifi_get_status(const proto_frame_t *f)
{
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);
    uint8_t buf[6];
    buf[0] = (uint8_t)st.state;
    memcpy(&buf[1], st.ip, 4);
    buf[5] = (uint8_t)st.rssi;   /* int8_t reinterpreted as uint8_t */
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, buf, sizeof(buf));
}

static void handle_wifi_scan(const proto_frame_t *f)
{
    wifi_mgr_ap_info_t *aps = malloc(sizeof(wifi_mgr_ap_info_t) * WIFI_MGR_SCAN_MAX);
    if (!aps) {
        cdc_send_response(f->seq, f->cmd, PROTO_STATUS_ERROR, NULL, 0);
        return;
    }

    int count = wifi_mgr_scan(aps, WIFI_MGR_SCAN_MAX);
    if (count < 0) {
        free(aps);
        cdc_send_response(f->seq, f->cmd, PROTO_STATUS_ERROR, NULL, 0);
        return;
    }

    /* Build payload: [count:1]([ssid_len:1][ssid][rssi:1][auth:1])* */
    uint8_t buf[PROTO_MAX_PAYLOAD];
    size_t  n = 0;
    buf[n++] = (uint8_t)count;
    for (int i = 0; i < count && n < sizeof(buf) - 3; i++) {
        uint8_t slen = (uint8_t)strlen(aps[i].ssid);
        if (n + 1 + slen + 2 > sizeof(buf)) break;
        buf[n++] = slen;
        memcpy(&buf[n], aps[i].ssid, slen);  n += slen;
        buf[n++] = (uint8_t)aps[i].rssi;
        buf[n++] = aps[i].authmode;
    }

    free(aps);
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, buf, n);
}

/* ── Frame dispatcher (runs in cmd_task context) ── */

static void on_frame(const proto_frame_t *f, void *ctx)
{
    ESP_LOGD(TAG, "CMD=0x%02X SEQ=%u payload_len=%u", f->cmd, f->seq, f->payload_len);

    switch ((proto_cmd_t)f->cmd) {
    case CMD_PING:              handle_ping(f);              break;
    case CMD_GET_DEV_INFO:      handle_get_dev_info(f);      break;
    case CMD_RESET:             handle_reset(f);             break;
    case CMD_WIFI_SET_CONFIG:   handle_wifi_set_config(f);   break;
    case CMD_WIFI_GET_CONFIG:   handle_wifi_get_config(f);   break;
    case CMD_WIFI_CONNECT:      handle_wifi_connect(f);      break;
    case CMD_WIFI_DISCONNECT:   handle_wifi_disconnect(f);   break;
    case CMD_WIFI_GET_STATUS:   handle_wifi_get_status(f);   break;
    case CMD_WIFI_SCAN:         handle_wifi_scan(f);         break;
    default:
        ESP_LOGW(TAG, "Unknown CMD=0x%02X", f->cmd);
        cdc_send_response(f->seq, f->cmd, PROTO_STATUS_INVALID, NULL, 0);
        break;
    }
}

/* ── Command processing task ── */

static void cmd_task(void *arg)
{
    proto_parser_t *parser = proto_parser_create(on_frame, NULL);
    uint8_t         buf[CDC_RX_BUF_SIZE];

    while (1) {
        size_t n = xStreamBufferReceive(s_cmd_stream, buf, sizeof(buf),
                                        portMAX_DELAY);
        if (n > 0) {
            proto_parser_feed(parser, buf, n);
        }
    }
}

/* ── USB CDC callbacks ── */

static void usb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    uint8_t   tmp[CDC_RX_BUF_SIZE];
    size_t    rx_size = 0;

    esp_err_t ret = tinyusb_cdcacm_read(itf, tmp, sizeof(tmp), &rx_size);
    if (ret != ESP_OK || rx_size == 0) return;

    atomic_store(&s_data_active, 1);

    /* Forward to command task via StreamBuffer (non-blocking) */
    size_t sent = xStreamBufferSend(s_cmd_stream, tmp, rx_size, 0);
    if (sent < rx_size) {
        ESP_LOGW(TAG, "Stream buffer full, %u byte(s) dropped",
                 (unsigned)(rx_size - sent));
    }
}

static void usb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    atomic_store(&s_data_active, 1);
    ESP_LOGI(TAG, "Line state itf %d: DTR=%d RTS=%d", itf,
             event->line_state_changed_data.dtr,
             event->line_state_changed_data.rts);
}

/* ── app_main ── */

void app_main(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* LED GPIO */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level(LED_GPIO, 0);

    const esp_timer_create_args_t timer_args = {
        .callback = led_timer_cb,
        .name     = "led",
    };
    esp_timer_handle_t led_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &led_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(led_timer, LED_TIMER_PERIOD_US));

    /* WiFi */
    ESP_ERROR_CHECK(wifi_mgr_init());

    /* StreamBuffer for CDC → cmd_task */
    s_cmd_stream = xStreamBufferCreate(CMD_STREAM_SIZE, 1);
    xTaskCreate(cmd_task, "cmd", 4096, NULL, 5, NULL);

    /* TinyUSB */
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port                     = TINYUSB_CDC_ACM_0,
        .callback_rx                  = usb_cdc_rx_callback,
        .callback_rx_wanted_char      = NULL,
        .callback_line_state_changed  = usb_cdc_line_state_changed_callback,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    ESP_LOGI(TAG, "Ready. Waiting for commands on USB CDC.");
}
