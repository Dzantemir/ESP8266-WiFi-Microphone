/*
 * UDP transport — sends encoded audio packets to a receiver via a standard
 * UDP socket (lwIP). Requires WiFi AP association (router).
 *
 * Independent module — does not share state with tcp_stream.c or
 * rawtx_stream.c. Raw 802.11 TX is in rawtx_stream.c.
 *
 * FIX (Bug #2 UDP variant): added s_state_mutex to guard s_sock / s_ready /
 * s_dest. Previously the Main task (init/deinit) and Stream TX task (send)
 * accessed these without synchronization -> race:
 *   - send() on a fd that deinit() just closed -> EBADF (usually benign on
 *     lwIP, but can corrupt internal state on some SDK versions)
 *   - worse: after deinit + re-init, the new socket may reuse the SAME fd
 *     number, and a sendto() still in-flight from before the close (lwIP
 *     sendto can block up to SO_SNDTIMEO = 2s) would send on the NEW socket
 *     with the OLD destination -> packets to wrong address.
 *   - s_ready (bool) read/write was not memory-barriered.
 * Now send() snapshots s_sock + s_dest to locals under the mutex and uses
 * the locals for sendto() (does NOT hold the mutex during the blocking
 * sendto itself). deinit() takes the mutex before close()+clear.
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_log.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "udp_stream.h"

#ifndef IPTOS_DSCP_EF
#define IPTOS_DSCP_EF 0xB8
#endif

static const char *TAG = "udp";

static int s_sock = -1;
static struct sockaddr_in s_dest;
static bool s_ready = false;

/* FIX (Bug #2 UDP): mutex guarding s_sock / s_ready / s_dest. Created on
 * first init; destroyed in deinit. NULL before first init or after full
 * deinit — all access points check for NULL before taking. */
static SemaphoreHandle_t s_state_mutex = NULL;

/* Create the mutex on first use. Idempotent.
 * FIX (AUDIT-HIGH): returns bool so callers can detect allocation failure
 * (previously silent fallthrough -> udp_stream_init proceeded with
 * s_state_mutex=NULL, all subsequent state changes unsynchronized). */
static bool ensure_mutex(void)
{
    if (!s_state_mutex)
    {
        s_state_mutex = xSemaphoreCreateMutex();
        if (!s_state_mutex)
        {
            ESP_LOGE(TAG, "ensure_mutex: xSemaphoreCreateMutex failed");
            return false;
        }
    }
    return true;
}

/* FIX (AUDIT-MEDIUM): UDP payload upper bound. With MTU 1500 - 20 IP - 8 UDP
 * = 1472 bytes max unfragmented. We cap at 1400 to leave headroom for
 * WiFi encap (WPA/CCMP adds 8-16 bytes, 802.11 header 30+ bytes) and match
 * the RAWTX cap. Larger payloads trigger IP fragmentation -> loss of any
 * fragment loses the whole datagram (amplifies packet loss on congested WiFi). */
#define UDP_MAX_PAYLOAD 1400

