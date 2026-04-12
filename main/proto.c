#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "proto.h"

static const char *TAG = "proto";

/* ── CRC16-CCITT (poly 0x1021, init 0xFFFF) ── */
static uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

#define CRC_INIT 0xFFFFu

/* ── Parser state machine ── */
typedef enum {
    ST_MAGIC0,
    ST_MAGIC1,
    ST_LEN_H,
    ST_LEN_L,
    ST_BODY,
    ST_CRC_H,
    ST_CRC_L,
} parse_state_t;

struct proto_parser {
    parse_state_t    state;
    proto_frame_cb_t cb;
    void            *ctx;

    uint16_t         body_len;               /* expected body bytes (SEQ+CMD+PAYLOAD) */
    uint16_t         body_recv;              /* bytes received so far */
    uint8_t          body[2 + PROTO_MAX_PAYLOAD];
    uint8_t          crc_hi;                 /* first CRC byte */
    uint16_t         crc_calc;              /* running CRC */
};

proto_parser_t *proto_parser_create(proto_frame_cb_t cb, void *ctx)
{
    proto_parser_t *p = calloc(1, sizeof(*p));
    if (p) {
        p->cb       = cb;
        p->ctx      = ctx;
        p->state    = ST_MAGIC0;
        p->crc_calc = CRC_INIT;
    }
    return p;
}

void proto_parser_destroy(proto_parser_t *p)
{
    free(p);
}

static void parser_reset(proto_parser_t *p)
{
    p->state    = ST_MAGIC0;
    p->crc_calc = CRC_INIT;
}

void proto_parser_feed(proto_parser_t *p, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        switch (p->state) {
        case ST_MAGIC0:
            if (b == PROTO_MAGIC0) {
                p->crc_calc = crc16_update(CRC_INIT, b);
                p->state = ST_MAGIC1;
            }
            break;

        case ST_MAGIC1:
            p->crc_calc = crc16_update(p->crc_calc, b);
            if (b == PROTO_MAGIC1) {
                p->state = ST_LEN_H;
            } else {
                parser_reset(p);
                /* re-evaluate this byte as potential MAGIC0 */
                if (b == PROTO_MAGIC0) {
                    p->crc_calc = crc16_update(CRC_INIT, b);
                    p->state = ST_MAGIC1;
                }
            }
            break;

        case ST_LEN_H:
            p->crc_calc = crc16_update(p->crc_calc, b);
            p->body_len = (uint16_t)b << 8;
            p->state = ST_LEN_L;
            break;

        case ST_LEN_L:
            p->crc_calc = crc16_update(p->crc_calc, b);
            p->body_len |= b;
            if (p->body_len < 2 || p->body_len > 2 + PROTO_MAX_PAYLOAD) {
                ESP_LOGW(TAG, "Invalid LEN=%u, discarding", p->body_len);
                parser_reset(p);
            } else {
                p->body_recv = 0;
                p->state = ST_BODY;
            }
            break;

        case ST_BODY:
            p->crc_calc = crc16_update(p->crc_calc, b);
            p->body[p->body_recv++] = b;
            if (p->body_recv >= p->body_len) {
                p->state = ST_CRC_H;
            }
            break;

        case ST_CRC_H:
            p->crc_hi = b;
            p->state = ST_CRC_L;
            break;

        case ST_CRC_L: {
            uint16_t crc_recv = ((uint16_t)p->crc_hi << 8) | b;
            if (crc_recv == p->crc_calc) {
                proto_frame_t frame;
                frame.seq         = p->body[0];
                frame.cmd         = p->body[1];
                frame.payload_len = p->body_len - 2;
                memcpy(frame.payload, &p->body[2], frame.payload_len);
                p->cb(&frame, p->ctx);
            } else {
                ESP_LOGW(TAG, "CRC mismatch: calc=0x%04X recv=0x%04X",
                         p->crc_calc, crc_recv);
            }
            parser_reset(p);
            break;
        }
        } /* switch */
    }
}

size_t proto_build_response(uint8_t seq, uint8_t cmd,
                             proto_status_t status,
                             const uint8_t *data, size_t data_len,
                             uint8_t *buf, size_t buf_size)
{
    /* body = [SEQ:1][CMD|0x80:1][STATUS:1][DATA:N] */
    uint16_t body_len  = (uint16_t)(3 + data_len);
    size_t   frame_len = 2u + 2u + body_len + 2u;

    if (buf_size < frame_len) {
        ESP_LOGE(TAG, "Response buffer too small: need %u have %u",
                 (unsigned)frame_len, (unsigned)buf_size);
        return 0;
    }

    size_t n = 0;
    buf[n++] = PROTO_MAGIC0;
    buf[n++] = PROTO_MAGIC1;
    buf[n++] = (uint8_t)(body_len >> 8);
    buf[n++] = (uint8_t)(body_len & 0xFF);
    buf[n++] = seq;
    buf[n++] = cmd | PROTO_RESP_FLAG;
    buf[n++] = (uint8_t)status;

    if (data && data_len > 0) {
        memcpy(&buf[n], data, data_len);
        n += data_len;
    }

    uint16_t crc = CRC_INIT;
    for (size_t k = 0; k < n; k++) {
        crc = crc16_update(crc, buf[k]);
    }
    buf[n++] = (uint8_t)(crc >> 8);
    buf[n++] = (uint8_t)(crc & 0xFF);

    return n;
}
