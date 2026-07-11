/*
 * WiFi STA mode with exponential backoff reconnection.
 *
 * Supports two modes:
 * 1. UDP mode: connects to AP, gets IP via DHCP.
 * 2. Raw 802.11 TX mode: starts radio without AP connection,
 *    sets a fixed channel, and marks WiFi as "ready" for esp_wifi_80211_tx.
 *
 * =====================  DESIGN (follows Espressif's official example)  =====================
 *
 * The event handler follows the canonical pattern from ESP8266_RTOS_SDK and
 * ESP-IDF examples/wifi/getting_started/station/main/station_example_main.c:
 *
 *   WIFI_EVENT_STA_START       → esp_wifi_connect()       (directly in handler)
 *   WIFI_EVENT_STA_DISCONNECTED → schedule reconnect       (via event-group bit)
 *   IP_EVENT_STA_GOT_IP        → reset backoff, set CONNECTED bit
 *
 * Per Espressif's Wi-Fi Driver doc (wifi.rst §"Wi-Fi Reconnect"):
 *   "The recommended reconnect strategy is to call esp_wifi_connect() on
 *    receiving event WIFI_EVENT_STA_DISCONNECTED."
 *   "If the event is raised because esp_wifi_disconnect() is called, the
 *    application should not call esp_wifi_connect() to reconnect. It's
 *    application's responsibility to distinguish."
 *
 * We distinguish via the s_intentional_disconnect flag (set before our own
 * esp_wifi_disconnect() calls), NOT via reason codes — AUTH_LEAVE (3) and
 * ASSOC_LEAVE (8) can come from the AP too (arduino-esp32#7210).
 *
 * Backoff: a dedicated lightweight task applies exponential backoff
 * (1 s → 2 s → 4 s → 8 s → 15 s max, reset to 1 s on GOT_IP) and retries
 * indefinitely. The task is needed because calling esp_wifi_connect() directly
 * in the STA_DISCONNECTED handler with a vTaskDelay for backoff would block
 * the event-loop task (which processes ALL events). Using esp_timer for backoff
 * risks the #3458 deadlock (timer task ↔ WiFi task mutex cross-acquisition),
 * which is NOT fixed in ESP8266 RTOS SDK v3.4. A dedicated task avoids both
 * problems.
 *
 * NO reboot, NO failure cap. NO_AP_FOUND (201), BEACON_TIMEOUT, AP kicks, auth
 * failures — all retried with the same infinite backoff. These are transient
 * conditions; rebooting on them is wrong.
 * ==========================================================================================
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "tcpip_adapter.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "wifi_sta.h"
#include "svc_port.h"
#include "stream_control.h"
/* FIX (WiFi reconnect): needed for tcp_stream_reinit_listener() call when
 * the TCP transport is active. Header is safe to include unconditionally
 * (tcp_stream.c is always compiled, even when transport_mode != TCP). */
#include "tcp_stream.h"

static const char *TAG = "wifi_sta";

/* Event-group bits. */
#define WIFI_EVT_CONNECTED (1 << 0) /* GOT_IP received (UDP mode) */
#define WIFI_EVT_GOT_IP (1 << 1)
#define WIFI_EVT_STA_STARTED (1 << 2)   /* raw TX: STA_START fired, radio up */
#define WIFI_EVT_RECONNECT_REQ (1 << 3) /* set by STA_DISCONNECTED handler */
#define WIFI_EVT_EXIT (1 << 4)          /* set by deinit to stop the task */
#define WIFI_EVT_APPLY_CACHED (1 << 5)  /* set by STA_CONNECTED: apply cached IP */
/* FIX (H9): set by IP_EVENT_STA_GOT_IP handler; the reconnect task performs
 * the heavy svc_port_reinit_socket() / svc_port_update_broadcast() calls
 * instead of running them in the event-loop task (which serializes all
 * WiFi/IP events and has a small stack on ESP8266). */
#define WIFI_EVT_REINIT_SOCKET (1 << 6)

static EventGroupHandle_t s_wifi_evt = NULL;
static SemaphoreHandle_t s_backoff_mtx = NULL;
static uint32_t s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
static bool s_initialized = false;

/* Reconnect task — applies backoff and calls esp_wifi_connect(). */
static TaskHandle_t s_reconnect_task = NULL;
#define RECONNECT_TASK_STACK 2048
#define RECONNECT_TASK_PRIO 4

/* Set to true by code that intentionally calls esp_wifi_disconnect() so the
 * resulting STA_DISCONNECTED event does not schedule a reconnect. */
static volatile bool s_intentional_disconnect = false;

/* Cached IP info from the last successful DHCP. On WiFi reconnect, we
 * re-apply this IP directly via tcpip_adapter_set_ip_info() instead of
 * waiting for DHCP (which is unreliable after reconnect on ESP8266 RTOS SDK
 * v3.4). DHCP lease is typically valid for hours, so reuse is safe.
 * See RFC 2131 §4.3.2 "INIT-REBOOT" state — DHCP clients MAY reuse a
 * previously assigned address. */
static bool s_have_cached_ip = false;
static tcpip_adapter_ip_info_t s_cached_ip_info;

