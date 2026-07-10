/*
 * Service UDP port - EASSP protocol implementation.
 *
 * Listens on UDP:3950 for DISCOVER and CONFIGURE commands.
 * Sends INFO responses and periodic announcements.
 * Watchdog auto-stops streaming if server stops sending DISCOVER heartbeats.
 *
 * Uses POSIX sockets (lwip/sockets.h) on ESP8266 RTOS SDK v3.4.
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "tcpip_adapter.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "svc_port.h"
#include "svc_protocol.h"
#include "config_mgr.h"
#include "stream_mode.h"   /* FIX (AUDIT-XPORT-AUTOAPPLY): stream_mode_current_transport() */
#include "lwip/def.h" /* ntohs() */

extern uint32_t streaming_get_frame_ms(void);

static const char *TAG = "svc_port";

/* ---- State machine ---- */
typedef enum
{
    SVC_STOPPED = 0,
    SVC_IDLE = 1,
    SVC_STREAMING = 2,
} svc_state_t;

/* ---- Module state ---- */
static int s_sock = -1;
static uint16_t s_port = 0;  /* bound port (for reinit after WiFi reconnect) */
static SemaphoreHandle_t s_mutex = NULL;
static svc_state_t s_state = SVC_STOPPED;
static TaskHandle_t s_task_handle = NULL;
static uint32_t s_packets_sent = 0;

static uint8_t s_channels = AUDIO_CHANNELS;
static uint8_t s_error_code = SVC_ERR_NONE;
static uint16_t s_seq_counter = 0;

static ip_addr_t s_server_ip; /* audio destination */
static uint16_t s_server_port;
static ip_addr_t s_server_svc_addr; /* service port (for INFO replies) */
static uint16_t s_server_svc_port;

static uint8_t s_mac[6];
static ip_addr_t s_broadcast_addr;

static EventGroupHandle_t s_stream_evt_grp = NULL;
static SemaphoreHandle_t s_stop_done_sem = NULL;

static TickType_t s_last_discover_ticks = 0;

/* EHOSTUNREACH suppression - avoid log spam while WiFi is associating. */
static bool s_no_route_logged = false;

/* ---- Forward declarations ---- */
static void svc_task_fn(void *arg);
static void handle_discover(const svc_header_t *hdr,
                            const ip_addr_t *src_addr, uint16_t src_port);
static void handle_configure(const svc_header_t *hdr, const uint8_t *payload,
                             const ip_addr_t *src_addr, uint16_t src_port);


static void handle_stop(const svc_header_t *hdr,
                        const ip_addr_t *src_addr, uint16_t src_port);
static void send_info(uint16_t req_seq, const ip_addr_t *dest, uint16_t port);
static void build_info_payload(svc_info_payload_t *info);
static void send_to(const uint8_t *data, size_t len,
                    const ip_addr_t *dest, uint16_t port);
static TickType_t now_ticks(void);

/* ---- Helpers ---- */

static TickType_t now_ticks(void)
{
    return xTaskGetTickCount();
}

static void update_broadcast_addr(void)
{
    tcpip_adapter_ip_info_t ip_info;
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info) == ESP_OK)
    {
        ip_addr_t ip, mask;
        ip.addr = ip_info.ip.addr;
        mask.addr = ip_info.netmask.addr;
        /* Subnet-directed broadcast: IP | ~netmask */
        s_broadcast_addr.addr = ip.addr | ~mask.addr;
        ESP_LOGI(TAG, "Broadcast: " IPSTR " (from " IPSTR "/" IPSTR ")",
                 IP2STR(&s_broadcast_addr), IP2STR(&ip), IP2STR(&mask));
    }
    else
    {
        s_broadcast_addr.addr = IPADDR_BROADCAST;
        ESP_LOGW(TAG, "No IP yet, using 255.255.255.255 for broadcast");
    }
}

/* ---- Public API ---- */

