/*
 * tcp_mgr — TCP connection manager for SOCKS5 tunneling over serial
 *
 * Manages up to TCP_MAX_CONNS simultaneous TCP connections using the
 * ESP32 WiFi stack.  A background task reads from all open sockets
 * and delivers data/close events via the push callback.
 *
 * Push callback format:
 *   CMD_TCP_DATA_PUSH   (0x40): payload = [conn_id:1][data...]
 *   CMD_TCP_CLOSED_PUSH (0x41): payload = [conn_id:1]
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "proto.h"

/* Maximum simultaneous TCP connections.
 * Set to match CONFIG_LWIP_MAX_SOCKETS(24) minus ~8 reserved for the WiFi
 * stack (DHCP, DNS, management sockets). */
#define TCP_MAX_CONNS       16

/* Number of parallel tcp_connect_task workers.  On a single-core CPU each
 * worker blocks independently in DNS + connect, so N workers allow N
 * connection attempts to proceed simultaneously. */
#define TCP_CONNECT_WORKERS  4

/** Called from the tcp_rx_task to deliver data/events to the host */
typedef void (*tcp_push_cb_t)(uint8_t cmd, const uint8_t *data, size_t len);

/**
 * Initialise the TCP manager and start the background rx task.
 * Must be called after WiFi is initialised.
 */
void tcp_mgr_init(tcp_push_cb_t push_cb);

/**
 * Asynchronously open a TCP connection to host:port.
 * Allocates a connection slot immediately and enqueues the connect
 * request.  The result is delivered as a CMD_TCP_CONNECT_DONE push
 * frame once the connection attempt completes.
 *
 * @param host      Null-terminated hostname or IPv4 dotted-decimal string
 * @param port      Target TCP port (host byte order)
 * @return          Connection id 0–(TCP_MAX_CONNS-1) on success,
 *                  -1 on general error, -2 if all slots are busy
 */
int tcp_mgr_connect_async(const char *host, uint16_t port);

int tcp_mgr_ready(uint8_t conn_id);

/**
 * Send data on an open connection.
 *
 * @return  0 on success, -1 on error (connection probably dead)
 */
int tcp_mgr_send(uint8_t conn_id, const uint8_t *data, size_t len);

/**
 * Close a connection and free the slot.
 */
void tcp_mgr_close(uint8_t conn_id);

void tcp_mgr_get_bytes(uint64_t *rx_bytes, uint64_t *tx_bytes);