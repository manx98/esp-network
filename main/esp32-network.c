#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "proto.h"
#include "wifi_mgr.h"
#include "cpu_monitor.h"
#include "tcp_mgr.h"

static const char *TAG = "main";

#define LED_NVS_NS   "led_cfg"
#define LED_NVS_KEY  "enabled"

/* ── LED ── */
#define LED_GPIO            GPIO_NUM_15
#define LED_TIMER_PERIOD_US (50 * 1000)   /* 50 ms */

static atomic_int s_data_active = 0;
static atomic_int s_led_enabled = 1;  /* 1 = 开启状态指示，0 = 关闭 */

static void led_timer_cb(void *arg)
{
    if (!atomic_load(&s_led_enabled)) {
        gpio_set_level(LED_GPIO, 0);
        return;
    }
    if (atomic_exchange(&s_data_active, 0)) {
        gpio_set_level(LED_GPIO, 1);
    } else {
        gpio_set_level(LED_GPIO, 0);
    }
}

/* ── CDC ── */
#define CDC_RX_BUF_SIZE    512
/* Match CONFIG_TINYUSB_CDC_TX_BUFSIZE (4096) — write large chunks to amortise
 * per-write overhead and keep the USB TX ring-buffer well-fed. */
#define CDC_TX_CHUNK       4096
/* Larger stream buffer so the USB RX ISR never drops bytes at peak load.
 * Sized to hold ~20 max-payload frames while cmd_task is busy. */
#define CMD_STREAM_SIZE    8192
#define RESP_BUF_SIZE      (PROTO_MAX_PAYLOAD + 32)
#define PUSH_BUF_SIZE      (PROTO_MAX_PAYLOAD + 32)

static StreamBufferHandle_t  s_cmd_stream;
static SemaphoreHandle_t     s_cdc_mu;   /* serialises all CDC writes */

/* Write len bytes to CDC (caller must hold s_cdc_mu).
 * Splits into CDC_TX_CHUNK-sized pieces and flushes each one so the
 * TX ring-buffer never overflows, ensuring all bytes are delivered. */
static void cdc_write_locked(const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        uint32_t available_size = tud_cdc_n_write_available(TINYUSB_CDC_ACM_0);
        if (available_size == 0)
        {
            esp_err_t err = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(200));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "tud_cdcacm_write_queue() failed: %s", esp_err_to_name(err));
                break;
            }
        } else
        {
            if (chunk > available_size)
            {
                chunk = available_size;
            }
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                                        (uint8_t *)(buf + sent),
                                                        chunk);
            sent += chunk;
        }
    }
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(200));
    atomic_store(&s_data_active, 1);
}

/* Send a response frame on CDC port 0 */
static void cdc_send_response(uint8_t seq, uint8_t cmd,
                              proto_status_t status,
                              const uint8_t *data, size_t data_len)
{
    uint8_t buf[RESP_BUF_SIZE];
    size_t  len = proto_build_response(seq, cmd, status, data, data_len,
                                       buf, sizeof(buf));
    if (len == 0) return;

    xSemaphoreTake(s_cdc_mu, portMAX_DELAY);
    cdc_write_locked(buf, len);
    xSemaphoreGive(s_cdc_mu);
}

/* Send an unsolicited push frame (called from tcp_rx_task).
 * Uses heap allocation to avoid blowing the caller's stack. */
static void cdc_send_push(uint8_t cmd, const uint8_t *data, size_t data_len)
{
    uint8_t *buf = malloc(PUSH_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "cdc_send_push: OOM");
        return;
    }
    size_t len = proto_build_push(cmd, data, data_len, buf, PUSH_BUF_SIZE);
    if (len > 0) {
        xSemaphoreTake(s_cdc_mu, portMAX_DELAY);
        cdc_write_locked(buf, len);
        xSemaphoreGive(s_cdc_mu);
    }
    free(buf);
}

/* ── Command handlers ── */

static void handle_led_set(const proto_frame_t *f)
{
    if (f->payload_len < 1) {
        cdc_send_response(f->seq, f->cmd, PROTO_STATUS_INVALID, NULL, 0);
        return;
    }
    uint8_t val = f->payload[0] ? 1 : 0;
    atomic_store(&s_led_enabled, val);

    nvs_handle_t nvs;
    if (nvs_open(LED_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, LED_NVS_KEY, val);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, NULL, 0);
}

static void handle_led_get(const proto_frame_t *f)
{
    uint8_t enabled = (uint8_t)atomic_load(&s_led_enabled);
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, &enabled, 1);
}

