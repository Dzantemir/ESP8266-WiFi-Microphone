/*
 * TCP transport for audio streaming — implementation.
 *
 * ESP = listener (TCP server). Принимает один коннект, стримит аудио с
 * length-prefix framing. Неблокирующий send (drop при переполнении).
 *
 * Согласовано с server/eassp_server.bas (TCP OPEN + framing read).
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_log.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "tcp_stream.h"
#include "svc_port.h" /* FIX (H1): for svc_port_get_server_ip() */



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
/* FIX (AUDIT-MEDIUM): s_running is read by the accept task in a loop and
 * written by tcp_stream_deinit() from another task. Mark volatile so
 * the compiler doesn't hoist the read out of the while(s_running) loop
 * (FreeRTOS convention for cross-task flag variables). */
static volatile bool s_running = false;

/* FIX (Bug #2): mutex guarding s_client_sock. Previously the accept task
 * (tcp_accept_task_fn) and the send path (tcp_stream_send) both read/wrote
 * s_client_sock without synchronization -> race: send() could fire on a
 * socket that accept() was about to close (EBADF) or on the new socket
 * before it was fully established (ENOTCONN). Now both paths take the
 * mutex around their access; send() copies the fd to a local under the
 * mutex and uses the local (does NOT hold the mutex during the blocking
 * send() itself). */