esp_err_t svc_port_init(uint16_t port, void *stream_evt_grp)
{
    if (s_state != SVC_STOPPED)
    {
        ESP_LOGW(TAG, "Already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_port = port;
    s_stream_evt_grp = (EventGroupHandle_t)stream_evt_grp;

    s_mutex = xSemaphoreCreateMutex();
    s_stop_done_sem = xSemaphoreCreateBinary();
    if (!s_mutex || !s_stop_done_sem)
    {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        return ESP_ERR_NO_MEM;
    }

    /* Get MAC address.
     * FIX (AUDIT-LOW): check the return - if WiFi is not yet initialized
     * when svc_port_init runs, s_mac stays all-zeros and INFO packets
     * report MAC 00:00:00:00:00:00, which receivers may reject or log
     * as malformed. */
    esp_err_t mac_err = esp_wifi_get_mac(ESP_IF_WIFI_STA, s_mac);
    if (mac_err != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_wifi_get_mac failed: %s - INFO will report 00:...",
                 esp_err_to_name(mac_err));
        memset(s_mac, 0, sizeof(s_mac));
    }
    else
    {
        ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    }

    update_broadcast_addr();

    /* Create socket. */
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        /* FIX (S7): cleanup sync primitives on error. */
        if (s_mutex)        { vSemaphoreDelete(s_mutex);        s_mutex = NULL; }
        if (s_stop_done_sem){ vSemaphoreDelete(s_stop_done_sem); s_stop_done_sem = NULL; }
        return ESP_FAIL;
    }

    /* FIX (S3): set SO_REUSEADDR / SO_BROADCAST BEFORE bind(). Setting
     * SO_REUSEADDR after bind() has no effect on the current socket's bind
     * (it only affects future sockets). The whole point is to survive quick
     * stop->start cycles without EADDRINUSE. */
    int enable = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));

    /* Bind to service port. */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        close(s_sock);
        s_sock = -1;
        if (s_mutex)        { vSemaphoreDelete(s_mutex);        s_mutex = NULL; }
        if (s_stop_done_sem){ vSemaphoreDelete(s_stop_done_sem); s_stop_done_sem = NULL; }
        return ESP_FAIL;
    }

    /* Receive timeout - CRITICAL: without this, recvfrom blocks forever
     * and the periodic announce/watchdog code after it NEVER runs.
     * 200ms timeout lets the task loop through announcements ~5x/sec. */
    struct timeval tv_rcv = {.tv_sec = 0, .tv_usec = 200 * 1000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv));

    /* Send timeout. */
    struct timeval tv_snd = {.tv_sec = UDP_SEND_TIMEOUT_MS / 1000,
                             .tv_usec = (UDP_SEND_TIMEOUT_MS % 1000) * 1000};
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd));

    /* Create task. */
    BaseType_t res = xTaskCreate(svc_task_fn, "svc_port", TASK_STACK_SVC,
                                 NULL, TASK_PRIO_SVC, &s_task_handle);
    if (res != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create task");
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Service port active on UDP:%u (audio: %d ms, rate from config)",
             (unsigned)port, streaming_get_frame_ms());
    return ESP_OK;
}

bool svc_port_is_running(void)
{
    /* FIX (AUDIT-H6): read s_state under the mutex for acquire/release
     * semantics. svc_task_fn reads s_state under the mutex; an unsynchronized
     * read here could observe a stale value during a state transition. */
    bool running = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        running = (s_state != SVC_STOPPED);
        xSemaphoreGive(s_mutex);
    }
    else
    {
        /* Best-effort fallback if mutex is contended (e.g. deinit holds it). */
        running = (s_state != SVC_STOPPED);
    }
    return running;
}

/* FIX (H7): full teardown of the service port. Used when switching to a
 * transport that doesn't use svc_port (e.g. RAWTX). Closes the socket,
 * signals the task to exit, waits for it, frees sync primitives. */
void svc_port_deinit(void)
{
    if (s_state == SVC_STOPPED)
        return;

    s_state = SVC_STOPPED;

    /* Close socket - wakes any recvfrom() in the svc task (EBADF). */
    if (s_sock >= 0)
    {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
    }

    /* Wait for the task to exit (it polls s_state and breaks out of the
     * loop on SVC_STOPPED). FIX (FW#4): increased from 500ms to 3s.
     * The task may be inside recvfrom (200ms timeout) or inside
     * sendto/build_info. 500ms was too tight if a recvfrom just started.
     * Force-deleting a task inside lwIP orphans the lwIP mutex ->
     * all future socket calls deadlock. 3s covers 15 recvfrom cycles. */
    if (s_task_handle)
    {
        for (int i = 0; i < 30 && s_task_handle; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (s_task_handle)
        {
            ESP_LOGW(TAG, "svc_port: task did not exit in 3s - force-deleting "
                     "(may orphan lwIP mutex - reboot recommended)");
            vTaskDelete(s_task_handle);
            s_task_handle = NULL;
        }
    }

    if (s_mutex)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    if (s_stop_done_sem)
    {
        vSemaphoreDelete(s_stop_done_sem);
        s_stop_done_sem = NULL;
    }
}

/* Re-create the UDP socket after WiFi reconnect.
 *
 * ROOT CAUSE: when WiFi disconnects, lwIP destroys the netif structure. The
 * old socket (s_sock) becomes a "zombie" — bound to a destroyed interface.
 * After reconnect, sendto() on the zombie socket fails with ENETUNREACH
 * (errno 113) or silently drops packets. Broadcast announcements (INFO) stop
 * working → the receiver can no longer discover the ESP.
 *
 * Fix: on IP_EVENT_STA_GOT_IP (new IP obtained), wifi_sta.c calls this to
 * close the stale socket and open a fresh one bound to the same port. The
 * svc_port task keeps running — only the underlying fd is swapped.
 *
 * See: github.com/esp8266/Arduino#888 ("you need to reopen the UDP socket
 * after a Wifi disconnect"), github.com/esp8266/Arduino#969 ("interface
 * structure is destroyed").
 *
 * Thread-safety: called from the event-loop task (GOT_IP handler). The svc
 * task may be in recvfrom() on the old fd — close() wakes it (EBADF), it
 * loops back, sees the new fd. The mutex guards the fd swap. */
esp_err_t svc_port_reinit_socket(void)
{
    if (s_state == SVC_STOPPED)
        return ESP_ERR_INVALID_STATE;
    if (s_port == 0)
        return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Reinitializing UDP socket after WiFi reconnect (port %u)",
             (unsigned)s_port);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Close the stale socket (bound to the destroyed netif). */
    int old_sock = s_sock;
    s_sock = -1;
    if (old_sock >= 0)
    {
        shutdown(old_sock, SHUT_RDWR);
        close(old_sock);
    }

    /* Create a fresh socket bound to the same port on the new netif. */
    int new_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (new_sock < 0)
    {
        ESP_LOGE(TAG, "reinit: socket failed: errno=%d", errno);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    /* FIX (S3): set SO_REUSEADDR / SO_BROADCAST BEFORE bind() (mirroring
     * the fix in svc_port_init). Setting them after bind has no effect on
     * the current socket. */
    int enable = 1;
    setsockopt(new_sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    setsockopt(new_sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(s_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(new_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        ESP_LOGE(TAG, "reinit: bind failed: errno=%d", errno);
        close(new_sock);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    struct timeval tv_rcv = {.tv_sec = 0, .tv_usec = 200 * 1000};
    setsockopt(new_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv));
    struct timeval tv_snd = {.tv_sec = UDP_SEND_TIMEOUT_MS / 1000,
                             .tv_usec = (UDP_SEND_TIMEOUT_MS % 1000) * 1000};
    setsockopt(new_sock, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd));

    s_sock = new_sock;
    s_no_route_logged = false;  /* reset suppression — new socket, new chance */

    /* Refresh the broadcast address (netif changed → subnet may differ). */
    update_broadcast_addr();

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "UDP socket reinitialized (fd %d -> %d)", old_sock, new_sock);
    return ESP_OK;
}

void svc_port_notify_streaming_started(void)
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = SVC_STREAMING;
    s_last_discover_ticks = now_ticks();
    /* FIX (AUDIT-MEDIUM): clear any stale error code from a previous stream
     * (e.g. SVC_ERR_WATCHDOG from a watchdog timeout). Without this, INFO
     * packets would report ERROR indefinitely after a single watchdog
     * event - the receiver may refuse to reconfigure a device reporting
     * ERROR. A fresh stream start means the device is healthy again. */
    s_error_code = SVC_ERR_NONE;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Streaming started - DISCOVER watchdog active (%d s timeout), "
                  "periodic INFO every %d s",
             SVC_WATCHDOG_TIMEOUT_MS / 1000, SVC_INFO_INTERVAL_MS / 1000);
}

