/*
 * TCP transport for audio streaming — implementation.
 *
 * ESP = listener (TCP server). Принимает один коннект, стримит аудио с
 * length-prefix framing. Неблокирующий send (drop при переполнении).
 *
 * Согласовано с server/eassp_server.bas (TCP OPEN + framing read).
 */

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "board_config.h"

#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "tcp_stream.h"

#ifndef IPTOS_DSCP_EF
#define IPTOS_DSCP_EF 0xB8
#endif

static const char *TAG = "tcp_stream";

/* Listening socket (accept). */
static int s_listen_sock = -1;
/* Active client socket (connect accepted). -1 = no client. */
static int s_client_sock = -1;
/* Port we're listening on (for logging). */
static uint16_t s_listen_port = 0;
/* Accept task handle. */
static TaskHandle_t s_accept_task = NULL;
static bool s_running = false;

/* Drop counter for backpressure logging. */
static uint32_t s_drop_count = 0;

/* ---- Accept task: waits for incoming connect, replaces current client ---- */
static void tcp_accept_task_fn(void *arg)
{
    (void)arg;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    while (s_running)
    {
        int new_sock = accept(s_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (new_sock < 0)
        {
            if (s_running)
                ESP_LOGW(TAG, "accept failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Configure client socket: optional

        //  int flag = 1;
        //  setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        /* Set TOS (Type of Service) to Expedited Forwarding (Voice). */
        //  int tos = IPTOS_DSCP_EF;
        //   setsockopt(new_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

        // int sndbuf = 32768;
        // setsockopt(new_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        struct timeval sndto = {.tv_sec = TCP_SEND_TIMEOUT_MS / 1000,
                                .tv_usec = (TCP_SEND_TIMEOUT_MS % 1000) * 1000};
        setsockopt(new_sock, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

        /* Replace current client (close old, accept new). 1 client at a time. */
        int old = s_client_sock;
        s_client_sock = new_sock;
        if (old >= 0)
        {
            ESP_LOGI(TAG, "new client, closing old connection (sock=%d)", old);
            close(old);
        }

        ESP_LOGI(TAG, "client connected: %d.%d.%d.%d:%d (sock=%d)",
                 (int)(client_addr.sin_addr.s_addr & 0xFF),
                 (int)((client_addr.sin_addr.s_addr >> 8) & 0xFF),
                 (int)((client_addr.sin_addr.s_addr >> 16) & 0xFF),
                 (int)((client_addr.sin_addr.s_addr >> 24) & 0xFF),
                 (int)ntohs(client_addr.sin_port), new_sock);
        s_drop_count = 0;
    }

    vTaskDelete(NULL);
}

esp_err_t tcp_stream_init_listen(uint16_t port)
{
    /* REUSE LISTENING SOCKET across stop→start cycles. ESP8266 lwIP keeps
     * the listening socket in TIME_WAIT/CLOSE_WAIT even after close()+shutdown(),
     * causing EADDRINUSE (errno=112) on the next bind(). By keeping the
     * listening socket + accept task ALIVE across stream restarts (only
     * closing the CLIENT socket on stop), we avoid re-bind entirely.
     *
     * The listening socket + accept task are created ONCE on first init and
     * destroyed only in tcp_stream_deinit() (full teardown, e.g. transport
     * mode change via AT+XPORT + reboot). Stream restart (AT+HOTRESTART or
     * CMD_STOP→CONFIGURE) only closes the active client connection. */

    /* Already listening on the requested port? Listening socket + accept
     * task are alive — just reset client state and we're done. */
    if (s_listen_sock >= 0 && s_listen_port == port && s_accept_task)
    {
        /* Close any stale client connection from previous stream. */
        if (s_client_sock >= 0)
        {
            shutdown(s_client_sock, SHUT_RDWR);
            close(s_client_sock);
            s_client_sock = -1;
        }
        s_drop_count = 0;
        ESP_LOGI(TAG, "TCP reusing listening socket on port %u (accept task alive)",
                 (unsigned)port);
        return ESP_OK;
    }

    /* Different port, or first init, or previous deinit — full teardown. */
    if (s_listen_sock >= 0 || s_accept_task)
    {
        tcp_stream_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_listen_port = port;
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_sock < 0)
    {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return ESP_FAIL;
    }

    /* SO_REUSEADDR — allows bind() to succeed even if a previous socket
     * on this port is in TIME_WAIT (e.g. after a reboot). */
    int reuse = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(port);

    if (bind(s_listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        ESP_LOGE(TAG, "bind() port=%u failed: errno=%d", (unsigned)port, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    if (listen(s_listen_sock, 1) < 0)
    {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    /* Start accept task. */
    s_running = true;
    s_client_sock = -1;
    s_drop_count = 0;
    BaseType_t ok = xTaskCreate(tcp_accept_task_fn, "tcp_accept", TCP_ACCEPT_TASK_STACK, NULL,
                                TCP_ACCEPT_TASK_PRIO, &s_accept_task);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create accept task");
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TCP listening on port %u (waiting for client connect)", (unsigned)port);
    return ESP_OK;
}

esp_err_t tcp_stream_deinit(void)
{
    s_running = false;

    /* shutdown() forces accept() in the accept task to return immediately
     * (it may be blocked in accept() — close() alone doesn't always wake
     * it on ESP8266 lwIP). This is critical: without it, the listening
     * socket stays in LISTEN state and the next bind() fails with
     * EADDRINUSE (errno=112) on quick stop→start cycles. */
    if (s_listen_sock >= 0)
    {
        shutdown(s_listen_sock, SHUT_RDWR);
        close(s_listen_sock);
        s_listen_sock = -1;
    }
    if (s_client_sock >= 0)
    {
        shutdown(s_client_sock, SHUT_RDWR);
        close(s_client_sock);
        s_client_sock = -1;
    }

    /* Give the accept task time to notice s_running=false (set above) and
     * exit via vTaskDelete(NULL). shutdown() unblocks accept() so the task
     * loops back, sees s_running=false, and deletes itself. 200ms is plenty
     * (we don't use eTaskGetState — it's not reliably available across
     * ESP8266 RTOS SDK versions and needs extra includes). */
    if (s_accept_task)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_accept_task = NULL;
    }

    s_listen_port = 0;
    s_drop_count = 0;
    return ESP_OK;
}

void tcp_stream_close_client(void)
{
    /* Close ONLY the active client connection. Keep the listening socket
     * and accept task alive so the next tcp_stream_init_listen() can reuse
     * them (avoids EADDRINUSE on quick stop→start cycles).
     *
     * Used by stop_streaming(): the transport layer is "stopped" in the
     * sense that no more audio is sent, but the listening socket stays
     * open so the server can reconnect immediately on the next CONFIGURE. */
    if (s_client_sock >= 0)
    {
        shutdown(s_client_sock, SHUT_RDWR);
        close(s_client_sock);
        s_client_sock = -1;
    }
    s_drop_count = 0;
}

bool tcp_stream_is_ready(void)
{
    return s_client_sock >= 0;
}

/* Static frame buffer — single-threaded (only tcp_send_task calls this),
 * so no mutex needed. Max frame = 2 (len prefix) + 1416 (header + payload). */
static uint8_t s_frame_buf[2 + 1416];

esp_err_t tcp_stream_send(const uint8_t *data, size_t len)
{
    if (!data || !len || s_client_sock < 0)
        return ESP_ERR_INVALID_ARG;
    if (len > 1416)
        return ESP_ERR_INVALID_ARG;

    /* Build the FULL frame: [u16 length BE][data].
     * Send via BLOCKING send (SO_SNDTIMEO=2s). Blocking avoids the
     * deadlock that non-blocking + select() caused (ESP waits writable,
     * server waits data — nobody moves). With blocking, TCP stack drains
     * the buffer in background; backpressure flows through task queues. */
    s_frame_buf[0] = (uint8_t)((len >> 8) & 0xFF);
    s_frame_buf[1] = (uint8_t)(len & 0xFF);
    memcpy(s_frame_buf + 2, data, len);
    size_t frame_len = 2 + len;

    size_t sent = 0;
    while (sent < frame_len)
    {
        int w = send(s_client_sock, s_frame_buf + sent, frame_len - sent, 0);
        if (w < 0)
        {
            /* EAGAIN on a blocking socket with SO_SNDTIMEO = timeout expired. */
            ESP_LOGW(TAG, "send() failed: errno=%d (sent=%u/%u) — closing client",
                     errno, (unsigned)sent, (unsigned)frame_len);
            close(s_client_sock);
            s_client_sock = -1;
            return ESP_FAIL;
        }
        if (w == 0)
        {
            /* Connection closed by peer. */
            ESP_LOGW(TAG, "send() returned 0 — connection closed by peer");
            close(s_client_sock);
            s_client_sock = -1;
            return ESP_FAIL;
        }
        sent += (size_t)w;
    }

    return ESP_OK;
}