static SemaphoreHandle_t s_client_mutex = NULL;

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

        /* FIX (H1): authorize the client. Without this, ANY host on the LAN
         * can connect to the TCP audio port and instantly hijack the stream
         * (the previous code shutdown+close'd the current client and
         * installed the new one unconditionally). DoS is trivial: connect/
         * disconnect repeatedly, or connect-and-never-read to backpressure
         * the SO_SNDTIMEO=2s and stall the audio pipeline.
         *
         * Only accept the client whose IP matches the most recent
         * CONFIGURE's source IP (stored in svc_port.c as s_server_svc_addr).
         * If no CONFIGURE has been received yet, refuse (no legitimate
         * client should be connecting before configuring). */
        uint32_t allowed_ip = 0;
        bool have_server = svc_port_get_server_ip(&allowed_ip);
        if (!have_server || client_addr.sin_addr.s_addr != allowed_ip)
        {
            ESP_LOGW(TAG, "rejecting unauthorized TCP client %d.%d.%d.%d:%d",
                     (int)(client_addr.sin_addr.s_addr & 0xFF),
                     (int)((client_addr.sin_addr.s_addr >> 8) & 0xFF),
                     (int)((client_addr.sin_addr.s_addr >> 16) & 0xFF),
                     (int)((client_addr.sin_addr.s_addr >> 24) & 0xFF),
                     (int)ntohs(client_addr.sin_port));
            shutdown(new_sock, SHUT_RDWR);
            close(new_sock);
            continue;
        }

        /* Configure client socket:
         * - TCP_NODELAY: disable Nagle (send each frame immediately, no
         *   coalescing). ALWAYS set - critical for low-latency audio.
         *   Does NOT depend on WiFi QoS; it's a standard socket option.
         * - IP_TOS: Expedited Forwarding (voice priority in IP header).
         *   Routers with QoS may prioritize this traffic class.
         * - SO_SNDTIMEO: blocking send timeout (TCP_SEND_TIMEOUT_MS from
         *   menuconfig, default 2000 ms). On timeout -> close + reconnect.
         * - SO_KEEPALIVE (M7/T4): half-open detection during silence.
         *   Without keepalive, a crashed server that didn't send FIN/RST
         *   stays ESTABLISHED forever if stream_task_fn isn't calling
         *   send() (e.g. AGC noise gate below threshold). 10s idle,
         *   3s interval, 3 probes = dead in ~19s. */
        int flag = 1;
        if (setsockopt(new_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0)
            ESP_LOGW(TAG, "TCP_NODELAY setsockopt failed: errno=%d", errno);

        int tos = IPTOS_DSCP_EF;
        if (setsockopt(new_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0)
            ESP_LOGW(TAG, "IP_TOS setsockopt failed: errno=%d", errno);

        // FIX (AUDIT-LOW): SO_SNDBUF block removed - was commented out but
        // the comment at line ~99 still referenced "SO_SNDBUF 4 KB" as if
        // active. Default lwIP send buffer is fine for audio frame sizes.
        struct timeval sndto = {.tv_sec = TCP_SEND_TIMEOUT_MS / 1000,
                                .tv_usec = (TCP_SEND_TIMEOUT_MS % 1000) * 1000};
        if (setsockopt(new_sock, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto)) != 0)
            ESP_LOGW(TAG, "SO_SNDTIMEO setsockopt failed: errno=%d", errno);

        /* FIX (M7/T4): TCP keepalive so a crashed server is detected even
         * when the audio pipeline is silent (no send() calls to time out). */
        int keepalive = 1;
        int keepidle = 10; /* seconds before first probe */
        int keepintvl = 3; /* seconds between probes */
        int keepcnt = 3;   /* failed probes = dead */
        setsockopt(new_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        setsockopt(new_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
        setsockopt(new_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
        setsockopt(new_sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

        /* FIX (Bug #2): close old client and install new one ATOMICALLY under
         * the mutex. Previously the order was:
         *   s_client_sock = new_sock;     // send path may now use new_sock
         *   if (old >= 0) close(old);     // but old is still being read
         * which races with tcp_stream_send (different task) that may have
         * already captured old into a local and is mid-send(). Closing old
         * under it -> EBADF; and send() on new_sock before TCP handshake
         * completes -> ENOTCONN (errno 128, seen in logs).
         *
         * New order: under mutex, shutdown+close old FIRST (so any in-flight
         * send() on old fails fast with EPIPE/ENOTCONN, which the send path
         * handles), THEN publish new_sock. The send path always re-reads
         * s_client_sock under the mutex, so it never races.
         *
         * FIX (AUDIT-H1): re-check s_running under the mutex BEFORE
         * installing the new client. If tcp_stream_deinit() ran between
         * accept() returning and us reaching this point, s_running is now
         * false and we must NOT install a new client (deinit already
         * closed everything and nulled s_accept_task). Without this
         * check, the new client socket leaks + accept task handle is
         * already NULL, leaving no way to track the zombie task. */
        int old;
        if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (!s_running)
            {
                /* deinit won the race — refuse the new client and exit. */
                xSemaphoreGive(s_client_mutex);
                ESP_LOGW(TAG, "accept: s_running cleared during accept, refusing new client");
                shutdown(new_sock, SHUT_RDWR);
                close(new_sock);
                break;
            }
            old = s_client_sock;
            if (old >= 0)
            {
                shutdown(old, SHUT_RDWR);
                close(old);
                ESP_LOGI(TAG, "new client, closing old connection (sock=%d)", old);
            }
            s_client_sock = new_sock;
            xSemaphoreGive(s_client_mutex);
        }
        else
        {
            /* Mutex timeout (rare) — close the new socket, refuse the client.
             * Safer than installing without synchronization. */
            ESP_LOGW(TAG, "accept: client_mutex timeout -- refusing new client");
            shutdown(new_sock, SHUT_RDWR);
            close(new_sock);
            continue;
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
        /* FIX (H2): close any stale client connection under the mutex.
         * The previous code closed s_client_sock WITHOUT taking
         * s_client_mutex, racing with tcp_stream_send (in stream_task_fn)
         * which may have already snapshotted s_client_sock into a local
         * and be mid-send(). Closing under it -> EBADF on the snapshot,
         * handled by the send path, but the race violated the documented
         * invariant established by Bug #2. */
        if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
        {
            if (s_client_sock >= 0)
            {
                shutdown(s_client_sock, SHUT_RDWR);
                close(s_client_sock);
                s_client_sock = -1;
            }
            xSemaphoreGive(s_client_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "init_listen reuse: client_mutex timeout - client not closed");
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

    /* FIX (Bug #2): create the client mutex once, on first init. */
    if (!s_client_mutex)
    {
        s_client_mutex = xSemaphoreCreateMutex();
        if (!s_client_mutex)
        {
            ESP_LOGE(TAG, "Failed to create client mutex");
            close(s_listen_sock);
            s_listen_sock = -1;
            return ESP_ERR_NO_MEM;
        }
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
    /* FIX (Bug #2): close client under the mutex so tcp_stream_send can't
     * race on a half-closed fd. */
    if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        if (s_client_sock >= 0)
        {
            shutdown(s_client_sock, SHUT_RDWR);
            close(s_client_sock);
            s_client_sock = -1;
        }
        xSemaphoreGive(s_client_mutex);
    }
    else if (s_client_sock >= 0)
    {
        /* Fallback if mutex timed out (shouldn't happen — send path never
         * holds it for long). Close without the mutex; worst case is a
         * benign EBADF in the send path. */
        shutdown(s_client_sock, SHUT_RDWR);
        close(s_client_sock);
        s_client_sock = -1;
    }

    /* Give the accept task time to notice s_running=false (set above) and
     * exit via vTaskDelete(NULL). shutdown() unblocks accept() so the task
     * loops back, sees s_running=false, and deletes itself.
     * FIX (FW#4): poll for up to 2s instead of a single 200ms delay.
     * The accept task may be between accept() returning and looping back
     * to check s_running; a single 200ms delay could miss the window.
     * If it doesn't exit in 2s, log a warning (but don't force-delete —
     * vTaskDelete on a task inside accept/recv can orphan lwIP state).
     * We just NULL the handle and let the task self-delete eventually. */
    if (s_accept_task)
    {
        for (int i = 0; i < 20 && s_accept_task; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (s_accept_task)
        {
            ESP_LOGW(TAG, "tcp_stream: accept task did not exit in 2s - "
                     "leaving it (will self-delete; reboot if socket issues)");
        }
        s_accept_task = NULL;
    }

    s_listen_port = 0;
    s_drop_count = 0;
    return ESP_OK;
}

/* FIX (WiFi reconnect): re-create the TCP listening socket after WiFi
 * disconnect/reconnect. When WiFi disconnects, lwIP destroys the netif
 * structure. The old listening socket becomes a "zombie" — bound to a
 * destroyed interface, it never receives new incoming connections even
 * after WiFi reconnects with the same IP. Without this reinit, the server
 * cannot connect after a WiFi drop until the device reboots.
 *
 * Analogous to svc_port_reinit_socket() for the UDP service socket.
 *
 * Safe to call when TCP is not active (no-op if s_listen_sock < 0). */
esp_err_t tcp_stream_reinit_listener(void)
{
    if (s_listen_sock < 0)
        return ESP_OK; /* TCP not initialized - nothing to do */

    uint16_t port = s_listen_port;
    ESP_LOGI(TAG, "Reinitializing TCP listening socket after WiFi reconnect (port %u)",
             (unsigned)port);

    /* Tear down the existing listener + accept task (full deinit). */
    tcp_stream_deinit();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Re-create on the same port. */
    return tcp_stream_init_listen(port);
}

void tcp_stream_close_client(void)
{
    /* Close ONLY the active client connection. Keep the listening socket
     * and accept task alive so the next tcp_stream_init_listen() can reuse
     * them (avoids EADDRINUSE on quick stop→start cycles).
     *
     * Used by stop_streaming(): the transport layer is "stopped" in the
     * sense that no more audio is sent, but the listening socket stays
     * open so the server can reconnect immediately on the next CONFIGURE.
     *
     * FIX (Bug #2): close under the mutex so tcp_stream_send can't race. */
    if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        if (s_client_sock >= 0)
        {
            shutdown(s_client_sock, SHUT_RDWR);
            close(s_client_sock);
            s_client_sock = -1;
        }
        xSemaphoreGive(s_client_mutex);
    }
    else if (s_client_sock >= 0)
    {
        shutdown(s_client_sock, SHUT_RDWR);
        close(s_client_sock);
        s_client_sock = -1;
    }
    s_drop_count = 0;
}

bool tcp_stream_is_ready(void)
{
    /* FIX (M6): take the mutex for acquire/release semantics, mirroring
     * udp_stream_is_ready. On Xtensa the int read is atomic so there's no
     * torn read, but without a memory barrier the tx task may briefly
     * observe a stale value (e.g. see "ready" after accept already set
     * it to -1, or vice versa). */
    bool r = false;
    if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        r = (s_client_sock >= 0);
        xSemaphoreGive(s_client_mutex);
    }
    else
    {
        /* Best-effort fallback if mutex is contended. */
        r = (s_client_sock >= 0);
    }
    return r;
}

/* Static frame buffer — single-threaded (only tcp_send_task calls this),
 * so no mutex needed. Max frame = 2 (len prefix) + TCP_MAX_PAYLOAD (header + payload).
 *
 * FIX (AUDIT-MEDIUM): single source of truth for the TCP payload cap. Was
 * a literal 1416 duplicated in s_frame_buf declaration and the len check
 * in tcp_stream_send; if one changes without the other, memcpy overflows. */
#define TCP_MAX_PAYLOAD 1416
static uint8_t s_frame_buf[2 + TCP_MAX_PAYLOAD];

esp_err_t tcp_stream_send(const uint8_t *data, size_t len)
{
    if (!data || !len)
        return ESP_ERR_INVALID_ARG;
    /* FIX (AUDIT-MEDIUM): use the named constant; was a magic 1416.
     * FIX (AUDIT-MEDIUM): return ESP_ERR_INVALID_SIZE (not INVALID_ARG) -
     * the argument is valid, only its size exceeds the limit. Matches
     * rawtx_stream.c and udp_stream.c semantics. */
    if (len > TCP_MAX_PAYLOAD)
        return ESP_ERR_INVALID_SIZE;

    /* FIX (Bug #2): snapshot the client fd under the mutex. The accept
     * task may swap s_client_sock to a new fd at any moment (new client
     * connected); we must NOT send on a fd that's about to be closed by
     * accept. By copying to a local under the mutex, we get a stable fd
     * for the duration of this send(). If accept() replaces it meanwhile,
     * our local still points to the old fd — which accept() already
     * shutdown()+close()d, so send() will fail fast with EPIPE/ENOTCONN
     * (handled below) instead of corrupting the new client's stream. */
    int sock;
    if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        sock = s_client_sock;
        xSemaphoreGive(s_client_mutex);
    }
    else
    {
        /* Mutex timeout — extremely rare (accept/send never hold it long).
         * Drop the frame rather than risk an unsynchronized send. */
        return ESP_ERR_INVALID_STATE;
    }
    if (sock < 0)
        return ESP_ERR_INVALID_STATE; /* FIX (T6): state, not arg, error */

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
        int w = send(sock, s_frame_buf + sent, frame_len - sent, 0);
        if (w < 0)
        {
            /* EAGAIN on a blocking socket with SO_SNDTIMEO = timeout expired.
             * EPIPE/ENOTCONN (128) = the accept task already replaced this
             * fd and closed it under us, or peer closed.
             *
             * Only close s_client_sock if it STILL equals our snapshot (sock).
             * If accept() already replaced s_client_sock with a new fd and
             * closed the old one, we must NOT close sock again — the fd
             * number may have been reused by lwIP for a different socket,
             * and close(sock) would close THAT socket → memory leak/corruption.
             * This was the root cause of the 30KB heap drop: each failed
             * send did a double-close, leaking PCBs and pbufs in lwIP.
             *
             * FIX (AUDIT-LOW): save errno immediately - ESP_LOGW and
             * mutex operations may clobber it before we log.
             * FIX (AUDIT-HIGH): increment s_drop_count (was dead counter).
             * FIX (AUDIT-MEDIUM): shutdown() before close() for symmetric
             * cleanup with all other close paths in this file. */
            int saved_errno = errno;
            s_drop_count++;
            ESP_LOGW(TAG, "send() failed: errno=%d (sent=%u/%u, drops=%u) -- closing client",
                     saved_errno, (unsigned)sent, (unsigned)frame_len,
                     (unsigned)s_drop_count);
            if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                if (s_client_sock == sock)
                {
                    /* Our snapshot still matches - close it. */
                    shutdown(s_client_sock, SHUT_RDWR);
                    close(s_client_sock);
                    s_client_sock = -1;
                }
                /* If s_client_sock != sock, accept() already closed the old
                 * fd and installed a new one. Do NOT close sock - it's
                 * already closed, and the fd number may be reused. */
                xSemaphoreGive(s_client_mutex);
            }
            else
            {
                /* FIX (M8): mutex take timed out. s_client_sock still
                 * points to the broken fd; without this, the next
                 * tcp_stream_send will snapshot the same broken fd, send()
                 * will fail again, and we'll loop forever dropping audio.
                 * Force-clear s_client_sock without close() - the accept
                 * task will replace it. */
                ESP_LOGW(TAG, "send fail: client_mutex timeout - force-clearing s_client_sock");
                if (s_client_sock == sock)
                    s_client_sock = -1;
            }
            return ESP_FAIL;
        }
        if (w == 0)
        {
            /* Connection closed by peer. Same logic as above. */
            s_drop_count++;
            ESP_LOGW(TAG, "send() returned 0 -- connection closed by peer (drops=%u)",
                     (unsigned)s_drop_count);
            if (s_client_mutex && xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                if (s_client_sock == sock)
                {
                    shutdown(s_client_sock, SHUT_RDWR);
                    close(s_client_sock);
                    s_client_sock = -1;
                }
                xSemaphoreGive(s_client_mutex);
            }
            else
            {
                /* FIX (M8): same as above - force-clear to avoid looping. */
                if (s_client_sock == sock)
                    s_client_sock = -1;
            }
            return ESP_FAIL;
        }
        sent += (size_t)w;
    }

    return ESP_OK;
}
