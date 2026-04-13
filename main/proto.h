/*
 * Binary serial protocol
 *
 * Frame layout:
 *   [0xAA][0x55][LEN_H][LEN_L][SEQ][CMD][PAYLOAD...][CRC_H][CRC_L]
 *
 *   LEN  = 2 + sizeof(PAYLOAD)   (counts SEQ + CMD + PAYLOAD bytes)
 *   CRC  = CRC16-CCITT (poly 0x1021, init 0xFFFF) over all bytes before CRC
 *
 * Response frame: same layout, CMD has bit-7 set (CMD | 0x80),
 *   PAYLOAD[0] is a proto_status_t byte followed by optional data.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* ── Magic ── */
#define PROTO_MAGIC0        0xAAU
#define PROTO_MAGIC1        0x55U
#define PROTO_RESP_FLAG     0x80U
#define PROTO_MAX_PAYLOAD   1024U

/* ── Command table ──────────────────────────────────────────────────────────
 * Range     Owner          Notes
 * 0x01–0x0F System         ping, device info, reset, …
 * 0x10–0x1F WiFi           sta config, connect, scan, …
 * 0x20–0x2F Network        IP / DNS settings (reserved)
 * 0x30–0x3F OTA            firmware update (reserved)
 * 0x40–0x7E User-defined   free for application use
 * 0x7F      NACK           internal, not sent by host
 * ───────────────────────────────────────────────────────────────────────── */
typedef enum {
    /* System */
    CMD_PING            = 0x01,  /* → "PONG" */
    CMD_GET_DEV_INFO    = 0x02,  /* → [fw_ver_str] */
    CMD_RESET           = 0x03,  /* restart device */

    /* WiFi STA */
    CMD_WIFI_SET_CONFIG = 0x10,  /* [ssid_len:1][ssid][pass_len:1][pass] */
    CMD_WIFI_GET_CONFIG = 0x11,  /* → [ssid_len:1][ssid][pass_len:1][**masked**] */
    CMD_WIFI_CONNECT    = 0x12,  /* connect with stored config */
    CMD_WIFI_DISCONNECT = 0x13,  /* disconnect */
    CMD_WIFI_GET_STATUS = 0x14,  /* → [state:1][ip:4] */
    CMD_WIFI_SCAN       = 0x15,  /* → [count:1]([ssid_len:1][ssid][rssi:1][auth:1])* */

    /* TCP tunneling (0x20–0x22) */
    CMD_TCP_CONNECT     = 0x20,  /* [host_len:1][host:N][port_hi:1][port_lo:1] → [conn_id:1] */
    CMD_TCP_SEND        = 0x21,  /* [conn_id:1][data...] (no response) */
    CMD_TCP_CLOSE       = 0x22,  /* [conn_id:1] → OK */
    CMD_TCP_CONNECT_ACK  = 0x23,
    CMD_TCP_CONNECT_DONE = 0x26,  /* push: [conn_id:1][status:1]  0=ok, errno lo-byte=fail */
    CMD_TCP_SEND_CREDIT  = 0x27,  /* push: [conn_id:1][credits_hi:1][credits_lo:1] */

    /* Push frames (ESP32 → host, unsolicited, no RESP_FLAG) */
    CMD_TCP_DATA_PUSH   = 0x40,  /* [conn_id:1][data...] */
    CMD_TCP_CLOSED_PUSH = 0x41,  /* [conn_id:1] */
} proto_cmd_t;

/* ── Status codes (first byte of every response payload) ── */
typedef enum {
    PROTO_STATUS_OK      = 0x00,
    PROTO_STATUS_ERROR   = 0x01,
    PROTO_STATUS_BUSY    = 0x02,
    PROTO_STATUS_INVALID = 0x03,  /* bad arguments */
    PROTO_STATUS_TIMEOUT = 0x04,
} proto_status_t;

/* ── Parsed frame ── */
typedef struct {
    uint8_t  seq;
    uint8_t  cmd;
    uint8_t  payload[PROTO_MAX_PAYLOAD];
    uint16_t payload_len;
} proto_frame_t;

/* ── Parser ── */
typedef struct proto_parser proto_parser_t;

/** Called with a fully validated frame */
typedef void (*proto_frame_cb_t)(const proto_frame_t *frame, void *ctx);

proto_parser_t *proto_parser_create(proto_frame_cb_t cb, void *ctx);
void            proto_parser_destroy(proto_parser_t *p);

/** Feed raw bytes from the serial port into the parser */
void            proto_parser_feed(proto_parser_t *p, const uint8_t *data, size_t len);

/**
 * Build a response frame into buf.
 *
 * @param seq       Echo the request SEQ byte
 * @param cmd       Request CMD (0x80 will be OR'd in automatically)
 * @param status    proto_status_t
 * @param data      Optional data after status byte (may be NULL)
 * @param data_len  Length of data
 * @param buf       Output buffer
 * @param buf_size  Size of output buffer
 * @return Total bytes written, or 0 on buffer overflow
 */
size_t proto_build_response(uint8_t seq, uint8_t cmd,
                             proto_status_t status,
                             const uint8_t *data, size_t data_len,
                             uint8_t *buf, size_t buf_size);

/**
 * Build an unsolicited push frame (ESP32 → host) into buf.
 * Uses SEQ=0, no RESP_FLAG.
 *
 * @param cmd       Push command (e.g. CMD_TCP_DATA_PUSH)
 * @param data      Payload bytes (may be NULL)
 * @param data_len  Length of data
 * @param buf       Output buffer
 * @param buf_size  Size of output buffer
 * @return Total bytes written, or 0 on buffer overflow
 */
size_t proto_build_push(uint8_t cmd,
                         const uint8_t *data, size_t data_len,
                         uint8_t *buf, size_t buf_size);
