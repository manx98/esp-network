#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include "tcp_mgr.h"
#include <stdatomic.h>

static const char *TAG = "tcp_mgr";

/* ── Configuration ────────────────────────────────────────────── */

#define CONNECT_TIMEOUT_S    10

/* Recv buffer: max protocol payload so every read maps to one push frame. */
#define RX_BUF_SIZE          PROTO_MAX_PAYLOAD

/* Background task parameters */
#define RX_TASK_STACK        4096
#define RX_TASK_PRIORITY     5
#define SEND_TASK_STACK      4096
#define SEND_TASK_PRIORITY   5

/* Max outstanding send jobs.  32 × 1 KB ≈ 32 KB in-flight ceiling. */
#define SEND_QUEUE_DEPTH     32

/* TCP keep-alive: detect dead connections without app-level pinging.
 * After IDLE seconds idle, send a probe every INTVL seconds, drop after
 * CNT consecutive failures ≈ 30 + 3×5 = 45 s worst-case detection time. */
#define KA_IDLE_S    30
#define KA_INTVL_S    5
#define KA_CNT        3

/* ── Types ────────────────────────────────────────────────────── */

/* Heap-allocated send job; flexible array holds the payload bytes. */
typedef struct {
    size_t  len;
    uint8_t data[];
} send_job_t;

/* ── Module state ─────────────────────────────────────────────── */

static volatile int          s_fd = -1;   /* -1 = disconnected */
static SemaphoreHandle_t     s_mu;        /* protects s_fd */
static tcp_push_cb_t         s_push_cb;
static QueueHandle_t         s_send_queue;
static atomic_uint_least64_t s_rx_bytes;
static atomic_uint_least64_t s_tx_bytes;

/* ── Internal helpers ─────────────────────────────────────────── */

/*
 * Atomically swap s_fd to -1, close the socket, and notify ctrl.
 * Idempotent: if already closed the function is a no-op.
 * May be called from any task context.
 */
static void close_relay(const char *reason)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int fd = s_fd;
    s_fd   = -1;
    xSemaphoreGive(s_mu);

    if (fd < 0) return;   /* already closed by another path */

    /* Unblock any concurrent recv/send on this fd before closing. */
    shutdown(fd, SHUT_RDWR);
    close(fd);

    ESP_LOGI(TAG, "relay closed: %s", reason);
    s_push_cb(CMD_PROXY_CLOSED_PUSH, NULL, 0);
}

/* ── Background receive task ──────────────────────────────────── */

static void tcp_rx_task(void *arg)
{
    uint8_t *buf = malloc(RX_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "OOM in rx_task");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        /* Snapshot the current fd under lock. */
        xSemaphoreTake(s_mu, portMAX_DELAY);
        int fd = s_fd;
        xSemaphoreGive(s_mu);

        if (fd < 0) {
            /* No connection — sleep briefly to avoid spinning. */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /*
         * Use select() with a short timeout so:
         *   (a) we notice a new fd quickly after reconnect, and
         *   (b) we don't hog the CPU spinning on a quiet connection.
         */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 0, 20 * 1000 };  /* 20 ms */

        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            close_relay("rx select error");
            continue;
        }
        if (sel == 0) continue;  /* timeout */

        ssize_t n = recv(fd, buf, RX_BUF_SIZE, 0);
        if (n > 0) {
            atomic_fetch_add(&s_rx_bytes, (uint64_t)n);
            ESP_LOGD(TAG, "rx %d bytes → push", (int)n);
            s_push_cb(CMD_PROXY_DATA_PUSH, buf, (size_t)n);
        } else if (n == 0) {
            close_relay("remote closed");
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                close_relay(strerror(errno));
        }
    }

    /* Unreachable, but satisfy the compiler. */
    free(buf);
    vTaskDelete(NULL);
}

/* ── Background send task ─────────────────────────────────────── */

static void tcp_send_task(void *arg)
{
    while (1) {
        send_job_t *job = NULL;
        if (xQueueReceive(s_send_queue, &job, portMAX_DELAY) != pdTRUE || !job)
            continue;

        /* Re-read fd under lock so we catch a concurrent close_relay. */
        xSemaphoreTake(s_mu, portMAX_DELAY);
        int fd = s_fd;
        xSemaphoreGive(s_mu);

        if (fd < 0) {
            /* Relay was closed while this job was queued — discard silently. */
            free(job);
            continue;
        }

        /* Fully drain the job with retries; detect errors. */
        size_t sent = 0;
        bool   err  = false;
        while (sent < job->len) {
            ssize_t n = send(fd, job->data + sent, job->len - sent, 0);
            if (n <= 0) {
                ESP_LOGW(TAG, "send error (fd=%d): %s", fd, strerror(errno));
                err = true;
                break;
            }
            sent += (size_t)n;
            atomic_fetch_add(&s_tx_bytes, (uint64_t)n);
        }

        free(job);

        if (err)
            close_relay("send error");
    }
}

