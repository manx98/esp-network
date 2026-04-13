#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "tcp_mgr.h"

#include <stdatomic.h>

static const char *TAG = "tcp_mgr";

/* ── Configuration ─────────────────────────────────────────────────────────── */

/* Serial link is the hard bottleneck at ~100 KB/s.
 * Set rx buf to max protocol payload minus 1 byte for the conn_id prefix,
 * so each push frame carries as much data as possible. */
#define TCP_CONNECT_TIMEOUT_S     10
#define TCP_RX_BUF_SIZE           (PROTO_MAX_PAYLOAD - 1)   /* 1023 bytes */
#define TCP_RX_TASK_STACK         8192
#define TCP_RX_TASK_PRIORITY      5
#define TCP_CONNECT_TASK_STACK    4096
#define TCP_CONNECT_TASK_PRIORITY 4
#define TCP_SEND_TASK_STACK       4096
#define TCP_SEND_TASK_PRIORITY    5

/* Poll delays — tuned for CONFIG_FREERTOS_HZ=1000 (1 tick = 1 ms).
 * any_data=true  → 1 tick  (1 ms): keep draining quickly
 * any_active     → 2 ticks (2 ms): wait for next recv window
 * idle           → 20 ms:          save CPU when no connections open */
#define TCP_POLL_IDLE_MS          20

/* Send-queue depth: allow up to 4 pending jobs per connection */
#define TCP_SEND_QUEUE_DEPTH      (TCP_MAX_CONNS * 4)

/* Idle timeout: free stale connections (e.g. ctrl crashed) */
#define TCP_IDLE_TIMEOUT_MS       30000U   /* 30 s */
#define TCP_IDLE_CHECK_MS         10000U   /* check every 10 s */

/* Per-socket send buffer (bytes).  Larger = better burst; keep per-connection
 * memory reasonable: 16 conns × 5760 B ≈ 92 KB, well within 2.1 MB budget. */
#define TCP_SNDBUF                5760

/* ── State ──────────────────────────────────────────────────────────────────── */

typedef struct {
    int      fd;
    bool     active;
    bool     ready;
    uint32_t last_active_ms;
} tcp_conn_t;

/* Request item placed in s_connect_queue by tcp_mgr_connect_async */
typedef struct {
    uint8_t  conn_id;
    char     host[256];
    uint16_t port;
} connect_req_t;

/* Heap-allocated send job; pointer stored in s_send_queue */
typedef struct {
    uint8_t conn_id;
    size_t  len;
    uint8_t data[];   /* flexible array: malloc'd to exact payload size */
} send_job_t;

static tcp_conn_t        s_conns[TCP_MAX_CONNS];
static SemaphoreHandle_t s_mu;              /* protects s_conns table   */
static tcp_push_cb_t     s_push_cb;
static QueueHandle_t     s_connect_queue;   /* connect_req_t items      */
static QueueHandle_t     s_send_queue;      /* send_job_t *  pointers   */
static uint64_t          s_rx_bytes = 0;
static uint64_t          s_tx_bytes = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────────── */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Allocate next free slot with fd=-1 (caller must hold s_mu) */
static int alloc_slot_locked(void)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!s_conns[i].active) {
            s_conns[i].fd             = -1;
            s_conns[i].active         = true;
            s_conns[i].ready          = false;
            s_conns[i].last_active_ms = now_ms();
            return i;
        }
    }
    return -1;
}

static void close_conn_locked(int id)
{
    if (s_conns[id].active) {
        if (s_conns[id].fd >= 0) {
            close(s_conns[id].fd);
        }
        s_conns[id].fd     = -1;
        s_conns[id].active = false;
        s_conns[id].ready  = false;
    }
}

/* ── Core connect logic (shared by tcp_connect_task workers) ────────────────── */