void svc_port_notify_streaming_stopped(void)
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = SVC_IDLE;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Streaming stopped - announcements resumed");
}

void svc_port_reset_watchdog(void)
{
    if (!s_mutex)
        return;
    /* FIX C2: timeout instead of portMAX_DELAY to avoid deadlock if the
     * mutex was orphaned by a force-deleted task. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "reset_watchdog: mutex timeout - skipping");
        return;
    }
    /* Only reset if we're streaming (watchdog only runs in that state).
     * If idle, this is a no-op. */
    if (s_state == SVC_STREAMING)
    {
        s_last_discover_ticks = now_ticks();
    }
    xSemaphoreGive(s_mutex);
}

void svc_port_notify_stop_complete(void)
{
    if (!s_mutex)
        return;
    if (s_stop_done_sem)
    {
        xSemaphoreGive(s_stop_done_sem);
    }
}

void svc_port_update_stats(uint32_t packets_sent)
{
    if (!s_mutex)
        return;
    /* FIX (C2): Use timeout instead of portMAX_DELAY. If a force-deleted
     * task orphaned this mutex, portMAX_DELAY would deadlock forever.
     * With 100ms timeout we just skip the stats update on contention. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "update_stats: mutex timeout - skipping");
        return;
    }
    s_packets_sent = packets_sent;
    xSemaphoreGive(s_mutex);
}

bool svc_port_get_stream_dest(uint32_t *host, uint16_t *port)
{
    if (!host || !port)
        return false;
    if (!s_mutex)
        return false;
    bool valid;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *host = s_server_ip.addr;
    *port = s_server_port;
    valid = (s_server_ip.addr != 0 && s_server_port != 0);
    xSemaphoreGive(s_mutex);
    return valid;
}

/* FIX (H1): expose the server's service-channel IP so the TCP accept task
 * can refuse unauthorized clients. */
bool svc_port_get_server_ip(uint32_t *ip)
{
    if (!ip || !s_mutex)
        return false;
    bool valid;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *ip = s_server_svc_addr.addr;
    valid = (s_server_svc_addr.addr != 0);
    xSemaphoreGive(s_mutex);
    return valid;
}

void svc_port_set_channels(uint8_t channels)
{
    if (!s_mutex)
        return;
    if (channels < 1)
        channels = 1;
    if (channels > 2)
        channels = 2;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_channels = channels;
    xSemaphoreGive(s_mutex);
}

void svc_port_set_error(uint8_t error_code)
{
    if (!s_mutex)
        return;
    ip_addr_t addr;
    uint16_t port;

    /* FIX (C2): timeout instead of portMAX_DELAY to avoid deadlock if
     * the mutex was orphaned by a force-deleted task. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "set_error: mutex timeout - error %u not reported",
                 (unsigned)error_code);
        return;
    }
    s_error_code = error_code;
    addr = s_server_svc_addr;
    port = s_server_svc_port;
    xSemaphoreGive(s_mutex);

    /* Trigger immediate INFO send to server if address known. */
    if (addr.addr != 0 && port != 0)
    {
        { uint16_t s; xSemaphoreTake(s_mutex, portMAX_DELAY); s = s_seq_counter++; xSemaphoreGive(s_mutex); send_info(s, &addr, port); }
    }
}

void svc_port_clear_error(void)
{
    if (!s_mutex)
        return;
    /* FIX (C2): timeout instead of portMAX_DELAY. This is called from
     * udp_on_stream_started() at the end of start_streaming(). If the
     * mutex was orphaned by a previous force-delete, portMAX_DELAY would
     * deadlock here -> watchdog reboot after ~8s. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        ESP_LOGW(TAG, "clear_error: mutex timeout - error flag not cleared");
        return;
    }
    s_error_code = SVC_ERR_NONE;
    xSemaphoreGive(s_mutex);
}

/* Called by wifi_sta.c on IP_EVENT_STA_GOT_IP to refresh broadcast addr. */
void svc_port_update_broadcast(void)
{
    if (!s_mutex)
        return;
    /* Проверяем s_state (SVC_STOPPED = не инициализирован). */
    if (s_state != SVC_STOPPED)
    {
        update_broadcast_addr();
    }
}

void svc_port_get_status(svc_port_status_t *status)
{
    if (!status)
        return;
    memset(status, 0, sizeof(*status));
    if (!s_mutex)
        return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    status->running = (s_state != SVC_STOPPED);
    status->streaming = (s_state == SVC_STREAMING);
    status->error_code = s_error_code;
    memcpy(status->mac, s_mac, 6);
    status->server_stream_ip = s_server_ip.addr;
    status->server_stream_port = s_server_port;
    status->server_svc_ip = s_server_svc_addr.addr;
    status->server_svc_port = s_server_svc_port;
    status->packets_sent = s_packets_sent;
    if (s_state == SVC_STREAMING)
    {
        uint32_t elapsed_ms = (uint32_t)((now_ticks() - s_last_discover_ticks) * portTICK_PERIOD_MS);
        int32_t remaining = (int32_t)SVC_WATCHDOG_TIMEOUT_MS - (int32_t)elapsed_ms;
        status->watchdog_remaining_ms = (remaining > 0) ? remaining : 0;
    }
    else
    {
        status->watchdog_remaining_ms = -1;
    }
    xSemaphoreGive(s_mutex);
}

/* ---- Internal: send helpers ---- */

static void send_to(const uint8_t *data, size_t len,
                    const ip_addr_t *dest, uint16_t port)
{
    /* FIX (S6): snapshot s_sock under the mutex. svc_port_reinit_socket
     * swaps s_sock under s_mutex; without this snapshot we could read
     * s_sock=5, reinit closes 5 and recycles the fd number for a new
     * socket, sendto(5,...) then goes on a different socket than intended
     * (benign because dest/port are params) or on a closed fd -> EBADF. */
    if (!s_mutex)
        return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return;
    int sock = s_sock;
    xSemaphoreGive(s_mutex);
    if (sock < 0 || len == 0)
        return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = dest->addr;

    ssize_t sent = sendto(sock, data, len, 0,
                          (struct sockaddr *)&addr, sizeof(addr));
    if (sent < 0)
    {
        /* FIX (AUDIT-MEDIUM): compare against symbolic errno names instead
         * of the magic number 118. The comment was ambiguous - on ESP8266
         * RTOS SDK v3.4 both ENETUNREACH and EHOSTUNREACH can fire during
         * WiFi (re)association, and the numeric value comes from newlib/lwip
         * which can differ from Linux. Provide fallback to 118 in case the
         * toolchain's errno.h doesn't define them (older SDKs). */
#ifndef ENETUNREACH
#define ENETUNREACH 118
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH 118
#endif
        if (errno == ENETUNREACH || errno == EHOSTUNREACH)
        {
            if (!s_no_route_logged)
            {
                ESP_LOGW(TAG, "sendto: no route (WiFi associating?)");
                s_no_route_logged = true;
            }
        }
        else
        {
            ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
        }
    }
    else
    {
        s_no_route_logged = false;
    }
}

static void build_info_payload(svc_info_payload_t *info)
{
    memset(info, 0, sizeof(*info));

    device_config_t cfg;
    config_get_copy(&cfg);

    info->codec_id = (cfg.codec_mode == CODEC_MODE_PCM) ? CODEC_ID_PCM : CODEC_ID_ADPCM;
    info->sample_rate = cfg.sample_rate;
    info->frame_ms = streaming_get_frame_ms();
    info->bits_per_sample = cfg.bits_per_sample;
    /* FIX (AUDIT-XPORT-AUTOAPPLY): report the ACTIVE transport, not the NVS
     * value. AT+XPORT saves to NVS but does NOT switch immediately - the
     * stream continues on the old transport until AT+HOTRESTART or AT+RST.
     * If we reported cfg.transport_mode (NVS), the server would see the new
     * transport while the stream is still on the old one, and could try to
     * auto-switch (causing interruptions). By reporting the ACTIVE transport,
     * the server always sees what's actually running. */
    info->transport_mode = stream_mode_current_transport();

    /* FIX (L22): capture s_state into a local inside the mutex and use the
     * local after release. The previous code re-read s_state without the
     * mutex at line 614, which could race with a concurrent state change. */
    svc_state_t state_local;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    state_local = s_state;
    info->status = (state_local == SVC_STREAMING) ? SVC_STATUS_STREAMING : SVC_STATUS_IDLE;
    info->error = s_error_code;
    info->packets_sent = s_packets_sent;
    info->channels = s_channels;
    xSemaphoreGive(s_mutex);

    /* Only override status to ERROR when streaming.
     * When IDLE (after stop), leftover error codes from the stop process
     * (e.g. send() fail on closed socket) should NOT be shown as ERROR -
     * the device is simply idle. */
    if (info->error != SVC_ERR_NONE && state_local == SVC_STREAMING)
    {
        info->status = SVC_STATUS_ERROR;
    }

    memcpy(info->mac, s_mac, 6);
    info->free_heap = esp_get_free_heap_size();

    wifi_ap_record_t ap_info;
    int8_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        rssi = ap_info.rssi;
    }
    info->wifi_rssi = rssi;

    /* FIX (AUDIT-LOW): explicit NUL-terminate (matches the hostname pattern
     * 3 lines below). Currently safe due to the memset(info, 0, sizeof(*info))
     * earlier in this function, but if that memset is ever removed the
     * strncpy could leave the field unterminated. */
    strncpy(info->firmware, FIRMWARE_VERSION, sizeof(info->firmware) - 1);
    info->firmware[sizeof(info->firmware) - 1] = '\0';

    /* v2.2: hostname for display in receiver UI (NUL-terminated, max 32 chars). */
    strncpy(info->hostname, cfg.hostname, sizeof(info->hostname) - 1);
    info->hostname[sizeof(info->hostname) - 1] = '\0';
}

static void send_info(uint16_t req_seq, const ip_addr_t *dest, uint16_t port)
{
    svc_info_payload_t info;
    build_info_payload(&info);

    uint8_t buf[SVC_HEADER_SIZE + sizeof(svc_info_payload_t)];

    /* Use req_seq directly for the header sequence number.
     *
     * Callers are responsible for sourcing the seq:
     *   - DISCOVER/CONFIGURE/STOP replies: pass the request's seq (echo)
     *   - Periodic / error-triggered INFO: caller increments s_seq_counter
     *     under s_mutex and passes the result.
     *
     * Previously send_info ALSO incremented s_seq_counter itself, causing a
     * double increment — seq numbers jumped by 2, and reply headers never
     * echoed the request's seq (the req_seq parameter was silently
     * discarded). */
    svc_header_t hdr;
    svc_header_init(&hdr, SVC_CMD_INFO, req_seq, sizeof(svc_info_payload_t));
    memcpy(buf, &hdr, SVC_HEADER_SIZE);
    memcpy(buf + SVC_HEADER_SIZE, &info, sizeof(svc_info_payload_t));

    send_to(buf, sizeof(buf), dest, port);
}

/* ---- Sender validation (zero-day defense) ----
 * When the device is STREAMING, only the CURRENT streaming server (the one
 * that sent CONFIGURE) should be able to:
 *   - STOP the stream (handle_stop)
 *   - Reset the watchdog via DISCOVER heartbeat (handle_discover)
 *   - Redirect the stream via re-CONFIGURE (handle_configure)
 * Without this check, any host on the same network can send CMD_STOP or
 * spoof DISCOVER heartbeats to keep a dead stream alive, or redirect the
 * audio to themselves. This is a security hardening, not a protocol
 * requirement — the EASSP protocol has no authentication.
 * When IDLE (not streaming), any sender is accepted (discovery + first
 * CONFIGURE must work from any server). */
static bool sender_is_current_server(const ip_addr_t *src_addr)
{
    /* If not streaming, accept any sender (initial discovery/configure). */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool streaming = (s_state == SVC_STREAMING);
    ip_addr_t server_svc = s_server_svc_addr;
    xSemaphoreGive(s_mutex);
    if (!streaming)
        return true;
    /* Streaming: only accept from the server that configured us. */
    return (src_addr->addr == server_svc.addr);
}

/* ---- Internal: command handlers ---- */

static void handle_discover(const svc_header_t *hdr,
                            const ip_addr_t *src_addr, uint16_t src_port)
{
    ESP_LOGI(TAG, "DISCOVER from " IPSTR ":%u (seq=%u, plen=%u)",
             IP2STR(src_addr), (unsigned)src_port,
             (unsigned)hdr->seq, (unsigned)hdr->payload_len);

    /* SECURITY: when streaming, only the current server's DISCOVER resets
     * the watchdog. A spoofed DISCOVER from another host is ignored (no
     * watchdog reset, no INFO response). This prevents an attacker from
     * keeping a dead stream alive or probing device state. */
    if (!sender_is_current_server(src_addr))
    {
        ESP_LOGW(TAG, "DISCOVER from non-server " IPSTR " - ignored (streaming)",
                 IP2STR(src_addr));
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_discover_ticks = now_ticks();
    s_server_svc_addr = *src_addr;
    s_server_svc_port = src_port;
    bool streaming = (s_state == SVC_STREAMING);
    xSemaphoreGive(s_mutex);

    if (streaming)
    {
        /* Heartbeat - no response, just reset watchdog.
         *
         * FIX (AUDIT-XPORT-AUTOAPPLY): do NOT send INFO when streaming.
         * The INFO payload contains the current transport_mode. If the user
         * did AT+XPORT=1 (saved to NVS but not applied), the INFO would
         * report transport_mode=1 (TCP) even though the stream is still
         * running on UDP. The server's (now-removed) AUTOTRANSPORT logic
         * would see the change and auto-restart the stream, interrupting
         * it. By not sending INFO during streaming, the server only learns
         * about transport changes when the user explicitly stops and
         * restarts the stream. */
        return;
    }

    /* Idle: respond with INFO. */
    send_info(hdr->seq, src_addr, src_port);
}

static void handle_configure(const svc_header_t *hdr, const uint8_t *payload,
                             const ip_addr_t *src_addr, uint16_t src_port)
{
    ESP_LOGI(TAG, "CONFIGURE from " IPSTR ":%u", IP2STR(src_addr), (unsigned)src_port);

    const svc_configure_payload_t *cfg = (const svc_configure_payload_t *)payload;

    if (cfg->stream_port == 0)
    {
        ESP_LOGW(TAG, "CONFIGURE: invalid stream port");
        return;
    }

    /* CONFIGURE is just a "Start Stream" trigger.
     * The ESP is the audio authority - it streams exactly what is in its NVS
     * config (set by AT+CH). The server learns the channel count from the INFO
     * packet and adapts its playback (WaveOut) accordingly. We intentionally
     * IGNORE any channel count in the CONFIGURE payload to avoid mismatches. */

    uint16_t new_port = ntohs(cfg->stream_port);

    /* Check if already streaming to same destination. */
    bool same;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Only compare IP and port - channels are controlled entirely by NVS
     * (via AT+CH), so we don't restart the stream if the server sends a
     * CONFIGURE with a different channel count. */
    same = (s_state == SVC_STREAMING) &&
           s_server_ip.addr == src_addr->addr &&
           s_server_port == new_port;

    if (same)
    {
        s_last_discover_ticks = now_ticks();
        s_server_svc_addr = *src_addr;
        s_server_svc_port = src_port;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Already streaming to same destination - resetting watchdog");
        send_info(hdr->seq, src_addr, src_port);
        return;
    }

    /* Different destination - store it.
     * NOTE: s_packets_sent is reset AFTER the old stream is stopped below,
     * not here. If we reset it here while the old stream is still running,
     * stream_task_fn keeps calling svc_port_update_stats(sent) on every
     * packet and overwrites our 0, so INFO packets during the transition
     * would show stale counts. */
    s_server_ip.addr = src_addr->addr;
    s_server_port = new_port;
    s_server_svc_addr = *src_addr;
    s_server_svc_port = src_port;
    s_last_discover_ticks = now_ticks();
    xSemaphoreGive(s_mutex);

    /* If currently streaming, request stop AND wait for completion. */
    bool need_stop;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    need_stop = (s_state == SVC_STREAMING);
    xSemaphoreGive(s_mutex);

    if (need_stop && s_stream_evt_grp)
    {
        /* FIX (H3): previously this blocked the svc task for up to 2 s
         * (SVC_RECONFIGURE_STOP_TIMEOUT_MS) waiting for s_stop_done_sem.
         * During that wait the svc task loop body did not iterate ->
         * recvfrom() didn't run -> DISCOVER heartbeats and any new
         * CONFIGURE/STOP commands were dropped (lwIP socket buffer small),
         * the periodic INFO sender didn't run, watchdog check didn't run,
         * announce broadcasts didn't run.
         *
         * New approach: set STOP_REQ and poll s_stop_done_sem with a SHORT
         * timeout (50 ms) in a loop, calling recvfrom() between polls so
         * the service channel keeps working. Total wait still bounded by
         * SVC_RECONFIGURE_STOP_TIMEOUT_MS. */
        /* FIX (AUDIT-MEDIUM): clear s_stop_done_sem BEFORE setting STOP_REQ.
         * The previous order (STOP_REQ then clear) had a lost-wakeup race:
         * if the main loop processed STOP_REQ and called
         * svc_port_notify_stop_complete (which gives the sem) between the
         * two lines, the signal was consumed and lost, then handle_configure
         * waited the full SVC_RECONFIGURE_STOP_TIMEOUT_MS for a signal that
         * already fired - adding 2s of latency to every reconfigure. */
        xSemaphoreTake(s_stop_done_sem, 0); /* clear stale signal */
        xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SVC_RECONFIGURE_STOP_TIMEOUT_MS);
        bool stopped = false;
        while (xTaskGetTickCount() < deadline)
        {
            /* Service the recvfrom loop between polls: drain any pending
             * commands so they aren't lost during the stop wait.
             *
             * FIX (AUDIT-H5): snapshot s_sock under the mutex before
             * recvfrom, same pattern as in svc_task_fn. */
            int drain_sock;
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                drain_sock = s_sock;
                xSemaphoreGive(s_mutex);
            }
            else
            {
                drain_sock = s_sock; /* best-effort */
            }
            uint8_t drain_buf[SVC_RECV_BUF_SIZE];
            struct sockaddr_in drain_addr;
            socklen_t drain_len = sizeof(drain_addr);
            ssize_t rlen = recvfrom(drain_sock, drain_buf, sizeof(drain_buf),
                                    MSG_DONTWAIT,
                                    (struct sockaddr *)&drain_addr, &drain_len);
            if (rlen > 0)
            {
                /* Best-effort: process the drained command. We can't call
                 * handle_configure/handle_stop recursively, but we can
                 * update the watchdog timestamp for DISCOVER heartbeats.
                 *
                 * FIX (AUDIT-H7): write s_last_discover_ticks under the
                 * mutex - svc_task_fn reads it under the mutex at line ~1002,
                 * a torn 32-bit tick value (or stale interleaved read)
                 * corrupts the watchdog calculation. */
                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    s_last_discover_ticks = now_ticks();
                    xSemaphoreGive(s_mutex);
                }
                else
                {
                    s_last_discover_ticks = now_ticks();
                }
            }
            if (xSemaphoreTake(s_stop_done_sem, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                stopped = true;
                break;
            }
        }
        if (stopped)
            ESP_LOGI(TAG, "Previous stream stopped - starting new stream");
        else
            ESP_LOGW(TAG, "Stop not completed within %d ms - proceeding anyway",
                     SVC_RECONFIGURE_STOP_TIMEOUT_MS);
    }

    /* Reset packet counter now that the old stream is stopped (its task
     * will no longer call svc_port_update_stats). The new stream starts
     * fresh from 0. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_packets_sent = 0;
    xSemaphoreGive(s_mutex);

    /* Send INFO response. */
    send_info(hdr->seq, src_addr, src_port);

    /* Request main loop to start streaming. */
    device_config_t dev_cfg;
    config_get_copy(&dev_cfg);

    ESP_LOGI(TAG, "Requesting stream start: " IPSTR ":%u (%u Hz, %d ms, NVS ch=%u)",
             IP2STR(src_addr), (unsigned)new_port,
             (unsigned)dev_cfg.sample_rate, streaming_get_frame_ms(),
             (unsigned)channel_format_to_count(dev_cfg.channel_format));

    if (s_stream_evt_grp)
    {
        xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_START_REQ);
    }
}

