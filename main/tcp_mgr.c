#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "tcp_mgr.h"

#include <stdatomic.h>

static const char *TAG = "tcp_mgr";

/* ── Configuration ─────────────────────────────────────────────────────────── */

#define TCP_CONNECT_TIMEOUT_S  10
#define TCP_RX_BUF_SIZE        (PROTO_MAX_PAYLOAD - 4)  /* leave room for frame header */
#define TCP_RX_TASK_STACK      8192
#define TCP_RX_TASK_PRIORITY   5
#define TCP_POLL_IDLE_MS       20   /* delay when no connections are active */
#define TCP_POLL_ACTIVE_MS      5   /* delay when connections exist but no data */

/* ── State ──────────────────────────────────────────────────────────────────── */

typedef struct {
    int  fd;
    bool active;
    bool ready;
} tcp_conn_t;

static tcp_conn_t        s_conns[TCP_MAX_CONNS];
static SemaphoreHandle_t s_mu;        /* protects s_conns table */
static tcp_push_cb_t     s_push_cb;
static uint64_t s_rx_bytes = 0;
static uint64_t s_tx_bytes = 0;


/* ── Helpers ─────────────────────────────────────────────────────────────────── */

static int alloc_conn_locked(int fd)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!s_conns[i].active) {
            s_conns[i].fd     = fd;
            s_conns[i].active = true;
            s_conns[i].ready  = false;
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

/* ── Background rx task ─────────────────────────────────────────────────────── */

static void tcp_rx_task(void *arg)
{
    uint8_t *recv_buf = malloc(TCP_RX_BUF_SIZE);
    uint8_t *push_buf = malloc(2 + TCP_RX_BUF_SIZE);
    if (!recv_buf || !push_buf) {
        ESP_LOGE(TAG, "OOM in tcp_rx_task");
        free(recv_buf);
        free(push_buf);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        bool any_active = false;
        bool any_data   = false;

        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            bool active = s_conns[i].ready && s_conns[i].active;
            int  fd     = s_conns[i].fd;
            xSemaphoreGive(s_mu);

            if (!active || fd < 0) {
                continue;
            }
            any_active = true;

            ssize_t n = recv(fd, recv_buf, TCP_RX_BUF_SIZE, MSG_DONTWAIT);
            if (n > 0) {
                any_data = true;
                s_tx_bytes += (uint64_t)n;
                push_buf[0] = (uint8_t)i;
                memcpy(push_buf + 1, recv_buf, n);
                push_buf[n + 2] = '\0';
                ESP_LOGI(TAG, "conn %d rx %d bytes: %s", i, (int)n, (char*)(push_buf+1));
                s_push_cb(CMD_TCP_DATA_PUSH, push_buf, 1 + (size_t)n);
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                /* n == 0 → graceful close; real error → connection broken */
                ESP_LOGI(TAG, "conn %d remote closed (n=%d errno=%s)", i, (int)n, strerror(errno));

                xSemaphoreTake(s_mu, portMAX_DELAY);
                /* Re-check: might have been closed by tcp_mgr_close() already */
                if (s_conns[i].active && s_conns[i].fd == fd) {
                    close_conn_locked(i);
                }
                xSemaphoreGive(s_mu);

                uint8_t closed_payload[1] = {(uint8_t)i};
                s_push_cb(CMD_TCP_CLOSED_PUSH, closed_payload, 1);
            }
            /* EAGAIN / EWOULDBLOCK → no data yet, continue to next slot */
            /* Always yield at least 1 tick so the IDLE task can feed the watchdog.
            * When data is flowing use the minimum (1 tick ≈ 1 ms); when idle sleep
            * longer to avoid burning CPU for nothing. */
            vTaskDelay(any_data ? 1 : pdMS_TO_TICKS(any_active ? TCP_POLL_ACTIVE_MS : TCP_POLL_IDLE_MS));
        }

        /* Always yield at least 1 tick so the IDLE task can feed the watchdog.
         * When data is flowing use the minimum (1 tick ≈ 1 ms); when idle sleep
         * longer to avoid burning CPU for nothing. */
        vTaskDelay(any_data ? 1 : pdMS_TO_TICKS(any_active ? TCP_POLL_ACTIVE_MS
                                                            : TCP_POLL_IDLE_MS));
    }

    /* Unreachable, but tidy up */
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

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        s_conns[i].fd     = -1;
        s_conns[i].active = false;
        s_conns[i].ready  = false;
    }

    xTaskCreate(tcp_rx_task, "tcp_rx", TCP_RX_TASK_STACK, NULL,
                TCP_RX_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "initialised (max_conns=%d)", TCP_MAX_CONNS);
}