/* Increase raw TX data rate from default 1 Mbps to 54 Mbps.
 * Without this, esp_wifi_80211_tx() uses 1 Mbps base rate -> at 200 pkt/s
 * (PCM 48kHz/5ms) ~50% packets are dropped (air time exceeds real time).
 * At 54 Mbps the same traffic fits in ~10% of air time.
 *
 * NOTE: function returns int (0=OK, non-zero=error), NOT esp_err_t.
 * Declared in NON-OS SDK user_interface.h, not in RTOS SDK v3.4 public
 * headers - must be extern-declared locally.
 *
 * IMPORTANT (raw TX mode): we use wifi_set_user_fixed_rate() here, NOT
 * wifi_set_user_sup_rate(). The latter sets the SUPPORTED RATES advertised
 * to an ASSOCIATED AP (STA mode) — it has no effect in raw 802.11 TX mode
 * where there is no association. wifi_set_user_fixed_rate() pins the actual
 * TX rate used by esp_wifi_80211_tx(), which is what we need for raw frame
 * injection. WIFI_RATE_11M = 0x03 corresponds to the 11 Mbps 802.11b CCK
 * rate. */
extern esp_err_t wifi_set_user_fixed_rate(uint8_t enable_mask, uint8_t rate);
#define FIXED_RATE_MASK_NONE 0x00
#define FIXED_RATE_MASK_STA 0x01
#define FIXED_RATE_MASK_AP 0x02
#define FIXED_RATE_MASK_ALL 0x03
#define WIFI_RATE_1M 0x00
#define WIFI_RATE_2M 0x01
#define WIFI_RATE_5_5M 0x02
#define WIFI_RATE_11M 0x03

/* ---- Forward declarations ---- */
static void wifi_reconnect_task(void *arg);

