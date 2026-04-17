/*
 * HID transport layer — presents a byte-stream interface over USB HID.
 *
 * Each USB HID report is 64 bytes:
 *   report[0]    : number of valid data bytes that follow (0–63)
 *   report[1..63]: data, zero-padded
 *
 * The existing binary framing (proto.h) is unmodified; only the physical
 * transport changes from USB CDC to USB HID interrupt endpoints.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

/* ── Sizes ── */
#define HID_RPT_SIZE    64   /* bytes per USB report */
#define HID_RPT_PAYLOAD 63   /* usable bytes per report (byte 0 = length) */

/* ── RX stream buffer (host → device) ───────────────────────────────────────
 * cmd_task reads proto frames from this buffer, same as it did from the CDC
 * stream buffer.  Populated inside tud_hid_set_report_cb().
 */
extern StreamBufferHandle_t hid_rx_stream;

/* ── Init ────────────────────────────────────────────────────────────────── */

/** Initialise the HID transport: create stream buffers, start TX task. */
void hid_transport_init(void);

/* ── TX ──────────────────────────────────────────────────────────────────── */

/**
 * Write @len bytes to the host.
 * Thread-safe.  Non-blocking: excess bytes are dropped with a warning if the
 * TX stream buffer is full.
 */
void hid_transport_write(const uint8_t *buf, size_t len);

/* ── USB descriptors (passed to tinyusb_driver_install) ─────────────────── */

/** Pointer to the full-speed USB configuration descriptor (HID interface). */
const uint8_t *hid_config_desc(void);

/** Pointer to the HID report descriptor. */
const uint8_t *hid_report_desc(void);

/** Size of the HID report descriptor in bytes. */
size_t hid_report_desc_size(void);
