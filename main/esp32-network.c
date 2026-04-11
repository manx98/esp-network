#include <stdio.h>
#include <string.h>
#include <u8g2_esp32_hal.h>
#include <soc/gpio_num.h>
#include <iot_button.h>
#include <button_gpio.h>
#include <wifi_provisioner.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

/* ── Hardware ────────────────────────────────────────────────────────── */

#define BUTTON_GPIO_NUM     GPIO_NUM_21
#define BUTTON_ACTIVE_LEVEL 1

/* ── Globals ─────────────────────────────────────────────────────────── */

static const char *TAG = "main";

static u8g2_t              u8g2;
static wifi_prov_config_t  wf_config = WIFI_PROV_DEFAULT_CONFIG();
static SemaphoreHandle_t   s_disp_mutex;
static QueueHandle_t       s_cmd_queue;

/* ── App state ───────────────────────────────────────────────────────── */

typedef enum {
    APP_STATE_IDLE,
    APP_STATE_CONNECTING,   /* wifi_prov_connect() in progress */
    APP_STATE_CONNECTED,
    APP_STATE_PORTAL,       /* captive portal AP running */
    APP_STATE_RETRY_WAIT,   /* connect failed, counting down before next retry */
} app_state_t;

static volatile app_state_t s_state = APP_STATE_IDLE;

/* Tick count when entering CONNECTING; safety-net timeout per attempt. */
static TickType_t s_connect_start_tick = 0;
#define CONNECT_TIMEOUT_MS  30000

/* Tick count when entering RETRY_WAIT; controls next retry. */
static TickType_t s_retry_start_tick = 0;
#define RETRY_INTERVAL_MS   30000

/* IP cached at connect-time; refreshed by periodic RSSI updates */
static char s_connected_ip[20] = {0};

/* ── Command queue (all WiFi control runs from the main task) ─────────── */

typedef enum {
    CMD_START,           /* button: try stored creds */
    CMD_STOP,            /* button: stop everything  */
    CMD_RESET,           /* button: erase creds + open portal */
    CMD_CONNECT_FAILED,  /* callback: connection attempt failed */
    CMD_AUTH_FAILED,     /* callback: portal submission failed, stay on portal */
} app_cmd_t;

/* ── Display ─────────────────────────────────────────────────────────── */

/*
 * General layout (5×7 font, 128×64 display):
 *   y=10  line1  – title / status
 *   y=22  line2  – detail (IP, SSID, …)
 *   y=34  line3  – optional extra info
 *   ────  y=48   – separator line
 *   y=62  hint   – button guide
 *
 * Connected layout adds signal bars at top-right (x=111..126, y=2..12):
 *   4 bars, bottom-aligned, filled = strong / outline = weak
 */

/* Called with s_disp_mutex already held. */
static void draw_signal_bars(int rssi)
{
    /* How many bars to fill: ≥-55 → 4, ≥-65 → 3, ≥-75 → 2, ≥-85 → 1, else 0 */
    int filled;
    if      (rssi >= -55) filled = 4;
    else if (rssi >= -65) filled = 3;
    else if (rssi >= -75) filled = 2;
    else if (rssi >= -85) filled = 1;
    else                  filled = 0;

    /* 4 bars × 3 px wide, 1 px gap; bottom edge at y=12, heights 4/6/8/10 */
    static const struct { uint8_t x; uint8_t y; uint8_t h; } bars[4] = {
        {111,  8, 4},
        {115,  6, 6},
        {119,  4, 8},
        {123,  2, 10},
    };
    for (int i = 0; i < 4; i++) {
        if (i < filled) {
            u8g2_DrawBox(&u8g2, bars[i].x, bars[i].y, 3, bars[i].h);
        } else {
            u8g2_DrawFrame(&u8g2, bars[i].x, bars[i].y, 3, bars[i].h);
        }
    }
}