/* ====================================================================
 *  Event handler (UDP mode)
 *
 *  Follows the canonical pattern from ESP8266_RTOS_SDK and ESP-IDF
 *  examples/wifi/getting_started/station/main/station_example_main.c.
 *  esp_wifi_connect() on STA_START is called directly in the handler
 *  (runs in the event-loop task — safe, per Espressif's API contract).
 *  On STA_DISCONNECTED, we schedule a reconnect via an event-group bit
 *  (NOT esp_wifi_connect() directly) so a dedicated task can apply backoff
 *  without blocking the event loop.
 * ==================================================================== */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START - connecting...");
            /* Direct esp_wifi_connect() in the handler — same as the official
             * Espressif station example. Safe: runs in the event-loop task. */
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *evt = data;
            xEventGroupClearBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);

            /* CRITICAL FIX (per PDF 123.pdf recommendation): stop the audio
             * stream immediately on WiFi disconnect. While the stream is
             * running, I2S/PCM/TCP tasks consume CPU and hold sockets,
             * blocking the network stack. When WiFi reconnects, DHCP can't
             * run properly → no GOT_IP → ESP stuck in zombie state → no
             * broadcast announcements → server can't rediscover the device.
             *
             * Stopping the stream frees all resources. It will restart
             * automatically when the server sends CONFIGURE after reconnect. */
            if (streaming_is_active())
            {
                ESP_LOGW(TAG, "STA_DISCONNECTED reason %d - stopping stream + reconnect",
                         evt->reason);
                streaming_request_stop();
            }
            else
            {
                ESP_LOGW(TAG, "STA_DISCONNECTED reason %d - reconnect scheduled",
                         evt->reason);
            }

            /* Per Espressif's Wi-Fi Reconnect doc: if the disconnect was
             * caused by our own esp_wifi_disconnect() call, do NOT reconnect.
             * We track this via the s_intentional_disconnect flag (NOT via
             * reason codes — AUTH_LEAVE/ASSOC_LEAVE can come from the AP too,
             * see arduino-esp32#7210). */
            if (s_intentional_disconnect)
            {
                s_intentional_disconnect = false;
                ESP_LOGI(TAG, "  (intentional disconnect - no reconnect this cycle)");
                break;
            }

            /* Schedule a reconnect via the dedicated task (which applies
             * backoff). Every reason code gets the SAME backoff reconnect. */
            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_RECONNECT_REQ);
            break;
        }

        case WIFI_EVENT_STA_CONNECTED:
            /* Associated. If we have a cached IP, signal the reconnect task
             * to apply it (heavy work — tcpip_adapter_set_ip_info, broadcast
             * update — is done in the task, NOT here, to avoid stack overflow
             * in the event-loop task which has a small stack on ESP8266). */
            if (s_have_cached_ip)
            {
                ESP_LOGI(TAG, "STA_CONNECTED - cached IP available, signaling task");
                xEventGroupSetBits(s_wifi_evt, WIFI_EVT_APPLY_CACHED);
            }
            else
            {
                ESP_LOGI(TAG, "STA_CONNECTED - waiting for DHCP (first boot)");
            }
            /* Reset the svc_port DISCOVER watchdog (lightweight — just a
             * semaphore + tick read, safe for the event-loop task). */
            svc_port_reset_watchdog();
            break;

        case WIFI_EVENT_STA_AUTHMODE_CHANGE:
        {
            /* AP changed the authentication mode AFTER we associated.
             * Common legitimate causes:
             *   - Router firmware upgrade changed security policy (e.g.
             *     WPA2-PSK -> WPA3-SAE transition mode).
             *   - Mesh / WDS re-parenting to an AP with different auth.
             *   - Admin temporarily opened the network for setup.
             *
             * Hostile causes (downgrade attack):
             *   - Rogue AP forces a switch from WPA2 to OPEN/WEP to capture
             *     subsequent traffic in cleartext.
             *   - Evil-twin AP with weaker auth than the legitimate one.
             *
             * Policy:
             *   - Log every transition (INFO level for safe upgrades,
             *     WARN for downgrades).
             *   - For OPEN/WEP downgrades: the audio stream is now going out
             *     unencrypted on the radio -> stop it immediately and force
             *     a disconnect so we re-evaluate against the configured AP.
             *     The next STA_DISCONNECTED will go through the normal
             *     backoff reconnect path. We do NOT silently accept a
             *     cleartext downgrade.
             *   - For WPA-family transitions (WPA2->WPA3, WPA->WPA2, etc.):
             *     the link is still encrypted -> keep the stream running,
             *     just log the change. The CONFIGURE in NVS still says
             *     WPA2-PSK; if the AP no longer accepts it, the next
             *     disconnect will trigger reconnect anyway.
             *
             * Structural note: ESP8266 RTOS SDK v3.4 may fire this event
             * with NULL data on older SDKs - guard with a NULL check so we
             * don't dereference invalid memory. */
            wifi_event_sta_authmode_change_t *auth = data;
            if (!auth)
            {
                ESP_LOGW(TAG, "STA_AUTHMODE_CHANGE: event data NULL (old SDK?) - ignoring");
                break;
            }

            ESP_LOGI(TAG, "STA_AUTHMODE_CHANGE: %d -> %d",
                     (int)auth->old_mode, (int)auth->new_mode);

            /* Downgrade detection: OPEN (0) and WEP (1) are unencrypted.
             * Switching TO either of those from an encrypted mode is a
             * security-relevant downgrade - we refuse to stream cleartext. */
            bool old_encrypted = (auth->old_mode != WIFI_AUTH_OPEN &&
                                  auth->old_mode != WIFI_AUTH_WEP);
            bool new_encrypted = (auth->new_mode != WIFI_AUTH_OPEN &&
                                  auth->new_mode != WIFI_AUTH_WEP);

            if (old_encrypted && !new_encrypted)
            {
                ESP_LOGW(TAG, "  DOWNGRADE to unencrypted auth mode %d - "
                              "stopping stream + forcing reconnect",
                         (int)auth->new_mode);
                /* Stop the audio stream first - audio packets would otherwise
                 * go out unencrypted until the disconnect propagates. */
                if (streaming_is_active())
                    streaming_request_stop();
                /* Clear CONNECTED/GOT_IP so the rest of the system sees us
                 * as not-connected while we re-establish. */
                xEventGroupClearBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
                /* Mark as intentional so the resulting STA_DISCONNECTED
                 * does NOT trigger an immediate reconnect on top of ours
                 * (two concurrent esp_wifi_connect() calls corrupt the
                 * driver). The reconnect task's backoff will kick in on
                 * the next organic disconnect. */
                s_intentional_disconnect = true;
                esp_wifi_disconnect();
            }
            else if (auth->old_mode != auth->new_mode)
            {
                /* Encrypted->encrypted transition (e.g. WPA2->WPA3) or
                 * OPEN->OPEN-ish edge case. Log it but otherwise no-op:
                 * the link is still up, the IP is still valid, the stream
                 * can continue. If the new mode is incompatible with our
                 * configured credentials, the next beacon/probe will trigger
                 * a normal STA_DISCONNECTED and we'll backoff-reconnect. */
                ESP_LOGI(TAG, "  encrypted->encrypted transition - stream continues");
            }
            /* old == new is a no-op (some SDK versions fire the event
             * spuriously). Nothing to do. */
            break;
        }

        default:
            break;
        }
    }
    else if (base == IP_EVENT)
    {
        if (id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t *evt = data;
            ESP_LOGI(TAG, "GOT_IP: " IPSTR, IP2STR(&evt->ip_info.ip));

            /* Guard against duplicate GOT_IP: when we apply cached IP via
             * tcpip_adapter_set_ip_info() in the reconnect task, the SDK
             * fires IP_EVENT_STA_GOT_IP. But by that time, the reconnect
             * task has ALREADY set WIFI_EVT_GOT_IP and done svc_port_update_broadcast.
             *
             * BUT: if DHCP assigned a DIFFERENT IP than our cached one (router
             * changed its mind, lease pool rotated, etc.), we must NOT skip —
             * we need to update the cache, reinit the socket (bound to old IP),
             * and restart the stream (configured to old IP). Only skip if the
             * IP matches our cached value. */
            if ((xEventGroupGetBits(s_wifi_evt) & WIFI_EVT_GOT_IP) &&
                s_have_cached_ip &&
                evt->ip_info.ip.addr == s_cached_ip_info.ip.addr &&
                evt->ip_info.gw.addr == s_cached_ip_info.gw.addr &&
                evt->ip_info.netmask.addr == s_cached_ip_info.netmask.addr)
            {
                ESP_LOGI(TAG, "GOT_IP duplicate (same IP, already processed) - skipping");
                return;
            }

            /* Either first GOT_IP, or IP changed since cached — process fully. */
            if (s_have_cached_ip &&
                evt->ip_info.ip.addr != s_cached_ip_info.ip.addr)
            {
                ESP_LOGW(TAG, "IP changed: cached " IPSTR " → new " IPSTR " — restarting stream",
                         IP2STR(&s_cached_ip_info.ip),
                         IP2STR(&evt->ip_info.ip));
                /* IP changed — stop stream (configured to old IP), then
                 * let the server rediscover us with the new IP. */
                if (streaming_is_active())
                    streaming_request_stop();
                xEventGroupClearBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
            }

            /* Cache the IP info for fast reconnect next time. */
            s_cached_ip_info = evt->ip_info;
            s_have_cached_ip = true;

            /* FIX (H9): defer the heavy svc_port_reinit_socket() /
             * svc_port_update_broadcast() calls to the reconnect task.
             * Running them in the event-loop task (small stack, serializes
             * ALL events) blocks all WiFi/IP events while socket close +
             * reopen + broadcast setup runs. The reconnect task sets the
             * CONNECTED / GOT_IP bits itself after performing the work. */
            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_REINIT_SOCKET);

            /* Reset backoff to MIN — connection succeeded. */
            xSemaphoreTake(s_backoff_mtx, portMAX_DELAY);
            s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
            xSemaphoreGive(s_backoff_mtx);

            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
        }
        else if (id == IP_EVENT_STA_LOST_IP)
        {
            /* IP was lost (DHCP lease expired, or IP lost timer fired after
             * 120s without recovering). The SDK has already reset the IP to
             * 0.0.0.0. WiFi is STILL associated with the AP — only the IP
             * is gone.
             *
             * We must NOT call esp_wifi_connect() (it would fail with
             * "already connected"). Instead, re-apply the cached IP directly
             * via WIFI_EVT_APPLY_CACHED — same path as STA_CONNECTED. The
             * reconnect task does tcpip_adapter_set_ip_info(), which triggers
             * IP_EVENT_STA_GOT_IP → full recovery without WiFi disconnect.
             *
             * Without this handler, the code would think it's still connected
             * (CONNECTED/GOT_IP bits stay set) while all sends silently fail. */
            ESP_LOGW(TAG, "IP_EVENT_STA_LOST_IP - IP lost, re-applying cached IP");
            xEventGroupClearBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
            if (streaming_is_active())
                streaming_request_stop();
            /* Signal reconnect task to re-apply cached IP (NOT reconnect WiFi). */
            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_APPLY_CACHED);
        }
    }
}