int tcp_mgr_connect(const char *host, uint16_t port)
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
        ESP_LOGW(TAG, "socket() failed: %d", errno);
        freeaddrinfo(res);
        return -1;
    }

    /* Non-blocking connect with explicit select() timeout.
     * SO_SNDTIMEO is not reliable for connect() in all lwIP versions. */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect(%s:%u) immediate fail: %d", host, port, errno);
        close(sock);
        return -1;
    }

    if (ret != 0) {
        /* Wait for the connection to complete */
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(sock, &wfds);
        FD_SET(sock, &efds);
        struct timeval tv = {TCP_CONNECT_TIMEOUT_S, 0};
        int n = select(sock + 1, NULL, &wfds, &efds, &tv);
        if (n <= 0) {
            ESP_LOGW(TAG, "connect(%s:%u) timeout/select error: %d", host, port, n);
            close(sock);
            return -1;
        }
        /* Check actual socket error */
        int sock_err = 0;
        socklen_t errlen = sizeof(sock_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &sock_err, &errlen);
        if (sock_err != 0) {
            ESP_LOGW(TAG, "connect(%s:%u) failed: %d", host, port, sock_err);
            close(sock);
            return -1;
        }
    }

    /* Restore blocking mode; tcp_rx_task uses MSG_DONTWAIT for recv */
    fcntl(sock, F_SETFL, flags);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    int conn_id = alloc_conn_locked(sock);
    xSemaphoreGive(s_mu);

    if (conn_id < 0) {
        ESP_LOGW(TAG, "no free connection slots");
        close(sock);
        return -2;
    }

    ESP_LOGI(TAG, "connected: id=%d fd=%d %s:%u", conn_id, sock, host, port);
    return conn_id;
}

int tcp_mgr_ready(uint8_t conn_id)
{
    if (conn_id >= TCP_MAX_CONNS) {
        return -1;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_conns[conn_id].active) {
        s_conns[conn_id].ready = true;
    }
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "conn %d rx ready (fd=%d)", conn_id, s_conns[conn_id].fd);
    return 0;
}

int tcp_mgr_send(uint8_t conn_id, const uint8_t *data, size_t len)
{
    if (conn_id >= TCP_MAX_CONNS) {
        return -1;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool active = s_conns[conn_id].active;
    int  fd     = s_conns[conn_id].fd;
    xSemaphoreGive(s_mu);

    if (!active || fd < 0) {
        ESP_LOGW(TAG, "conn %d send unavailable", conn_id);
        return -1;
    }

    ESP_LOGD(TAG, "conn_id=%d fd=%d len=%d", conn_id, fd, (int)len);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "conn %d send error: %d", conn_id, errno);
            return -1;
        }
        ESP_LOGI(TAG, "sent %d bytes", n);
        sent += (size_t)n;
        s_rx_bytes += (uint64_t)n;
    }
    return 0;
}

void tcp_mgr_close(uint8_t conn_id)
{
    if (conn_id >= TCP_MAX_CONNS) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    close_conn_locked(conn_id);
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "conn %d closed by host", conn_id);
}

void tcp_mgr_get_bytes(uint64_t* rx_bytes, uint64_t* tx_bytes)
{
    *rx_bytes = atomic_load(&s_rx_bytes);
    *tx_bytes = atomic_load(&s_tx_bytes);
}