static int do_tcp_connect(const char *host, uint16_t port)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int gai_err = getaddrinfo(host, port_str, &hints, &res);
    if (gai_err != 0 || res == NULL) {
        ESP_LOGW(TAG, "getaddrinfo(%s:%u) failed: %d", host, port, gai_err);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    /* Limit send buffer to control per-connection lwIP memory.
     * 5760 bytes = 4 × MSS ≈ good burst buffer for the serial link. */
    int sndbuf = TCP_SNDBUF;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    /* Non-blocking connect + select() timeout */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect(%s:%u) immediate fail: %s", host, port, strerror(errno));
        close(sock);
        return -1;
    }

    if (ret != 0) {
        fd_set wfds, efds;
        FD_ZERO(&wfds); FD_ZERO(&efds);
        FD_SET(sock, &wfds); FD_SET(sock, &efds);
        struct timeval tv = {TCP_CONNECT_TIMEOUT_S, 0};
        int n = select(sock + 1, NULL, &wfds, &efds, &tv);
        if (n <= 0) {
            ESP_LOGW(TAG, "connect(%s:%u) timeout: %d", host, port, n);
            close(sock);
            return -1;
        }
        int sock_err = 0;
        socklen_t errlen = sizeof(sock_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &sock_err, &errlen);
        if (sock_err != 0) {
            ESP_LOGW(TAG, "connect(%s:%u) failed: %s", host, port, strerror(sock_err));
            close(sock);
            return -1;
        }
    }

    /* Restore blocking mode; tcp_rx_task uses MSG_DONTWAIT for recv */
    fcntl(sock, F_SETFL, flags);
    return sock;
}

/* ── Async connect workers (TCP_CONNECT_WORKERS instances) ──────────────────── */

static void tcp_connect_task(void *arg)
{
    connect_req_t req;
    while (1) {
        xQueueReceive(s_connect_queue, &req, portMAX_DELAY);

        int sock = do_tcp_connect(req.host, req.port);

        uint8_t result[2];
        result[0] = req.conn_id;
        if (sock >= 0) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            s_conns[req.conn_id].fd             = sock;
            s_conns[req.conn_id].last_active_ms = now_ms();
            xSemaphoreGive(s_mu);
            result[1] = 0;
            ESP_LOGI(TAG, "async connected: id=%d fd=%d %s:%u",
                     req.conn_id, sock, req.host, req.port);
        } else {
            result[1] = (uint8_t)(errno ? (uint8_t)errno : 1);
            xSemaphoreTake(s_mu, portMAX_DELAY);
            close_conn_locked(req.conn_id);
            xSemaphoreGive(s_mu);
            ESP_LOGW(TAG, "async connect failed: id=%d %s:%u",
                     req.conn_id, req.host, req.port);
        }
        s_push_cb(CMD_TCP_CONNECT_DONE, result, 2);
    }
}

/* ── Async send task (T8 + T10) ─────────────────────────────────────────────── */

static void tcp_send_task(void *arg)
{
    while (1) {
        send_job_t *job = NULL;
        xQueueReceive(s_send_queue, &job, portMAX_DELAY);
        if (!job) continue;

        xSemaphoreTake(s_mu, portMAX_DELAY);
        bool active = s_conns[job->conn_id].active;
        int  fd     = s_conns[job->conn_id].fd;
        xSemaphoreGive(s_mu);

        int rc = 0;
        if (!active || fd < 0) {
            rc = -1;
        } else {
            size_t sent = 0;
            while (sent < job->len) {
                ssize_t n = send(fd, job->data + sent, job->len - sent, 0);
                if (n <= 0) {
                    ESP_LOGW(TAG, "conn %d send error: %s",
                             job->conn_id, strerror(errno));
                    rc = -1;
                    break;
                }
                ESP_LOGD(TAG, "conn %d sent %d bytes", job->conn_id, (int)n);
                sent += (size_t)n;
                s_rx_bytes += (uint64_t)n;
            }
            if (rc == 0) {
                xSemaphoreTake(s_mu, portMAX_DELAY);
                if (s_conns[job->conn_id].active)
                    s_conns[job->conn_id].last_active_ms = now_ms();
                xSemaphoreGive(s_mu);

                /* Return send credit to ctrl (bytes successfully written to TCP) */
                uint8_t credit[3] = {
                    job->conn_id,
                    (uint8_t)(job->len >> 8),
                    (uint8_t)(job->len),
                };
                s_push_cb(CMD_TCP_SEND_CREDIT, credit, 3);
            }
        }

        if (rc != 0) {
            tcp_mgr_close(job->conn_id);
            uint8_t payload = job->conn_id;
            s_push_cb(CMD_TCP_CLOSED_PUSH, &payload, 1);
        }

        free(job);
    }
}

/* ── Background rx task ──────────────────────────────────────────────────────── */