/* ---- CMD_STOP: explicit stream stop from server ---- */
static void handle_stop(const svc_header_t *hdr,
                        const ip_addr_t *src_addr, uint16_t src_port)
{
    ESP_LOGI(TAG, "CMD_STOP from " IPSTR ":%u - stopping stream",
             IP2STR(src_addr), (unsigned)src_port);

    /* SECURITY: only the current streaming server can stop the stream.
     * Without this, any host on the network can send CMD_STOP and kill
     * the stream. When IDLE, CMD_STOP is a no-op anyway (nothing to stop),
     * but we still validate to avoid logging noise from spoofers. */
    if (!sender_is_current_server(src_addr))
    {
        ESP_LOGW(TAG, "CMD_STOP from non-server " IPSTR " - ignored",
                 IP2STR(src_addr));
        return;
    }

    /* Request main loop to stop streaming. */
    if (s_stream_evt_grp)
    {
        xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);
    }

    /* FIX (M11): do NOT set s_state = SVC_IDLE here. STREAM_EVT_STOP_REQ is
     * just a request - the main loop processes it asynchronously and calls
     * svc_port_notify_streaming_stopped() when the actual stop completes.
     * Setting IDLE here immediately resumes announce broadcasts while the
     * audio pipeline is still running - a second receiver seeing the
     * announce may send CONFIGURE and kick off the legitimate stream. The
     * INFO reply below will report the current state (SVC_STREAMING),
     * which is honest ("I'm stopping, still streaming"). */
    /* (state left as SVC_STREAMING; notify_streaming_stopped will set IDLE) */

    /* Send INFO response reflecting the current state. */
    send_info(hdr->seq, src_addr, src_port);
}