static void handle_ping(const proto_frame_t *f)
{
    const uint8_t pong[] = "PONG";
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, pong, sizeof(pong) - 1);
}

static void handle_get_dev_info(const proto_frame_t *f)
{
    /*
     * Payload format:
     *   [fw_ver_len : 1]
     *   [fw_ver     : N]   IDF version string (no null terminator)
     *   [free_heap  : 4]   uint32 LE
     *   [total_heap : 4]   uint32 LE
     *   [min_heap   : 4]   uint32 LE  (minimum since boot)
     *   [cpu_load   : 1]   uint8  0–100 %
     *   [uptime_s   : 4]   uint32 LE  (seconds since boot)
     *   [task_count : 2]   uint16 LE
     */
    const char *ver     = esp_get_idf_version();
    uint8_t     ver_len = (uint8_t)strlen(ver);

    uint32_t free_heap  = esp_get_free_heap_size();
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    uint32_t min_heap   = esp_get_minimum_free_heap_size();
    uint8_t  cpu_load   = cpu_monitor_get_load();
    uint32_t uptime_s   = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint16_t task_count = (uint16_t)uxTaskGetNumberOfTasks();
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    tcp_mgr_get_bytes(&rx_bytes, &tx_bytes);
    /* 1(ver_len) + 64(ver) + 4+4+4(heap) + 1(cpu) + 4(uptime) + 2(tasks) + 8+8(bytes) */
    uint8_t buf[1 + 64 + 4 + 4 + 4 + 1 + 4 + 2 + 8 + 8];
    size_t n = 0;

    buf[n++] = ver_len;
    memcpy(&buf[n], ver, ver_len);  n += ver_len;

#define PUT_U32(v) do { buf[n]=(v)&0xFF; buf[n+1]=((v)>>8)&0xFF; \
                        buf[n+2]=((v)>>16)&0xFF; buf[n+3]=((v)>>24)&0xFF; n+=4; } while(0)

#define PUT_U64(v) do { \
buf[n] = (v) & 0xFF; \
buf[n+1] = ((v) >> 8) & 0xFF; \
buf[n+2] = ((v) >> 16) & 0xFF; \
buf[n+3] = ((v) >> 24) & 0xFF; \
buf[n+4] = ((v) >> 32) & 0xFF; \
buf[n+5] = ((v) >> 40) & 0xFF; \
buf[n+6] = ((v) >> 48) & 0xFF; \
buf[n+7] = ((v) >> 56) & 0xFF; \
n += 8; \
} while(0)

    PUT_U32(free_heap);
    PUT_U32(total_heap);
    PUT_U32(min_heap);
    buf[n++] = cpu_load;
    PUT_U32(uptime_s);
    buf[n++] = (uint8_t)(task_count & 0xFF);
    buf[n++] = (uint8_t)(task_count >> 8);
    PUT_U64(rx_bytes);
    PUT_U64(tx_bytes);
#undef PUT_U32
#undef PUT_U64

    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, buf, n);
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

static void handle_wifi_set_hostname(const proto_frame_t *f)
{
    const uint8_t *p = f->payload;
    size_t rem = f->payload_len;

    if (rem < 1) goto bad_arg;
    uint8_t hlen = *p++;  rem--;
    if (rem < hlen || hlen == 0 || hlen > WIFI_MGR_HOSTNAME_MAX) goto bad_arg;

    char hostname[WIFI_MGR_HOSTNAME_MAX + 1] = {0};
    memcpy(hostname, p, hlen);

    esp_err_t ret = wifi_mgr_set_hostname(hostname);
    cdc_send_response(f->seq, f->cmd,
                      ret == ESP_OK ? PROTO_STATUS_OK : PROTO_STATUS_ERROR,
                      NULL, 0);
    return;

bad_arg:
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_INVALID, NULL, 0);
}

static void handle_wifi_get_hostname(const proto_frame_t *f)
{
    char hostname[WIFI_MGR_HOSTNAME_MAX + 1] = {0};
    esp_err_t ret = wifi_mgr_get_hostname(hostname, sizeof(hostname));
    if (ret != ESP_OK) {
        cdc_send_response(f->seq, f->cmd, PROTO_STATUS_ERROR, NULL, 0);
        return;
    }
    uint8_t hlen = (uint8_t)strlen(hostname);
    uint8_t buf[1 + WIFI_MGR_HOSTNAME_MAX];
    buf[0] = hlen;
    memcpy(&buf[1], hostname, hlen);
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, buf, 1 + hlen);
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

/* ── Proxy relay command handlers ────────────────────────────────────────── */