esp_err_t udp_stream_init(uint32_t host_ip, uint16_t host_port)
{
    /* FIX (AUDIT-HIGH): propagate mutex-alloc failure instead of silently
     * continuing without synchronization. */
    if (!ensure_mutex())
    {
        ESP_LOGE(TAG, "init: mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* Take mutex for the whole init. If s_ready is already true, caller
     * should have called udp_stream_deinit() first — but be defensive:
     * close any existing socket under the mutex before re-creating. */
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGE(TAG, "init: state_mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    if (s_ready && s_sock >= 0)
    {
        /* FIX (M5): mirror udp_stream_deinit - call shutdown() before
         * close() to unblock any sendto() stuck in lwIP on this socket.
         * Without shutdown(), close() alone may not wake sendto() on
         * ESP8266 lwIP, leaving it blocked up to SO_SNDTIMEO=2s. */
        ESP_LOGW(TAG, "init: already ready -- closing old socket first");
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
        s_ready = false;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        ESP_LOGE(TAG, "socket: errno=%d", errno);
        if (s_state_mutex) xSemaphoreGive(s_state_mutex);
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = UDP_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (UDP_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Set TOS (Type of Service) to Expedited Forwarding (Voice). */
    int tos = IPTOS_DSCP_EF;
    setsockopt(s_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons(host_port);
    s_dest.sin_addr.s_addr = host_ip; /* already network byte order */

    ESP_LOGI(TAG, "UDP -> %d.%d.%d.%d:%u",
             (int)(host_ip & 0xFF), (int)((host_ip >> 8) & 0xFF),
             (int)((host_ip >> 16) & 0xFF), (int)((host_ip >> 24) & 0xFF),
             (unsigned)host_port);

    /* Order matters: set s_dest BEFORE s_ready=true so that a racing send()
     * (which checks s_ready first then reads s_dest) never sees s_ready=true
     * with a stale s_dest. Under the mutex this is strictly ordered. */
    s_ready = true;

    if (s_state_mutex) xSemaphoreGive(s_state_mutex);
    return ESP_OK;
}

esp_err_t udp_stream_deinit(void)
{
    /* FIX (Bug #2 UDP): close under the mutex so a racing send() can't
     * capture s_sock into a local and then sendto() on a closed fd. */
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        if (s_sock >= 0)
        {
            /* shutdown() first to unblock any sendto() stuck in lwIP on
             * this socket (ESP8266 lwIP sometimes doesn't wake sendto()
             * on close() alone for UDP). */
            shutdown(s_sock, SHUT_RDWR);
            close(s_sock);
            s_sock = -1;
        }
        s_ready = false;
        xSemaphoreGive(s_state_mutex);
    }
    else
    {
        /* Fallback if mutex timed out (shouldn't happen — send path never
         * holds it for long). Close without the mutex; worst case is a
         * benign EBADF in the send path, which it already handles. */
        if (s_sock >= 0)
        {
            shutdown(s_sock, SHUT_RDWR);
            close(s_sock);
            s_sock = -1;
        }
        s_ready = false;
    }
    return ESP_OK;
}

bool udp_stream_is_ready(void)
{
    /* Read under mutex for memory-barrier consistency (bool r/w is atomic
     * on Xtensa, but the mutex provides the acquire/release semantics that
     * a bare read doesn't). */
    bool r = false;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        r = s_ready;
        xSemaphoreGive(s_state_mutex);
    }
    else
    {
        /* Mutex timeout — return the raw value (best effort). */
        r = s_ready;
    }
    return r;
}

esp_err_t udp_stream_send(const uint8_t *data, size_t len)
{
    if (!data || !len)
        return ESP_ERR_INVALID_ARG;
    /* FIX (AUDIT-MEDIUM): reject oversized payloads to avoid IP fragmentation. */
    if (len > UDP_MAX_PAYLOAD)
    {
        ESP_LOGW(TAG, "send: payload %u > %u, rejecting",
                 (unsigned)len, (unsigned)UDP_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }

    /* FIX (Bug #2 UDP): snapshot sock + dest under the mutex. deinit() may
     * close s_sock at any moment (stop_streaming / transport switch); we
     * must NOT sendto() on a fd that's about to be closed. By copying to
     * locals under the mutex, we get a stable fd + dest for the duration
     * of this sendto(). If deinit() closes it meanwhile, our local still
     * points to the old fd — which deinit() already close()d, so sendto()
     * will fail fast with EBADF (handled below) instead of corrupting a
     * new socket that init() may have created with the same fd number. */
    int sock;
    struct sockaddr_in dest;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        sock = s_sock;
        dest = s_dest;
        bool ready = s_ready;
        xSemaphoreGive(s_state_mutex);
        if (!ready || sock < 0)
            return ESP_ERR_INVALID_STATE;
    }
    else
    {
        /* Mutex timeout — extremely rare (deinit never holds it for long).
         * Drop the frame rather than risk an unsynchronized send. */
        return ESP_ERR_INVALID_STATE;
    }

    if (sendto(sock, data, len, 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0)
    {
        /* Common errors:
         *   EBADF (9)   — deinit closed the fd under us; benign.
         *   ENOMEM (12) — lwIP out of buffers (WiFi congested); drop frame.
         *   EHOSTUNREACH (118) — WiFi still associating; drop frame.
         *
         * FIX (AUDIT-LOW): log the errno so congestion vs. deinit-race vs.
         * no-association can be distinguished during debugging.
         *
         * FIX (log-fix-D): on ENOMEM (12), add a 1ms backoff. The log
         * showed 10+ consecutive errno=12 events because the TX loop
         * immediately retried sendto() with the next frame, but lwIP's
         * send buffer was still full from the previous failed send.
         * A 1ms delay gives lwIP time to drain one buffer slot, so the
         * next frame has a chance to succeed instead of also failing.
         * This reduces burst-drop patterns that cause server underruns. */
        int saved_errno = errno;
        ESP_LOGW(TAG, "sendto failed: errno=%d len=%u", saved_errno, (unsigned)len);
        if (saved_errno == 12) /* ENOMEM - lwIP buffer exhaustion */
        {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        (void)saved_errno;
        return ESP_FAIL;
    }
    return ESP_OK;
}