/* ====================================================================
 *  Reconnect task
 *
 *  Owns esp_wifi_connect() calls that need backoff. The event handler only
 *  sets an event-group bit (non-blocking, thread-safe); this task blocks on
 *  xEventGroupWaitBits and applies exponential backoff before calling
 *  esp_wifi_connect(). Runs in its own task context — does NOT block the
 *  event loop, does NOT risk the #3458 timer-task deadlock.
 * ==================================================================== */
static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Reconnect task started");

    while (s_initialized)
    {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_evt,
            WIFI_EVT_RECONNECT_REQ | WIFI_EVT_APPLY_CACHED |
                WIFI_EVT_REINIT_SOCKET | WIFI_EVT_EXIT,
            pdTRUE,  /* clearOnExit */
            pdFALSE, /* wake on ANY */
            portMAX_DELAY);

        if (!s_initialized || (bits & WIFI_EVT_EXIT))
            break;

        /* FIX (H9): handle deferred socket reinit from IP_EVENT_STA_GOT_IP.
         * The event handler sets this bit instead of doing the work inline
         * (the work involves closing + reopening a socket, which can block
         * and would stall the event-loop task). */
        if (bits & WIFI_EVT_REINIT_SOCKET)
        {
            svc_port_update_broadcast();
            if (svc_port_is_running())
            {
                esp_err_t rerr = svc_port_reinit_socket();
                if (rerr != ESP_OK)
                    ESP_LOGW(TAG, "svc_port_reinit_socket: %s", esp_err_to_name(rerr));
            }
            /* FIX (WiFi reconnect): also reinit the TCP listening socket if
             * it exists. When WiFi disconnects, lwIP destroys the netif;
             * the old TCP listener becomes a zombie and never receives new
             * connections even after WiFi reconnects with the same IP.
             * Without this, the server cannot connect after a WiFi drop
             * until the device reboots. tcp_stream_reinit_listener() is a
             * no-op if TCP is not active (s_listen_sock < 0). */
            esp_err_t terr = tcp_stream_reinit_listener();
            if (terr != ESP_OK)
                ESP_LOGW(TAG, "tcp_stream_reinit_listener: %s", esp_err_to_name(terr));
            /* Don't continue - fall through in case RECONNECT_REQ is also
             * set (unlikely but possible). */
        }

        /* ---- Apply cached IP (signaled by STA_CONNECTED handler).
         * Done here (not in the event handler) because tcpip_adapter_set_ip_info
         * + svc_port_update_broadcast are heavy and overflow the event-loop
         * task's small stack on ESP8266. Our task has a 2048-byte stack. */
        if (bits & WIFI_EVT_APPLY_CACHED)
        {
            if (s_have_cached_ip)
            {
                ESP_LOGI(TAG, "Applying cached IP " IPSTR, IP2STR(&s_cached_ip_info.ip));
                tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
                esp_err_t ip_err = tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA,
                                                             &s_cached_ip_info);
                if (ip_err == ESP_OK)
                {
                    svc_port_update_broadcast();
                    xSemaphoreTake(s_backoff_mtx, portMAX_DELAY);
                    s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
                    xSemaphoreGive(s_backoff_mtx);
                    xEventGroupSetBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
                    ESP_LOGI(TAG, "Cached IP applied - GOT_IP signaled");
                }
                else
                {
                    ESP_LOGW(TAG, "set_ip_info failed: %s — falling back to DHCP",
                             esp_err_to_name(ip_err));
                }
            }
            /* Continue waiting for the next event — don't fall through to
             * the reconnect path (we're already connected). */
            continue;
        }

        /* ---- Reconnect request from STA_DISCONNECTED handler ---- */
        /* Exponential backoff: 1s → 2s → 4s → 8s → 15s (max).
         * Reset to 1s on GOT_IP. Retries INDEFINITELY — no reboot, no cap. */
        xSemaphoreTake(s_backoff_mtx, portMAX_DELAY);
        uint32_t delay_ms = s_backoff_ms;
        s_backoff_ms <<= 1;
        if (s_backoff_ms > WIFI_RECONNECT_BACKOFF_MAX_MS)
            s_backoff_ms = WIFI_RECONNECT_BACKOFF_MAX_MS;
        xSemaphoreGive(s_backoff_mtx);

        ESP_LOGW(TAG, "Reconnect in %u ms", (unsigned)delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        if (!s_initialized)
            break;

        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
            /* Re-schedule — backoff will continue climbing. */
            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_RECONNECT_REQ);
        }
    }

    ESP_LOGI(TAG, "Reconnect task exiting");
    s_reconnect_task = NULL;
    vTaskDelete(NULL);
}