static void handle_proxy_connect(const proto_frame_t *f)
{
    /* payload: [host_len:1][host:N][port_hi:1][port_lo:1] */
    const uint8_t *p   = f->payload;
    size_t         rem = f->payload_len;

    if (rem < 1) goto bad_arg;
    uint8_t host_len = *p++;  rem--;
    if (rem < (size_t)host_len + 2) goto bad_arg;

    char host[256];
    memcpy(host, p, host_len);
    host[host_len] = '\0';
    p   += host_len;
    rem -= host_len;

    uint16_t port = ((uint16_t)p[0] << 8) | p[1];

    /* Synchronous connect — blocks cmd_task for up to ~10 s. */
    int ret = tcp_mgr_connect(host, port);
    if (ret == -2) {
        cdc_send_response(f->seq, f->cmd, PROTO_STATUS_BUSY, NULL, 0);
        return;
    }
    cdc_send_response(f->seq, f->cmd,
                      ret == 0 ? PROTO_STATUS_OK : PROTO_STATUS_ERROR,
                      NULL, 0);
    return;

bad_arg:
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_INVALID, NULL, 0);
}

static void handle_proxy_send(const proto_frame_t *f)
{
    /* payload: [data...] — raw bytes forwarded to proxy, no response.
     * Errors are reported autonomously via CMD_PROXY_CLOSED_PUSH. */
    if (f->payload_len == 0) return;
    tcp_mgr_send(f->payload, f->payload_len);
}

static void handle_proxy_disconnect(const proto_frame_t *f)
{
    tcp_mgr_disconnect();
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, NULL, 0);
}

static void handle_proxy_get_status(const proto_frame_t *f)
{
    uint8_t status = tcp_mgr_is_connected() ? 1 : 0;
    cdc_send_response(f->seq, f->cmd, PROTO_STATUS_OK, &status, 1);
}

/* ── Frame dispatcher (runs in cmd_task context) ── */

static void on_frame(const proto_frame_t *f, void *ctx)
{
    ESP_LOGI(TAG, "CMD=0x%02X SEQ=%u payload_len=%u", f->cmd, f->seq, f->payload_len);

    switch ((proto_cmd_t)f->cmd) {
    case CMD_PING:               handle_ping(f);               break;
    case CMD_GET_DEV_INFO:       handle_get_dev_info(f);       break;
    case CMD_RESET:              handle_reset(f);              break;
    case CMD_LED_SET:            handle_led_set(f);            break;
    case CMD_LED_GET:            handle_led_get(f);            break;
    case CMD_WIFI_SET_CONFIG:    handle_wifi_set_config(f);    break;
    case CMD_WIFI_GET_CONFIG:    handle_wifi_get_config(f);    break;
    case CMD_WIFI_CONNECT:       handle_wifi_connect(f);       break;
    case CMD_WIFI_DISCONNECT:    handle_wifi_disconnect(f);    break;
    case CMD_WIFI_GET_STATUS:    handle_wifi_get_status(f);    break;
    case CMD_WIFI_SCAN:          handle_wifi_scan(f);          break;
    case CMD_WIFI_SET_HOSTNAME:  handle_wifi_set_hostname(f);  break;
    case CMD_WIFI_GET_HOSTNAME:  handle_wifi_get_hostname(f);  break;
    case CMD_PROXY_CONNECT:      handle_proxy_connect(f);      break;
    case CMD_PROXY_SEND:         handle_proxy_send(f);         break;
    case CMD_PROXY_DISCONNECT:   handle_proxy_disconnect(f);   break;
    case CMD_PROXY_GET_STATUS:   handle_proxy_get_status(f);   break;
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
            ESP_LOGD(TAG, "Received %d bytes", n);
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
    ESP_LOGI(TAG, "Starting...");
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 恢复 LED 开关状态 */
    {
        nvs_handle_t nvs;
        if (nvs_open(LED_NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
            uint8_t val = 1;
            nvs_get_u8(nvs, LED_NVS_KEY, &val);
            nvs_close(nvs);
            atomic_store(&s_led_enabled, val);
        }
    }

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

    /* CPU monitor */
    cpu_monitor_init();

    /* WiFi */
    ESP_ERROR_CHECK(wifi_mgr_init());

    /* CDC write mutex */
    s_cdc_mu = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_cdc_mu ? ESP_OK : ESP_ERR_NO_MEM);

    /* StreamBuffer for CDC → cmd_task */
    s_cmd_stream = xStreamBufferCreate(CMD_STREAM_SIZE, 1);
    xTaskCreate(cmd_task, "cmd", 8192, NULL, 5, NULL);

    /* TCP manager (SOCKS5 tunnelling) */
    tcp_mgr_init(cdc_send_push);
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
