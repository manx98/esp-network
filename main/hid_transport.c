#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "tusb.h"
#include "class/hid/hid_device.h"
#include "hid_transport.h"

static const char *TAG = "hid";

/* ── TX configuration ── */
#define TX_STREAM_SIZE  8192
#define RX_STREAM_SIZE  8192
#define TX_TASK_STACK   4096
#define TX_TASK_PRIO    5

/* ── Handles ── */
StreamBufferHandle_t hid_rx_stream;       /* tud_hid_set_report_cb → cmd_task */
static StreamBufferHandle_t s_tx_stream;  /* hid_transport_write → hid_tx_task */
static SemaphoreHandle_t    s_tx_mu;      /* multi-writer protection            */

/* ── USB descriptors ─────────────────────────────────────────────────────── */

static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(HID_RPT_SIZE)
};

/*
 * Full-speed HID configuration descriptor:
 *   - 1 interface (HID generic IN+OUT)
 *   - EP 0x01 OUT (host→device), EP 0x81 IN (device→host)
 *   - 64-byte reports, 1 ms polling interval (fastest allowed for FS)
 */
static const uint8_t s_hid_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0,
                          (uint16_t)(TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN),
                          0x00, 100),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE,
                              sizeof(s_hid_report_desc),
                              0x01, 0x81,
                              HID_RPT_SIZE, 1)
};

const uint8_t *hid_report_desc(void)      { return s_hid_report_desc; }
size_t         hid_report_desc_size(void) { return sizeof(s_hid_report_desc); }
const uint8_t *hid_config_desc(void)      { return s_hid_config_desc; }

/* ── TX task ─────────────────────────────────────────────────────────────── */
/*
 * Drains s_tx_stream in 63-byte chunks.  Each chunk is wrapped in a 64-byte
 * HID IN report:  report[0]=len, report[1..len]=data, report[len+1..63]=0.
 *
 * Waits (1 ms loops) when the device is not mounted or the IN endpoint is
 * busy (previous report not yet read by host).
 */
static void hid_tx_task(void *arg)
{
    (void)arg;
    uint8_t report[HID_RPT_SIZE];

    for (;;) {
        /* Block until at least one byte of data is ready to send */
        size_t n = xStreamBufferReceive(s_tx_stream, &report[1],
                                        HID_RPT_PAYLOAD, portMAX_DELAY);
        if (n == 0) continue;

        report[0] = (uint8_t)n;
        memset(&report[1 + n], 0, HID_RPT_PAYLOAD - n);

        /* Wait until USB is up and the IN endpoint can accept a new report */
        while (!tud_mounted() || !tud_hid_n_ready(0)) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        tud_hid_n_report(0, 0, report, HID_RPT_SIZE);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void hid_transport_init(void)
{
    s_tx_stream  = xStreamBufferCreate(TX_STREAM_SIZE, 1);
    hid_rx_stream = xStreamBufferCreate(RX_STREAM_SIZE, 1);
    s_tx_mu      = xSemaphoreCreateMutex();
    configASSERT(s_tx_stream && hid_rx_stream && s_tx_mu);

    xTaskCreate(hid_tx_task, "hid_tx", TX_TASK_STACK, NULL, TX_TASK_PRIO, NULL);
}

void hid_transport_write(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return;
    xSemaphoreTake(s_tx_mu, portMAX_DELAY);
    size_t sent = xStreamBufferSend(s_tx_stream, buf, len, 0);
    if (sent < len) {
        ESP_LOGW(TAG, "TX stream full, dropped %u byte(s)",
                 (unsigned)(len - sent));
    }
    xSemaphoreGive(s_tx_mu);
}

/* ── TinyUSB HID callbacks ───────────────────────────────────────────────── */

/* Required: return the HID report descriptor for interface @itf. */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf)
{
    (void)itf;
    return s_hid_report_desc;
}

/* Required: handle GET_REPORT control request.  Not used by this transport. */
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    (void)itf; (void)report_id; (void)report_type;
    (void)buffer; (void)reqlen;
    return 0;
}

/*
 * Required: called when the host sends a 64-byte OUT report.
 * report format: buffer[0]=data_len, buffer[1..data_len]=proto bytes
 */
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
    (void)itf; (void)report_id; (void)report_type;
    if (bufsize < 1) return;

    uint8_t len = buffer[0];
    if (len > HID_RPT_PAYLOAD || (uint16_t)len > (uint16_t)(bufsize - 1)) {
        len = (uint8_t)(bufsize - 1);
    }
    if (len == 0) return;

    /* tud_hid_set_report_cb runs in the TinyUSB FreeRTOS task — not an ISR */
    size_t sent = xStreamBufferSend(hid_rx_stream, &buffer[1], len, 0);
    if (sent < len) {
        ESP_LOGW(TAG, "RX stream full, dropped %u byte(s)",
                 (unsigned)(len - sent));
    }
}