/* ====================================================================
 *  Raw TX mode (unchanged)
 * ==================================================================== */

/* Raw TX configuration context - shared between wifi_sta_init_raw()
 * (calling task) and wifi_raw_event_handler() (event-loop task).
 *
 * channel:      set by caller BEFORE esp_wifi_start(); read by handler.
 * configured:   set true by handler AFTER all radio-config calls complete.
 * config_err:   captures the first fatal error from set_channel / set_protocol
 *               (set_rate failure is non-fatal and only logged). ESP_OK if
 *               everything succeeded.
 *
 * Synchronization: the writes to `configured` / `config_err` in the handler
 * happen-before the xEventGroupSetBits() call; the caller's read after
 * xEventGroupWaitBits() is therefore safe (FreeRTOS event-group ops use
 * critical sections, providing the necessary memory barrier). `volatile`
 * is kept for intent documentation. */
typedef struct
{
    uint8_t channel;
    volatile bool configured;
    volatile esp_err_t config_err;
} raw_tx_ctx_t;

static raw_tx_ctx_t s_raw_ctx = {0};

/* Raw 802.11 TX mode event handler.
 *
 * Listens ONLY for WIFI_EVENT_STA_START - the signal that esp_wifi_start()
 * has finished bringing the radio up. We do NOT call esp_wifi_connect()
 * here (raw TX has no AP).
 *
 * IMPORTANT: per the official esp_wifi_set_protocol() API doc:
 *   "@attention Please call this API in SYSTEM_EVENT_STA_START event"
 * So set_protocol (and, for symmetry and ordering safety, set_channel and
 * wifi_set_user_fixed_rate too) are called INSIDE this handler - not from
 * the calling task after the event. Doing all radio config here:
 *   1. satisfies the official API contract for set_protocol,
 *   2. guarantees channel/protocol/rate are applied atomically before the
 *      WIFI_EVT_STA_STARTED bit is signalled,
 *   3. eliminates the "initial drop" at stream start - the calling task
 *      can never reach esp_wifi_80211_tx() before the radio is up AND
 *      fully configured. */
static void wifi_raw_event_handler(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base != WIFI_EVENT || id != WIFI_EVENT_STA_START)
        return;

    ESP_LOGI(TAG, "WIFI_EVENT_STA_START - configuring radio (raw TX mode)");

    /* 1. Set the WiFi channel for raw TX (requires STA started). */
    esp_err_t err = esp_wifi_set_channel(s_raw_ctx.channel,
                                         WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_channel(%d) failed: %s",
                 s_raw_ctx.channel, esp_err_to_name(err));
        s_raw_ctx.config_err = err;
    }

    /* 2. Restrict protocol to 802.11b only.
     *    MUST be called inside the STA_START event per API docs. 11b-only
     *    gives the best raw-TX air-time vs. drop-rate tradeoff (tested
     *    ~1-2% drop at 200 pkt/s PCM 48kHz/5ms with 11M fixed rate). */
    err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_protocol failed: %s", esp_err_to_name(err));
        if (s_raw_ctx.config_err == ESP_OK)
            s_raw_ctx.config_err = err;
    }

    /* 3. Pin the TX rate to 11 Mbps. Without this, esp_wifi_80211_tx() uses
     *    the 1 Mbps base rate -> ~50% packet drop at 200 pkt/s (air time
     *    exceeds real time). At 11 Mbps the same traffic fits in ~10% air
     *    time. Non-fatal: if it fails the stream still works but with a
     *    higher drop rate for high-bitrate codecs.
     *
     * NOTE: wifi_set_user_fixed_rate() is the correct function for raw TX
     * mode — it pins the actual TX rate used by esp_wifi_80211_tx().
     * wifi_set_user_sup_rate() would be WRONG here: it sets the supported
     * rates advertised to an ASSOCIATED AP, which is irrelevant for raw
     * frame injection (no association exists). WIFI_RATE_11M = 0x03. */
    err = wifi_set_user_fixed_rate(FIXED_RATE_MASK_STA, WIFI_RATE_11M);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "wifi_set_user_fixed_rate failed: %s - "
                      "continuing with default rate",
                 esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Raw TX: protocol=11B, rate=11 Mbps (fixed)");
    }

    /* 4. Signal completion. The calling task waits on this bit; once set,
     *    the radio is up AND fully configured, so the first
     *    esp_wifi_80211_tx() will succeed. */
    s_raw_ctx.configured = true;
    if (s_wifi_evt)
        xEventGroupSetBits(s_wifi_evt, WIFI_EVT_STA_STARTED);
}

/* ====================================================================
 *  Common WiFi hardware init (shared by both modes)
 * ==================================================================== */
static esp_err_t wifi_hw_init(void)
{
    tcpip_adapter_init();

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
        return err;

    return ESP_OK;
}

