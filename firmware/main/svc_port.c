/*
 * Service UDP port - EASSP protocol implementation.
 *
 * Listens on UDP:3950 for DISCOVER and CONFIGURE commands.
 * Sends INFO responses and periodic announcements.
 * Watchdog auto-stops streaming if server stops sending DISCOVER heartbeats.
 *
 * Uses POSIX sockets (lwip/sockets.h) on ESP8266 RTOS SDK v3.4.
 */

#include "svc_port.h"
#include "svc_protocol.h"
#include "config_mgr.h"

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
#include "lwip/def.h" /* ntohs() */

#include "board_config.h"
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

    s_stream_evt_grp = (EventGroupHandle_t)stream_evt_grp;

    s_mutex = xSemaphoreCreateMutex();
    s_stop_done_sem = xSemaphoreCreateBinary();
    if (!s_mutex || !s_stop_done_sem)
    {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        return ESP_ERR_NO_MEM;
    }

    /* Get MAC address. */
    esp_wifi_get_mac(ESP_IF_WIFI_STA, s_mac);
    ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    update_broadcast_addr();

    /* Create socket. */
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

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
        return ESP_FAIL;
    }

    /* Socket options. */
    int enable = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));

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
    return s_state != SVC_STOPPED;
}

void svc_port_notify_streaming_started(void)
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = SVC_STREAMING;
    s_last_discover_ticks = now_ticks();
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
        send_info(s_seq_counter++, &addr, port);
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
    if (s_sock < 0 || len == 0)
        return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = dest->addr;

    ssize_t sent = sendto(s_sock, data, len, 0,
                          (struct sockaddr *)&addr, sizeof(addr));
    if (sent < 0)
    {
        if (errno == 118 /* ENETUNREACH/EHOSTUNREACH */)
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

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    info->status = (s_state == SVC_STREAMING) ? SVC_STATUS_STREAMING : SVC_STATUS_IDLE;
    info->error = s_error_code;
    info->packets_sent = s_packets_sent;
    info->channels = s_channels;
    xSemaphoreGive(s_mutex);

    if (info->error != SVC_ERR_NONE)
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

    strncpy(info->firmware, FIRMWARE_VERSION, sizeof(info->firmware) - 1);
}

static void send_info(uint16_t req_seq, const ip_addr_t *dest, uint16_t port)
{
    svc_info_payload_t info;
    build_info_payload(&info);

    uint8_t buf[SVC_HEADER_SIZE + sizeof(svc_info_payload_t)];

    uint16_t seq;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    seq = s_seq_counter++;
    xSemaphoreGive(s_mutex);

    svc_header_t hdr;
    svc_header_init(&hdr, SVC_CMD_INFO, seq, sizeof(svc_info_payload_t));
    memcpy(buf, &hdr, SVC_HEADER_SIZE);
    memcpy(buf + SVC_HEADER_SIZE, &info, sizeof(svc_info_payload_t));

    send_to(buf, sizeof(buf), dest, port);
}

/* ---- Internal: command handlers ---- */

static void handle_discover(const svc_header_t *hdr,
                            const ip_addr_t *src_addr, uint16_t src_port)
{
    ESP_LOGI(TAG, "DISCOVER from " IPSTR ":%u (seq=%u, plen=%u)",
             IP2STR(src_addr), (unsigned)src_port,
             (unsigned)hdr->seq, (unsigned)hdr->payload_len);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_discover_ticks = now_ticks();
    s_server_svc_addr = *src_addr;
    s_server_svc_port = src_port;
    bool streaming = (s_state == SVC_STREAMING);
    xSemaphoreGive(s_mutex);

    if (streaming)
    {
        /* Heartbeat - no response, just reset watchdog. */
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

    /* Different destination - store it. */
    s_server_ip.addr = src_addr->addr;
    s_server_port = new_port;
    s_server_svc_addr = *src_addr;
    s_server_svc_port = src_port;
    s_packets_sent = 0;
    s_last_discover_ticks = now_ticks();
    xSemaphoreGive(s_mutex);

    /* If currently streaming, request stop AND wait for completion. */
    bool need_stop;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    need_stop = (s_state == SVC_STREAMING);
    xSemaphoreGive(s_mutex);

    if (need_stop && s_stream_evt_grp)
    {
        xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);

        /* Clear any stale signal, then wait for stop completion. */
        xSemaphoreTake(s_stop_done_sem, 0);
        if (xSemaphoreTake(s_stop_done_sem, pdMS_TO_TICKS(SVC_RECONFIGURE_STOP_TIMEOUT_MS)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Stop not completed within %d ms - proceeding anyway",
                     SVC_RECONFIGURE_STOP_TIMEOUT_MS);
        }
        else
        {
            ESP_LOGI(TAG, "Previous stream stopped - starting new stream");
        }
    }

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

    /* Request main loop to stop streaming. */
    if (s_stream_evt_grp)
    {
        xEventGroupSetBits(s_stream_evt_grp, STREAM_EVT_STOP_REQ);
    }

    /* Update state immediately so watchdog/announce logic is consistent. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = SVC_IDLE;
    xSemaphoreGive(s_mutex);

    /* Send INFO response (status = IDLE). */
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

        /* Receive (non-blocking-ish - short timeout for periodic tasks). */
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t len = recvfrom(s_sock, recv_buf, sizeof(recv_buf) - 1, 0,
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

                if (hdr.payload_len <= avail)
                {
                    if (hdr.cmd == SVC_CMD_DISCOVER)
                        handle_discover(&hdr, &ip, port);
                    else if (hdr.cmd == SVC_CMD_CONFIGURE && hdr.payload_len >= CFG_PAYLOAD_SZ)
                        handle_configure(&hdr, payload, &ip, port);
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
                    send_info(s_seq_counter++, &svc_addr, svc_port);
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
                    send_info(s_seq_counter++, &bcast, SVC_PORT_DEFAULT);
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Service port task exiting");
    vTaskDelete(NULL);
}