/* ── Public API ───────────────────────────────────────────────── */

void tcp_mgr_init(tcp_push_cb_t push_cb)
{
    s_push_cb = push_cb;
    s_mu      = xSemaphoreCreateMutex();
    configASSERT(s_mu);

    atomic_store(&s_rx_bytes, 0);
    atomic_store(&s_tx_bytes, 0);

    s_send_queue = xQueueCreate(SEND_QUEUE_DEPTH, sizeof(send_job_t *));
    configASSERT(s_send_queue);

    xTaskCreate(tcp_rx_task,   "tcp_rx",   RX_TASK_STACK,   NULL,
                RX_TASK_PRIORITY,   NULL);
    xTaskCreate(tcp_send_task, "tcp_send", SEND_TASK_STACK, NULL,
                SEND_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "relay init (ka=%ds/%ds×%d)", KA_IDLE_S, KA_INTVL_S, KA_CNT);
}

int tcp_mgr_connect(const char *host, uint16_t port)
{
    if (!host || strlen(host) > 255) return -1;

    /* Reject if a relay connection is already open. */
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool busy = (s_fd >= 0);
    xSemaphoreGive(s_mu);
    if (busy) {
        ESP_LOGW(TAG, "connect: already connected");
        return -2;
    }

    /* ── DNS resolve ── */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res  = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || !res) {
        ESP_LOGW(TAG, "getaddrinfo(%s:%u) err=%d", host, port, gai);
        if (res) freeaddrinfo(res);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed: %s", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    /* ── TCP keep-alive ── */
    int opt = 1;
    setsockopt(sock, SOL_SOCKET,  SO_KEEPALIVE,  &opt, sizeof(opt));
    opt = KA_IDLE_S;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,  &opt, sizeof(opt));
    opt = KA_INTVL_S;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &opt, sizeof(opt));
    opt = KA_CNT;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,   &opt, sizeof(opt));

    /* ── Non-blocking connect with select() timeout ── */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (rc != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect(%s:%u) failed: %s", host, port, strerror(errno));
        close(sock);
        return -1;
    }

    if (rc != 0) {
        fd_set wfds, efds;
        FD_ZERO(&wfds); FD_ZERO(&efds);
        FD_SET(sock, &wfds); FD_SET(sock, &efds);
        struct timeval tv = { CONNECT_TIMEOUT_S, 0 };
        int n = select(sock + 1, NULL, &wfds, &efds, &tv);
        if (n <= 0) {
            ESP_LOGW(TAG, "connect(%s:%u) timeout (n=%d)", host, port, n);
            close(sock);
            return -1;
        }
        int sock_err = 0;
        socklen_t errlen = sizeof(sock_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &sock_err, &errlen);
        if (sock_err != 0) {
            ESP_LOGW(TAG, "connect(%s:%u) error: %s", host, port, strerror(sock_err));
            close(sock);
            return -1;
        }
    }

    /* Restore blocking mode; rx_task uses select() + blocking recv. */
    fcntl(sock, F_SETFL, flags);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_fd = sock;
    xSemaphoreGive(s_mu);

    ESP_LOGI(TAG, "relay connected: %s:%u (fd=%d)", host, port, sock);
    return 0;
}

int tcp_mgr_send(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return -1;

    /* Silently drop if not connected — ctrl should check status first. */
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool up = (s_fd >= 0);
    xSemaphoreGive(s_mu);
    if (!up) return -1;

    send_job_t *job = malloc(sizeof(send_job_t) + len);
    if (!job) {
        ESP_LOGW(TAG, "send OOM (%u bytes)", (unsigned)len);
        return -1;
    }
    job->len = len;
    memcpy(job->data, data, len);

    if (xQueueSend(s_send_queue, &job, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "send queue full");
        free(job);
        return -1;
    }
    return 0;
}

void tcp_mgr_disconnect(void)
{
    close_relay("host request");
}

bool tcp_mgr_is_connected(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool c = (s_fd >= 0);
    xSemaphoreGive(s_mu);
    return c;
}

void tcp_mgr_get_bytes(uint64_t *rx_bytes, uint64_t *tx_bytes)
{
    *rx_bytes = atomic_load(&s_rx_bytes);
    *tx_bytes = atomic_load(&s_tx_bytes);
}