/* ---- Common WiFi hardware start (shared by both modes) ---- */
static esp_err_t wifi_hw_start(uint8_t tx_power)
{
    /* Start WiFi. Event handlers and config must be set BEFORE this call. */
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* CRITICAL: Disable WiFi power save (Modem-sleep).
     *
     * ESP8266 enables Modem-sleep by default — the radio sleeps between
     * beacons to save power. During audio streaming (high traffic: 768 kbps
     * PCM), the radio can't keep up: beacons are missed, encryption keys
     * desync, and the AP kicks the station with reason 7 (NOT_ASSOCED) after
     * ~15 seconds. The symptom is "drop unencrypted frame" errors followed by
     * a disconnect.
     *
     * WIFI_PS_NONE keeps the radio fully awake at all times. This increases
     * power consumption (~70 mA vs ~20 mA idle) but is REQUIRED for reliable
     * streaming. Espressif FAQ recommends this for high-traffic applications.
     *
     * Must be called AFTER esp_wifi_start() (the WiFi driver must be running
     * for set_ps to take effect). */
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) failed: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "Power save disabled (WIFI_PS_NONE) — streaming-optimized");

    if (tx_power > WIFI_TX_POWER_MAX)
        tx_power = WIFI_TX_POWER_MAX;
    err = esp_wifi_set_max_tx_power(tx_power * 4);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_max_tx_power failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "TX power set to %u dBm", tx_power);

    return ESP_OK;
}

/* ====================================================================
 *  UDP mode: connect to AP
 * ==================================================================== */
esp_err_t wifi_sta_init(const char *ssid, const char *password,
                        const char *hostname, uint8_t tx_power)
{
    esp_err_t err = ESP_FAIL;

    if (s_initialized)
    {
        wifi_sta_deinit();
    }
    if (!ssid || !password)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hostname || !hostname[0])
    {
        hostname = WIFI_HOSTNAME_DEFAULT;
    }

    /* FIX (C5): create sync primitives; if a later step fails, jump to
     * fail_init which frees them and unregisters partially-registered
     * handlers (otherwise a retry would double-register handlers). */
    s_wifi_evt = xEventGroupCreate();
    s_backoff_mtx = xSemaphoreCreateMutex();
    if (!s_wifi_evt || !s_backoff_mtx)
    {
        ESP_LOGE(TAG, "Failed to create event group / mutex");
        goto fail_init_noinit;
    }
    s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
    s_intentional_disconnect = false;

    /* 1. Init WiFi hardware (esp_wifi_init, set_mode).
     * FIX (build): initialize err to ESP_FAIL so the fail_init_noinit path
     * (reached from the sync-primitive creation failure above, before
     * wifi_hw_init is called) has a defined value to return. Without this,
     * -Werror=maybe-uninitialized fires because GCC can't prove err is
     * always set before the goto path reads it. */

    err = wifi_hw_init();
    if (err != ESP_OK)
        goto fail_init_noinit;

    /* 2. Register event handlers BEFORE esp_wifi_start.
     *    WIFI_EVENT (all) + IP_EVENT_STA_GOT_IP + IP_EVENT_STA_LOST_IP. */
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK)
        goto fail_init;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK)
        goto fail_init;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK)
        goto fail_init;

    /* 3. Configure STA (requires esp_wifi_init and set_mode). */
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    wifi_cfg.sta.ssid[sizeof(wifi_cfg.sta.ssid) - 1] = '\0';
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.password[sizeof(wifi_cfg.sta.password) - 1] = '\0';
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);

    /* 4. Set hostname (DHCP/mDNS). */
    tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, hostname);
    ESP_LOGI(TAG, "Hostname set to '%s'", hostname);

    /* 5. Start the reconnect task BEFORE esp_wifi_start, so it's ready to
     *    handle the first disconnect if the AP is unreachable. */
    s_initialized = true;
    if (xTaskCreate(wifi_reconnect_task, "wifi_recon", RECONNECT_TASK_STACK,
                    NULL, RECONNECT_TASK_PRIO, &s_reconnect_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create reconnect task");
        s_reconnect_task = NULL;
        /* Non-fatal: WiFi still works, just no auto-reconnect. */
    }

    /* 6. Start WiFi hardware (generates WIFI_EVENT_STA_START -> esp_wifi_connect). */
    err = wifi_hw_start(tx_power);
    if (err != ESP_OK)
    {
        /* FIX (C5): Full cleanup on failure, mirroring wifi_sta_deinit. */
        s_initialized = false;
        if (s_wifi_evt)
            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_EXIT);
        if (s_reconnect_task)
        {
            /* FIX (FW#4): wait up to 3s for the reconnect task to exit on
             * its own before force-deleting. Force-delete inside
             * esp_wifi_connect orphans the WiFi driver mutex. */
            for (int i = 0; i < 30 && s_reconnect_task; i++)
                vTaskDelay(pdMS_TO_TICKS(100));
            if (s_reconnect_task)
            {
                ESP_LOGW(TAG, "wifi init fail: reconnect task did not exit, force-deleting");
                vTaskDelete(s_reconnect_task);
                s_reconnect_task = NULL;
            }
        }
        esp_wifi_stop();
        esp_wifi_deinit();
        goto fail_init;
    }

    /* FIX (L29): Zero the wifi_cfg struct (including plaintext password)
     * before returning so it doesn't linger on the stack. */
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));

    ESP_LOGI(TAG, "WiFi STA initialized (UDP mode), connecting to %s...", ssid);
    return ESP_OK;

fail_init:
    /* Handlers were registered - unregister them. */
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event_handler);
fail_init_noinit:
    /* Only sync primitives to free (handlers may not be registered yet). */
    if (s_backoff_mtx)
    {
        vSemaphoreDelete(s_backoff_mtx);
        s_backoff_mtx = NULL;
    }
    if (s_wifi_evt)
    {
        vEventGroupDelete(s_wifi_evt);
        s_wifi_evt = NULL;
    }
    return err != ESP_OK ? err : ESP_FAIL;
}

/* ====================================================================
 *  Raw 802.11 TX mode: no AP, just radio + channel
 * ==================================================================== */
