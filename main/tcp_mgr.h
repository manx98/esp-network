/*
 * tcp_mgr — Single long-lived TCP relay to a proxy server.
 *
 * ESP32 maintains exactly one TCP connection to the configured proxy
 * server.  The ctrl side is responsible for all proxy-protocol framing
 * and connection multiplexing; ESP32's role is purely to relay bytes:
 *
 *   ctrl → CDC → ESP32 → TCP → proxy server   (via tcp_mgr_send)
 *   proxy server → TCP → ESP32 → CDC → ctrl   (CMD_PROXY_DATA_PUSH)
 *
 * When the TCP connection is lost for any reason (remote close, network
 * error, keepalive failure) a CMD_PROXY_CLOSED_PUSH frame is delivered
 * to ctrl via the push callback.
 *
 * TCP keep-alive is enabled on the socket so dead connections are
 * detected without application-level pinging (~30 s idle + 3×5 s probes).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "proto.h"

/** Callback invoked from a background task to push data/events to ctrl. */
typedef void (*tcp_push_cb_t)(uint8_t cmd, const uint8_t *data, size_t len);

/**
 * Initialise the relay and start background rx/send tasks.
 * Must be called once after WiFi is initialised.
 */
void tcp_mgr_init(tcp_push_cb_t push_cb);

/**
 * Synchronously connect to the proxy server.
 *
 * Blocks the calling task for up to ~10 s (DNS + TCP connect timeout).
 * Returns immediately with -2 if a connection is already open.
 *
 * @return  0   connected successfully
 *          -1  connection failed (DNS, timeout, refused, …)
 *          -2  already connected (busy)
 */
int tcp_mgr_connect(const char *host, uint16_t port);

/**
 * Enqueue raw bytes for transmission to the proxy server.
 * Returns immediately; the actual send happens in the background send task.
 *
 * @return  0 on success, -1 on error (queue full, OOM, not connected)
 */
int tcp_mgr_send(const uint8_t *data, size_t len);

/**
 * Close the relay connection (if open) and notify ctrl via
 * CMD_PROXY_CLOSED_PUSH.
 */
void tcp_mgr_disconnect(void);

/** Returns true if the relay connection is currently up. */
bool tcp_mgr_is_connected(void);

/** Cumulative byte counters.  Thread-safe (atomic reads). */
void tcp_mgr_get_bytes(uint64_t *rx_bytes, uint64_t *tx_bytes);