static void tcp_rx_task(void *arg)
{
    uint8_t *recv_buf = malloc(TCP_RX_BUF_SIZE);
    uint8_t *push_buf = malloc(1 + TCP_RX_BUF_SIZE);
    if (!recv_buf || !push_buf) {
        ESP_LOGE(TAG, "OOM in tcp_rx_task");
        free(recv_buf);
        free(push_buf);
        vTaskDelete(NULL);
        return;
    }

    /* Snapshot buffers: read all connection states under one lock per cycle. */
    int     snap_fd[TCP_MAX_CONNS];
    uint32_t last_idle_check_ms = now_ms();

    while (1) {
        bool any_active = false;
        bool any_data   = false;

        /* ── Single-lock snapshot of all connection fds ── */
        xSemaphoreTake(s_mu, portMAX_DELAY);
        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            snap_fd[i] = (s_conns[i].active && s_conns[i].ready && s_conns[i].fd >= 0)
                         ? s_conns[i].fd : -1;
        }
        xSemaphoreGive(s_mu);

        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            int fd = snap_fd[i];
            if (fd < 0) continue;
            any_active = true;

            /* Drain this fd to EAGAIN before moving to the next connection */
            ssize_t n = recv(fd, recv_buf, TCP_RX_BUF_SIZE, MSG_DONTWAIT);
            while (n > 0) {
                any_data = true;
                s_tx_bytes += (uint64_t)n;
                push_buf[0] = (uint8_t)i;
                memcpy(push_buf + 1, recv_buf, n);
                ESP_LOGD(TAG, "conn %d rx %d bytes", i, (int)n);
                s_push_cb(CMD_TCP_DATA_PUSH, push_buf, 1 + (size_t)n);
                n = recv(fd, recv_buf, TCP_RX_BUF_SIZE, MSG_DONTWAIT);
            }

            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                ESP_LOGI(TAG, "conn %d remote closed (n=%d errno=%s)",
                         i, (int)n, strerror(errno));
                bool closed = false;
                xSemaphoreTake(s_mu, portMAX_DELAY);
                /* Guard against fd being recycled between snapshot and now */
                if (s_conns[i].active && s_conns[i].fd == fd) {
                    close_conn_locked(i);
                    closed = true;
                    /* Update last_active_ms while under lock */
                } else if (s_conns[i].active) {
                    s_conns[i].last_active_ms = now_ms();
                }
                xSemaphoreGive(s_mu);
                if (closed) {
                    uint8_t closed_payload[1] = {(uint8_t)i};
                    s_push_cb(CMD_TCP_CLOSED_PUSH, closed_payload, 1);
                }
            } else if (any_data) {
                /* Batch-update last_active_ms for connections that had data */
                xSemaphoreTake(s_mu, portMAX_DELAY);
                if (s_conns[i].active && s_conns[i].fd == fd)
                    s_conns[i].last_active_ms = now_ms();
                xSemaphoreGive(s_mu);
            }
        }

        /* ── Idle timeout check (every TCP_IDLE_CHECK_MS) ── */
        uint32_t t = now_ms();
        if (t - last_idle_check_ms >= TCP_IDLE_CHECK_MS) {
            last_idle_check_ms = t;
            uint8_t timed_out[TCP_MAX_CONNS];
            int     n_timed_out = 0;

            xSemaphoreTake(s_mu, portMAX_DELAY);
            for (int i = 0; i < TCP_MAX_CONNS; i++) {
                if (s_conns[i].active &&
                    (t - s_conns[i].last_active_ms) > TCP_IDLE_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "conn %d idle timeout, closing", i);
                    timed_out[n_timed_out++] = (uint8_t)i;
                    close_conn_locked(i);
                }
            }
            xSemaphoreGive(s_mu);

            for (int k = 0; k < n_timed_out; k++)
                s_push_cb(CMD_TCP_CLOSED_PUSH, &timed_out[k], 1);
        }

        /* ── Yield ──
         * With CONFIG_FREERTOS_HZ=1000, vTaskDelay(1) = 1 ms.
         * When data is flowing stay aggressive (1 ms); when connections are
         * open but quiet wait 2 ms; otherwise back off to save CPU. */
        vTaskDelay(any_data ? 1 : pdMS_TO_TICKS(any_active ? 2 : TCP_POLL_IDLE_MS));
    }

    free(recv_buf);
    free(push_buf);
    vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────────────── */