esp_err_t wifi_sta_init_raw(uint8_t channel, uint8_t tx_power)
{
    if (s_initialized)
    {
        wifi_sta_deinit();
    }
    if (channel < 1 || channel > 13)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_wifi_evt = xEventGroupCreate();
    if (!s_wifi_evt)
    {
        return ESP_ERR_NO_MEM;
    }

    /* FIX (GROK-G11-2): create s_backoff_mtx here too. Previously this was
     * only created in wifi_sta_init() (line ~728), NOT in init_raw. If
     * wifi_sta_reconfigure() was called in raw TX mode (e.g. via AT+WIFI=...),
     * it would xSemaphoreTake(s_backoff_mtx, portMAX_DELAY) on a NULL handle
     * -> FreeRTOS dereferences NULL -> CRASH. Creating the mutex here (and
     * deinit() already destroys + NULLs it) makes reconfigure safe in both
     * STA and raw modes. */
    s_backoff_mtx = xSemaphoreCreateMutex();
    if (!s_backoff_mtx)
    {
        vEventGroupDelete(s_wifi_evt);
        s_wifi_evt = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Raw TX mode has no reconnect task (no AP to reconnect to). */
    s_intentional_disconnect = false;

    /* Set up the context shared with the STA_START handler BEFORE registering
     * the handler / starting WiFi. */
    s_raw_ctx.channel = channel;
    s_raw_ctx.configured = false;
    s_raw_ctx.config_err = ESP_OK;

    /* 1. Init WiFi hardware (esp_wifi_init, set_mode). */
    esp_err_t err = wifi_hw_init();
    if (err != ESP_OK)
        goto fail_raw_init;

    /* 2. Register the STA_START handler BEFORE esp_wifi_start(). */
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                     wifi_raw_event_handler, NULL);
    if (err != ESP_OK)
        goto fail_raw_init_after_hw;

    /* 3. Start WiFi hardware (queues the radio bring-up; STA_START fires soon). */
    err = wifi_hw_start(tx_power);
    if (err != ESP_OK)
        goto fail_raw_init_after_handler;

    /* 4. Wait for the STA_START handler to finish configuring the radio. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt,
                                           WIFI_EVT_STA_STARTED,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_RAW_START_TIMEOUT_MS));
    if (!(bits & WIFI_EVT_STA_STARTED))
    {
        ESP_LOGE(TAG, "Timeout (%d ms) waiting for WIFI_EVENT_STA_START "
                      "(raw TX) - radio did not come up",
                 WIFI_RAW_START_TIMEOUT_MS);
        err = ESP_ERR_TIMEOUT;
        goto fail_raw_init_after_start;
    }

    /* 5. Surface any fatal config error captured by the handler. */
    if (s_raw_ctx.config_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Raw TX radio configuration failed: %s",
                 esp_err_to_name(s_raw_ctx.config_err));
        err = s_raw_ctx.config_err;
        goto fail_raw_init_after_start;
    }

    /* 6. Radio is up and fully configured. Mark "connected" so the pipeline
     *    tasks don't wait for WiFi. */
    xEventGroupSetBits(s_wifi_evt, WIFI_EVT_CONNECTED);

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized (Raw 802.11 TX mode, channel %d, "
                  "protocol=11B, rate=11 Mbps)",
             channel);
    return ESP_OK;

    /* FIX (GROK-3.6): proper cleanup on all error paths. Previously each
     * error return leaked s_wifi_evt, s_backoff_mtx, registered handlers,
     * and/or a half-initialized WiFi stack. Now we unwind in reverse order
     * via labeled cleanup blocks. This mirrors the fail_init pattern used
     * by wifi_sta_init() (line ~810). */
fail_raw_init_after_start:
    esp_wifi_stop();
fail_raw_init_after_handler:
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START,
                                 wifi_raw_event_handler);
fail_raw_init_after_hw:
    esp_wifi_deinit();
fail_raw_init:
    if (s_backoff_mtx)
    {
        vSemaphoreDelete(s_backoff_mtx);
        s_backoff_mtx = NULL;
    }
    if (s_wifi_evt)
    {
        vEventGroupDelete(s_wifi_evt);
        s_wifi_evt = NULL;
    }
    return err;
}

/* ====================================================================
 *  Common API
 * ==================================================================== */

esp_err_t wifi_sta_deinit(void)
{
    if (!s_initialized)
        return ESP_OK;

    /* Unregister event handlers first, so no new events fire into a task
     * that's about to exit. */
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START,wifi_raw_event_handler);

    /* FIX (C3/M18): give any in-flight event handler a brief grace period to
     * finish before we tear down the event group / mutex it may touch.
     * esp_event_handler_unregister() does NOT guarantee the handler has
     * completed when it returns on ESP8266 RTOS SDK v3.4. 50 ms is enough
     * for the handler's own work to drain. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* FIX (C4/H8): Set the intentional_disconnect flag so the DISCONNECTED
     * event (fired by esp_wifi_disconnect below) does NOT schedule a
     * reconnect while we're tearing down. */
    s_intentional_disconnect = true;
    /* Disconnect first so the SDK posts STA_DISCONNECTED with our flag set,
     * then stop the radio. */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Signal the reconnect task to exit, then wait for it (real join via
     * the EXIT bit + notification pattern). */
    s_initialized = false;
    if (s_wifi_evt)
        xEventGroupSetBits(s_wifi_evt, WIFI_EVT_EXIT);

    if (s_reconnect_task)
    {
        /* FIX (C4 + FW#4): wait up to 5s for the reconnect task to exit on
         * its own (it may be inside esp_wifi_connect() which can block
         * several seconds). Only force-delete as a last resort.
         * Force-deleting a task inside esp_wifi_connect orphans the WiFi
         * driver mutex. This is the correct pattern - the other call sites
         * (svc_port, tcp_stream, wifi init-fail) have been updated to match. */
        for (int i = 0; i < 50 && s_reconnect_task; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_reconnect_task)
        {
            ESP_LOGW(TAG, "wifi_sta_deinit: reconnect task did not exit, force-deleting");
            vTaskDelete(s_reconnect_task);
            s_reconnect_task = NULL;
        }
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_wifi_evt)
    {
        vEventGroupDelete(s_wifi_evt);
        s_wifi_evt = NULL;
    }
    if (s_backoff_mtx)
    {
        vSemaphoreDelete(s_backoff_mtx);
        s_backoff_mtx = NULL;
    }
    s_intentional_disconnect = false;

    ESP_LOGI(TAG, "WiFi deinitialized");
    return ESP_OK;
}