static void display_show(const char *line1, const char *line2,
                          const char *line3, const char *hint)
{
    if (xSemaphoreTake(s_disp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    u8g2_ClearBuffer(&u8g2);
    if (line1) u8g2_DrawStr(&u8g2, 0, 10, line1);
    if (line2) u8g2_DrawStr(&u8g2, 0, 22, line2);
    if (line3) u8g2_DrawStr(&u8g2, 0, 34, line3);
    if (hint) {
        u8g2_DrawHLine(&u8g2, 0, 48, 128);
        u8g2_DrawStr(&u8g2, 0, 62, hint);
    }
    u8g2_SendBuffer(&u8g2);
    xSemaphoreGive(s_disp_mutex);
}

/* Connected-specific display: signal bars + IP + RSSI value */
static void display_show_connected(int rssi)
{
    char rssi_str[14];
    snprintf(rssi_str, sizeof(rssi_str), "RSSI: %d dBm", rssi);

    if (xSemaphoreTake(s_disp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    u8g2_ClearBuffer(&u8g2);
    u8g2_DrawStr(&u8g2,  0, 10, "Connected!");
    draw_signal_bars(rssi);                      /* top-right corner */
    u8g2_DrawStr(&u8g2,  0, 22, s_connected_ip);
    u8g2_DrawStr(&u8g2,  0, 34, rssi_str);
    u8g2_DrawHLine(&u8g2, 0, 48, 128);
    u8g2_DrawStr(&u8g2,  0, 62, "[Hold 3s] Reset");
    u8g2_SendBuffer(&u8g2);
    xSemaphoreGive(s_disp_mutex);
}

/* Retry-wait display: show remaining seconds until next attempt. */
static void display_show_retry(uint32_t remaining_s)
{
    char countdown[20];
    snprintf(countdown, sizeof(countdown), "Retry in %lus...", (unsigned long)remaining_s);
    display_show("Connect Failed", countdown, NULL, "[Hold 3s] Reset WiFi");
}

/* ── WiFi callbacks (event-loop task — only update display / post cmds) ── */

static void on_connected(void)
{
    esp_netif_ip_info_t ip = {};
    strncpy(s_connected_ip, "No IP", sizeof(s_connected_ip));
    if (wifi_prov_get_ip_info(&ip) == ESP_OK) {
        snprintf(s_connected_ip, sizeof(s_connected_ip), IPSTR, IP2STR(&ip.ip));
    }

    wifi_ap_record_t ap_info = {};
    int rssi = -100;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    ESP_LOGI(TAG, "WiFi connected – IP: %s  RSSI: %d dBm", s_connected_ip, rssi);
    s_state = APP_STATE_CONNECTED;
    display_show_connected(rssi);
}

static void on_connect_failed(void)
{
    app_cmd_t cmd;
    if (s_state == APP_STATE_CONNECTING) {
        /* Stored-creds connect failed — schedule a background retry. */
        ESP_LOGW(TAG, "Connection failed – will retry in %d s",
                 RETRY_INTERVAL_MS / 1000);
        cmd = CMD_CONNECT_FAILED;
    } else {
        /* Portal submission failed — stay on portal, notify user. */
        ESP_LOGW(TAG, "Portal auth failed");
        display_show("Auth Failed", "Wrong password?", "Try again", "[Click] Stop");
        cmd = CMD_AUTH_FAILED;
    }
    xQueueSend(s_cmd_queue, &cmd, 0);
}

static void on_portal_start(void)
{
    /* Display already set before wifi_prov_start() is called from main task. */
}

static void on_credentials(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Portal credentials received – connecting to \"%s\"", ssid);
    display_show("Connecting...", ssid, NULL, "[Click] Stop");
    wifi_prov_connect_with_creds(ssid, password);
}

/* ── Button callbacks (timer task — only post commands) ──────────────── */

static void button_single_click_cb(void *arg, void *usr_data)
{
    app_cmd_t cmd = (s_state == APP_STATE_IDLE) ? CMD_START : CMD_STOP;
    ESP_LOGI(TAG, "Single click → %s", cmd == CMD_START ? "START" : "STOP");
    xQueueSend(s_cmd_queue, &cmd, 0);
}

static void button_long_press_cb(void *arg, void *usr_data)
{
    ESP_LOGI(TAG, "Long press → RESET");
    app_cmd_t cmd = CMD_RESET;
    xQueueSend(s_cmd_queue, &cmd, 0);
}

/* ── Helpers called from main task only ──────────────────────────────── */

static void do_start_portal(void)
{
    s_state = APP_STATE_PORTAL;
    display_show("Portal Active", wf_config.ap_ssid,
                 "Connect & open", "[Click] Stop");
    wifi_prov_start(&wf_config);
}

static void do_connect(void)
{
    s_state = APP_STATE_CONNECTING;
    s_connect_start_tick = xTaskGetTickCount();
    display_show("Connecting...", "Using saved creds", NULL, "[Click] Stop");

    esp_err_t err = wifi_prov_connect(&wf_config);
    if (err == ESP_ERR_NOT_FOUND) {
        /* No stored credentials — tell user to long-press for setup. */
        ESP_LOGI(TAG, "No stored credentials");
        s_state = APP_STATE_IDLE;
        display_show("No Credentials", "Hold to setup WiFi", NULL, "[Hold 3s] Setup");
    } else if (err != ESP_OK) {
        /* Immediate failure — fall into the retry path. */
        app_cmd_t fc = CMD_CONNECT_FAILED;
        xQueueSend(s_cmd_queue, &fc, 0);
    }
    /* ESP_OK → async; on_connected / on_connect_failed will fire. */
}

/* ── Init ────────────────────────────────────────────────────────────── */

static void init_u8g2(void)
{
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = GPIO_NUM_8;
    hal.bus.i2c.scl = GPIO_NUM_9;
    u8g2_esp32_hal_init(hal);
    u8x8_SetI2CAddress(&u8g2.u8x8, 0x78);
    u8g2_Setup_sh1106_i2c_128x64_noname_f(&u8g2, U8G2_R0,
        u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
    u8g2_SetFont(&u8g2, u8g2_font_5x7_t_cyrillic);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_ClearDisplay(&u8g2);
}

/* ── Main ────────────────────────────────────────────────────────────── */

void app_main(void)
{
    s_disp_mutex = xSemaphoreCreateMutex();
    s_cmd_queue  = xQueueCreate(8, sizeof(app_cmd_t));

    init_u8g2();

    /* Configure provisioner */
    wf_config.ap_ssid           = "MyDevice-Setup";
    wf_config.on_connected      = on_connected;
    wf_config.on_connect_failed = on_connect_failed;
    wf_config.on_portal_start   = on_portal_start;
    wf_config.on_credentials    = on_credentials;

    /* Button */
    const button_config_t btn_cfg = {
        .long_press_time  = 3000,
        .short_press_time = 100,
    };
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num     = BUTTON_GPIO_NUM,
        .active_level = BUTTON_ACTIVE_LEVEL,
        .disable_pull = false,
    };
    button_handle_t btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &btn));
    iot_button_register_cb(btn, BUTTON_SINGLE_CLICK,   NULL, button_single_click_cb, NULL);
    iot_button_register_cb(btn, BUTTON_LONG_PRESS_UP,  NULL, button_long_press_cb,   NULL);

    ESP_ERROR_CHECK(wifi_prov_init());

    display_show("WiFi Provisioner", "Ready", NULL, "[Click] Start");
    ESP_LOGI(TAG, "Ready – press GPIO%d to start", BUTTON_GPIO_NUM);

    /* ── Command loop ─────────────────────────────────────────────────── */
    app_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(2000)) != pdTRUE) {
            /* Timeout tick ─────────────────────────────────────────────── */

            /* Refresh signal strength when connected. */
            if (s_state == APP_STATE_CONNECTED) {
                wifi_ap_record_t ap_info = {};
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    display_show_connected(ap_info.rssi);
                }
            }

            /* Safety-net: if stuck in CONNECTING too long, treat as failure. */
            if (s_state == APP_STATE_CONNECTING &&
                (xTaskGetTickCount() - s_connect_start_tick) >=
                    pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
                ESP_LOGW(TAG, "Connection timeout – scheduling retry");
                wifi_prov_stop();
                cmd = CMD_CONNECT_FAILED;
                goto handle_cmd;
            }

            /* Retry countdown: fire connect when interval elapses. */
            if (s_state == APP_STATE_RETRY_WAIT) {
                TickType_t elapsed = xTaskGetTickCount() - s_retry_start_tick;
                if (elapsed >= pdMS_TO_TICKS(RETRY_INTERVAL_MS)) {
                    ESP_LOGI(TAG, "Retrying WiFi connection…");
                    do_connect();
                } else {
                    uint32_t remaining_ms = RETRY_INTERVAL_MS
                                           - (uint32_t)pdTICKS_TO_MS(elapsed);
                    display_show_retry((remaining_ms + 999) / 1000);
                }
            }

            continue;
        }

handle_cmd:
        switch (cmd) {

        case CMD_START:
            do_connect();
            break;

        case CMD_STOP:
            wifi_prov_stop();
            s_state = APP_STATE_IDLE;
            display_show("Idle", "", NULL, "[Click] Start");
            break;

        case CMD_RESET:
            wifi_prov_stop();
            wifi_prov_erase_credentials();
            do_start_portal();
            break;

        case CMD_CONNECT_FAILED:
            /* Guard: only enter retry-wait if we were actually connecting.
               Prevents a stale event from firing after CMD_STOP. */
            if (s_state == APP_STATE_CONNECTING) {
                s_state = APP_STATE_RETRY_WAIT;
                s_retry_start_tick = xTaskGetTickCount();
                display_show_retry(RETRY_INTERVAL_MS / 1000);
            }
            break;

        case CMD_AUTH_FAILED:
            /* Portal stays active; display already updated in callback. */
            s_state = APP_STATE_PORTAL;
            break;
        }
    }
}