/* ---- Internal: service task ---- */

static void svc_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Service port task started");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = SVC_IDLE;
    xSemaphoreGive(s_mutex);

    uint8_t recv_buf[SVC_RECV_BUF_SIZE];
    TickType_t last_info = 0;
#if SVC_ANNOUNCE_ENABLED
    uint32_t announce_ms = SVC_ANNOUNCE_MIN_MS;
    TickType_t last_announce = 0;
#endif

    while (1)
    {
        /* Exit check. */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        svc_state_t st = s_state;
        xSemaphoreGive(s_mutex);
        if (st == SVC_STOPPED)
            break;

        /* Receive (non-blocking-ish - short timeout for periodic tasks).
         *
         * FIX (AUDIT-H5): snapshot s_sock under the mutex before recvfrom.
         * svc_port_reinit_socket() (called from the WiFi reconnect task)
         * closes+replaces s_sock under the mutex. Without this snapshot,
         * a WiFi reconnect during recvfrom would race on the closed fd
         * (EBADF) or on a recycled fd number (reading from a different
         * socket). Matches the pattern already used in send_to(). */
        int sock_snapshot;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            sock_snapshot = s_sock;
            xSemaphoreGive(s_mutex);
        }
        else
        {
            sock_snapshot = s_sock; /* best-effort */
        }
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        /* FIX (L24): use sizeof(recv_buf) - the buffer is never used as a
         * C string (no strncpy/strlen on it), so the -1 was unexplained
         * and wasted 1 byte of RX capacity. */
        ssize_t len = recvfrom(sock_snapshot, recv_buf, sizeof(recv_buf), 0,
                               (struct sockaddr *)&src, &slen);

        if (len >= SVC_HEADER_SIZE)
        {
            svc_header_t hdr;
            memcpy(&hdr, recv_buf, SVC_HEADER_SIZE);

            if (hdr.magic[0] == EASSP_MAGIC0 && hdr.magic[1] == EASSP_MAGIC1 &&
                hdr.version == EASSP_VER)
            {

                ip_addr_t ip = {.addr = src.sin_addr.s_addr};
                uint16_t port = ntohs(src.sin_port);
                const uint8_t *payload = recv_buf + SVC_HEADER_SIZE;
                size_t avail = (size_t)len - SVC_HEADER_SIZE;

                /* SECURITY (zero-day defense): validate payload_len.
                 * payload_len is uint16 (max 65535) but our buffer is only
                 * SVC_RECV_BUF_SIZE (256). The check `payload_len <= avail`
                 * below already prevents reading past the buffer, but an
                 * absurdly large payload_len (e.g. 60000) with a short
                 * packet is a clear sign of a malformed/malicious packet.
                 * Reject early with a clear log.
                 *
                 * FIX (AUDIT-MEDIUM): cast payload_len to size_t to avoid
                 * sign-compare warning (uint16_t promotes to int, then is
                 * compared with size_t). */
                if ((size_t)hdr.payload_len > sizeof(recv_buf) - SVC_HEADER_SIZE)
                {
                    ESP_LOGW(TAG, "RX payload_len %u > buf capacity %u - rejected",
                             (unsigned)hdr.payload_len,
                             (unsigned)(sizeof(recv_buf) - SVC_HEADER_SIZE));
                }
                else if ((size_t)hdr.payload_len <= avail)
                {
                    if (hdr.cmd == SVC_CMD_DISCOVER)
                        handle_discover(&hdr, &ip, port);
                    else if (hdr.cmd == SVC_CMD_CONFIGURE && hdr.payload_len >= CFG_PAYLOAD_SZ)
                        handle_configure(&hdr, payload, &ip, port);
                    else if (hdr.cmd == SVC_CMD_CONFIGURE)
                        /* FIX (L23): explicit branch for truncated CONFIGURE
                         * so the log says what's wrong, not "unknown". */
                        ESP_LOGW(TAG, "RX CONFIGURE plen=%u too short (need %u)",
                                 (unsigned)hdr.payload_len, (unsigned)CFG_PAYLOAD_SZ);
                    else if (hdr.cmd == SVC_CMD_STOP)
                        handle_stop(&hdr, &ip, port);
                    else
                        ESP_LOGW(TAG, "RX cmd=0x%02X plen=%u (unknown/unhandled)",
                                 (unsigned)hdr.cmd, (unsigned)hdr.payload_len);
                }
                else
                {
                    ESP_LOGW(TAG, "RX truncated: cmd=0x%02X plen=%u avail=%u",
                             (unsigned)hdr.cmd, (unsigned)hdr.payload_len, (unsigned)avail);
                }
            }
            else if (len > 0)
            {
                ESP_LOGW(TAG, "RX bad magic/ver: %02X %02X %02X (len=%d)",
                         (unsigned)hdr.magic[0], (unsigned)hdr.magic[1],
                         (unsigned)hdr.version, (int)len);
            }
        }

        /* Periodic: INFO / watchdog / announce - read state once. */
        TickType_t now = now_ticks();
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        st = s_state;
        ip_addr_t svc_addr = s_server_svc_addr;
        uint16_t svc_port = s_server_svc_port;
        uint32_t elapsed = (uint32_t)(now - s_last_discover_ticks) * portTICK_PERIOD_MS;
        ip_addr_t bcast = s_broadcast_addr;
        xSemaphoreGive(s_mutex);

        if (st == SVC_STREAMING)
        {
            /* Periodic INFO. */
            if ((uint32_t)(now - last_info) * portTICK_PERIOD_MS >= SVC_INFO_INTERVAL_MS)
            {
                last_info = now;
                if (svc_addr.addr)
                    { uint16_t s; xSemaphoreTake(s_mutex, portMAX_DELAY); s = s_seq_counter++; xSemaphoreGive(s_mutex); send_info(s, &svc_addr, svc_port); }
            }
            /* Watchdog. */
            if (elapsed >= SVC_WATCHDOG_TIMEOUT_MS)
            {
                ESP_LOGW(TAG, "Watchdog expired (%u ms) - stopping", (unsigned)elapsed);
                if (s_stream_evt_grp)
                    xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_state = SVC_IDLE;
                s_error_code = SVC_ERR_WATCHDOG;
                xSemaphoreGive(s_mutex);
            }
        }