esp_err_t wifi_sta_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_evt)
        return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt,
                                           WIFI_EVT_CONNECTED,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_EVT_CONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_sta_is_connected(void)
{
    if (!s_wifi_evt)
        return false;
    return (xEventGroupGetBits(s_wifi_evt) & WIFI_EVT_CONNECTED) != 0;
}

void wifi_sta_set_tx_power(uint8_t tx_power)
{
    if (tx_power > WIFI_TX_POWER_MAX)
        tx_power = WIFI_TX_POWER_MAX;
    /* FIX (H10): check the return code so the user is not told "TX power
     * set to N dBm" when the SDK rejected the value. */
    uint8_t raw = (tx_power > 63) ? 255 : (uint8_t)(tx_power * 4);
    esp_err_t err = esp_wifi_set_max_tx_power(raw);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "esp_wifi_set_max_tx_power(%u) failed: %s",
                 (unsigned)tx_power, esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "TX power set to %u dBm", tx_power);
}

esp_err_t wifi_sta_reconfigure(const char *ssid, const char *password)
{
    if (!s_initialized || !ssid || !password)
        return ESP_ERR_INVALID_STATE;

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    wifi_cfg.sta.ssid[sizeof(wifi_cfg.sta.ssid) - 1] = '\0';
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.password[sizeof(wifi_cfg.sta.password) - 1] = '\0';

    esp_err_t err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
    if (err != ESP_OK)
        return err;

    /* Reset backoff for the new credentials.
     * FIX (GROK-G11-2): NULL guard for defense-in-depth. In raw TX mode the
     * mutex is now created by init_raw (above), so this should never be NULL
     * when s_initialized is true. But if a future code path forgets to create
     * it, this guard prevents a crash (backoff reset is best-effort, not
     * critical for raw TX which has no reconnect task). */
    if (s_backoff_mtx)
    {
        xSemaphoreTake(s_backoff_mtx, portMAX_DELAY);
        s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
        xSemaphoreGive(s_backoff_mtx);
    }
    else
    {
        s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
    }

    /* FIX (C6/H8): Set the intentional_disconnect flag BEFORE calling
     * esp_wifi_connect(). If the STA is currently associated, esp_wifi_connect
     * will internally disconnect first -> STA_DISCONNECTED event fires. The
     * handler checks s_intentional_disconnect; if it's false (the old bug),
     * it schedules a reconnect via the task while this function is also
     * calling esp_wifi_connect -> two concurrent esp_wifi_connect calls ->
     * reconnect loop and driver confusion.
     *
     * The flag is cleared in the event handler when the DISCONNECTED event
     * arrives (so the subsequent esp_wifi_connect from this function is
     * not suppressed), and the handler will NOT schedule a task reconnect. */
    s_intentional_disconnect = true;

    /* esp_wifi_connect() here runs in the caller's task. If the STA is
     * currently associated, the SDK queues a STA_DISCONNECTED event that
     * is dispatched asynchronously from the event-loop task — it has NOT
     * been processed yet when esp_wifi_connect() returns. */
    err = esp_wifi_connect();

    /* FIX (GROK-2): RACE on s_intentional_disconnect.
     *
     * OLD CODE: unconditionally cleared the flag right after
     * esp_wifi_connect() returned. The asynchronous STA_DISCONNECTED
     * handler (in event-loop task) then ran LATER, saw the flag already
     * false, and scheduled a spurious reconnect via the backoff task —
     * racing with the connect initiated here, resetting backoff, and
     * (with new SSID just written) potentially connecting to the wrong AP.
     *
     * NEW CODE: only clear the flag if esp_wifi_connect() returned an
     * error indicating NO DISCONNECTED event will fire (i.e. the STA was
     * not associated and connect failed synchronously). When the SDK
     * fires a DISCONNECTED event, the handler clears the flag itself
     * (see wifi_event_handler, around line 199).
     *
     * As a belt-and-suspenders guard, we also wait briefly for the
     * handler to consume the flag — this covers the rare case where
     * esp_wifi_connect returns ESP_OK without firing DISCONNECTED (e.g.
     * already connected to the same AP). After 200 ms the flag is force-
     * cleared so the next organic disconnect isn't suppressed. */
    if (err != ESP_OK)
    {
        /* Synchronous failure: no DISCONNECTED event will fire, so the
         * handler will not clear the flag. Clear it here. */
        s_intentional_disconnect = false;
    }
    else
    {
        /* Wait up to 200 ms for the event handler to consume the flag
         * (it sets it to false on line ~199 of this file). If the flag
         * is still set after the wait, no DISCONNECTED event ever fired
         * (e.g. already associated to the same SSID) — clear it manually. */
        for (int i = 0; i < 20 && s_intentional_disconnect; i++)
            vTaskDelay(pdMS_TO_TICKS(10));
        s_intentional_disconnect = false;
    }

    /* FIX (L29): wipe the password from the stack. */
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));

    return err;
}