void tcp_mgr_init(tcp_push_cb_t push_cb)
{
    s_push_cb = push_cb;
    s_mu      = xSemaphoreCreateMutex();
    configASSERT(s_mu);

    /* Connect queue: depth = max connections so all can be queued at once.
     * TCP_CONNECT_WORKERS tasks drain it in parallel. */
    s_connect_queue = xQueueCreate(TCP_MAX_CONNS, sizeof(connect_req_t));
    configASSERT(s_connect_queue);

    /* Send queue: pointer items (jobs heap-allocated per send). */
    s_send_queue = xQueueCreate(TCP_SEND_QUEUE_DEPTH, sizeof(send_job_t *));
    configASSERT(s_send_queue);

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        s_conns[i].fd             = -1;
        s_conns[i].active         = false;
        s_conns[i].ready          = false;
        s_conns[i].last_active_ms = 0;
    }

    xTaskCreate(tcp_rx_task,   "tcp_rx",   TCP_RX_TASK_STACK,   NULL,
                TCP_RX_TASK_PRIORITY,   NULL);
    xTaskCreate(tcp_send_task, "tcp_send", TCP_SEND_TASK_STACK, NULL,
                TCP_SEND_TASK_PRIORITY, NULL);

    /* Spawn N parallel connect workers so multiple DNS+connect operations
     * proceed simultaneously without blocking each other. */
    for (int w = 0; w < TCP_CONNECT_WORKERS; w++) {
        xTaskCreate(tcp_connect_task, "tcp_conn", TCP_CONNECT_TASK_STACK, NULL,
                    TCP_CONNECT_TASK_PRIORITY, NULL);
    }

    ESP_LOGI(TAG, "initialised (max_conns=%d connect_workers=%d)",
             TCP_MAX_CONNS, TCP_CONNECT_WORKERS);
}

/* Allocate a slot and enqueue the connect request.
 * Returns conn_id ≥ 0 on success, -1 on error, -2 if all slots busy. */
int tcp_mgr_connect_async(const char *host, uint16_t port)
{
    if (!host || strlen(host) > 255) return -1;

    xSemaphoreTake(s_mu, portMAX_DELAY);
    int conn_id = alloc_slot_locked();
    xSemaphoreGive(s_mu);

    if (conn_id < 0) {
        ESP_LOGW(TAG, "no free connection slots");
        return -2;
    }

    connect_req_t req;
    req.conn_id = (uint8_t)conn_id;
    strlcpy(req.host, host, sizeof(req.host));
    req.port = port;

    if (xQueueSend(s_connect_queue, &req, 0) != pdTRUE) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        close_conn_locked(conn_id);
        xSemaphoreGive(s_mu);
        ESP_LOGW(TAG, "connect queue full");
        return -1;
    }

    ESP_LOGI(TAG, "connect queued: id=%d %s:%u", conn_id, host, port);
    return conn_id;
}

int tcp_mgr_ready(uint8_t conn_id)
{
    if (conn_id >= TCP_MAX_CONNS) return -1;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_conns[conn_id].active && s_conns[conn_id].fd >= 0) {
        s_conns[conn_id].ready          = true;
        s_conns[conn_id].last_active_ms = now_ms();
    }
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "conn %d rx ready (fd=%d)", conn_id, s_conns[conn_id].fd);
    return 0;
}

/* Enqueue a send job (non-blocking from cmd_task perspective).
 * Returns 0 on success, -1 on error. */
int tcp_mgr_send(uint8_t conn_id, const uint8_t *data, size_t len)
{
    if (conn_id >= TCP_MAX_CONNS || len == 0) return -1;
    if (len > PROTO_MAX_PAYLOAD - 1) {
        ESP_LOGW(TAG, "conn %d send too large: %u", conn_id, (unsigned)len);
        return -1;
    }

    send_job_t *job = malloc(sizeof(send_job_t) + len);
    if (!job) {
        ESP_LOGW(TAG, "conn %d send OOM", conn_id);
        return -1;
    }
    job->conn_id = conn_id;
    job->len     = len;
    memcpy(job->data, data, len);

    if (xQueueSend(s_send_queue, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "send queue full, conn %d", conn_id);
        free(job);
        return -1;
    }
    return 0;
}

void tcp_mgr_close(uint8_t conn_id)
{
    if (conn_id >= TCP_MAX_CONNS) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    close_conn_locked(conn_id);
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "conn %d closed by host", conn_id);
}

void tcp_mgr_get_bytes(uint64_t *rx_bytes, uint64_t *tx_bytes)
{
    *rx_bytes = atomic_load(&s_rx_bytes);
    *tx_bytes = atomic_load(&s_tx_bytes);
}