#if SVC_ANNOUNCE_ENABLED
        else if (st == SVC_IDLE)
        {
            if ((uint32_t)(now - last_announce) * portTICK_PERIOD_MS >= announce_ms)
            {
                last_announce = now;
                uint32_t range = SVC_ANNOUNCE_MAX_MS - SVC_ANNOUNCE_MIN_MS;
                announce_ms = SVC_ANNOUNCE_MIN_MS + (esp_random() % (range + 1));
                if (bcast.addr)
                {
                    /* FIX (M9): send to the ACTUAL bound port (s_port), not
                     * the compile-time SVC_PORT_DEFAULT. If the service port
                     * was changed at runtime (or via a non-default Kconfig),
                     * receivers listening on the actual port would never
                     * see the broadcast announce. */
                    uint16_t s;
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    s = s_seq_counter++;
                    xSemaphoreGive(s_mutex);
                    send_info(s, &bcast, s_port);
                    /* FIX (DIAG-ANNOUNCE): log every Nth announce so we can
                     * verify the announce path is alive after stop_streaming.
                     * Without this, announce is silent and we can't tell if
                     * ESP is sending or not. Log every 5th (~5-25 sec apart). */
                    static uint32_t announce_log_counter = 0;
                    if ((++announce_log_counter % 5) == 1)
                    {
                        ESP_LOGI(TAG, "announce #%u -> " IPSTR ":%u (state=IDLE)",
                                 (unsigned)announce_log_counter,
                                 IP2STR(&bcast), (unsigned)s_port);
                    }
                }
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Service port task exiting");
    vTaskDelete(NULL);
}
