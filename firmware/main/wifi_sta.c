/*
 * WiFi STA mode with exponential backoff reconnection.
 *
 * Supports two modes:
 * 1. UDP mode: connects to AP, gets IP via DHCP.
 * 2. Raw 802.11 TX mode: starts radio without AP connection,
 *    sets a fixed channel, and marks WiFi as "ready" for esp_wifi_80211_tx.
 */


#include <string.h>
#include "freertos/FreeRTOS.h"
#include "board_config.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tcpip_adapter.h"
#include "wifi_sta.h"


#include "svc_port.h"

static const char *TAG = "wifi_sta";

#define WIFI_EVT_CONNECTED (1 << 0)
#define WIFI_EVT_GOT_IP (1 << 1)
#define WIFI_EVT_STA_STARTED (1 << 2) /* raw TX: WIFI_EVENT_STA_START fired -> radio up */

static EventGroupHandle_t s_wifi_evt = NULL;
static SemaphoreHandle_t s_backoff_mtx = NULL;
static uint32_t s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
static bool s_initialized = false;

/* Reconnect timer — scheduled by wifi_event_handler on STA_DISCONNECTED to
 * call esp_wifi_connect() after a backoff delay. File-scope (not
 * function-static) so wifi_sta_deinit() can stop it before esp_wifi_stop(),
 * preventing the callback from firing on a shut-down radio. */
static esp_timer_handle_t s_reconnect_timer = NULL;

/* Increase raw TX data rate from default 1 Mbps to 54 Mbps.
 * Without this, esp_wifi_80211_tx() uses 1 Mbps base rate -> at 200 pkt/s
 * (PCM 48kHz/5ms) ~50% packets are dropped (air time exceeds real time).
 * At 54 Mbps the same traffic fits in ~10% of air time.
 *
 * NOTE: function returns int (0=OK, non-zero=error), NOT esp_err_t.
 * Declared in NON-OS SDK user_interface.h, not in RTOS SDK v3.4 public
 * headers - must be extern-declared locally. */
extern esp_err_t wifi_set_user_fixed_rate(uint8_t enable_mask, uint8_t rate);
#define FIXED_RATE_MASK_NONE 0x00 // Фиксация отключена (автоматический выбор скорости)
#define FIXED_RATE_MASK_STA 0x01  // Фиксированная скорость для Station (клиент)
#define FIXED_RATE_MASK_AP 0x02   // Фиксированная скорость для SoftAP (точка доступа)
#define FIXED_RATE_MASK_ALL 0x03  // Фиксированная скорость для всех режимов (STA + AP)
#define WIFI_RATE_1M 0x00         // 1 Mbps
#define WIFI_RATE_2M 0x01         // 2 Mbps
#define WIFI_RATE_5_5M 0x02       // 5.5 Mbps
#define WIFI_RATE_11M 0x03        // 11 Mbps
#define WIFI_RATE_6M 0x0b         // 6 Mbps
#define WIFI_RATE_9M 0x0f         // 9 Mbps
#define WIFI_RATE_12M 0x0a        // 12 Mbps
#define WIFI_RATE_18M 0x0e        // 18 Mbps
#define WIFI_RATE_24M 0x09        // 24 Mbps
#define WIFI_RATE_36M 0x0d        // 36 Mbps
#define WIFI_RATE_48M 0x08        // 48 Mbps
#define WIFI_RATE_54M 0x0c        // 54 Mbps
#define WIFI_RATE_MCS0 0x10       // MCS0 (6.5 Mbps при 20 МГц)
#define WIFI_RATE_MCS1 0x11       // MCS1 (13 Mbps)
#define WIFI_RATE_MCS2 0x12       // MCS2 (19.5 Mbps)
#define WIFI_RATE_MCS3 0x13       // MCS3 (26 Mbps)
#define WIFI_RATE_MCS4 0x14       // MCS4 (39 Mbps)
#define WIFI_RATE_MCS5 0x15       // MCS5 (52 Mbps)
#define WIFI_RATE_MCS6 0x16       // MCS6 (58.5 Mbps)
#define WIFI_RATE_MCS7 0x17       // MCS7 (65 Mbps при 20 МГц)

/* Event handler - only active in UDP mode.
 * In raw TX mode a separate, smaller handler (wifi_raw_event_handler) is
 * used: it only needs WIFI_EVENT_STA_START as a radio-ready signal and must
 * NOT call esp_wifi_connect() (raw TX has no AP). */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WIFI_EVENT_STA_START - connecting...");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *evt = data;
            xEventGroupClearBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
            xSemaphoreTake(s_backoff_mtx, portMAX_DELAY);
            uint32_t delay = s_backoff_ms;
            s_backoff_ms <<= 1;
            if (s_backoff_ms > WIFI_RECONNECT_BACKOFF_MAX_MS)
            {
                s_backoff_ms = WIFI_RECONNECT_BACKOFF_MAX_MS;
            }
            xSemaphoreGive(s_backoff_mtx);

            ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED - reason: %d, reconnecting in %u ms...",
                     evt->reason, (unsigned)delay);
            /* FIX B3: Use a timer instead of vTaskDelay to avoid blocking the
             * event loop task. The default event task processes ALL events
             * (WiFi, IP, etc.) - blocking it for up to 30s starves everything. */
            if (!s_reconnect_timer)
            {
                const esp_timer_create_args_t timer_args = {
                    .callback = (void *)esp_wifi_connect,
                    .name = "wifi_reconnect"};
                esp_timer_create(&timer_args, &s_reconnect_timer);
            }
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000);
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
            ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP: " IPSTR, IP2STR(&evt->ip_info.ip));
            svc_port_update_broadcast();

            xSemaphoreTake(s_backoff_mtx, portMAX_DELAY);
            s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
            xSemaphoreGive(s_backoff_mtx);

            xEventGroupSetBits(s_wifi_evt, WIFI_EVT_CONNECTED | WIFI_EVT_GOT_IP);
        }
    }
}

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
     *    higher drop rate for high-bitrate codecs. */
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

/* ---- Common WiFi hardware init (shared by both modes) ---- */
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

/* ---- UDP mode: connect to AP ---- */
esp_err_t wifi_sta_init(const char *ssid, const char *password,
                        const char *hostname, uint8_t tx_power)
{
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

    s_wifi_evt = xEventGroupCreate();
    s_backoff_mtx = xSemaphoreCreateMutex();
    if (!s_wifi_evt || !s_backoff_mtx)
    {
        return ESP_ERR_NO_MEM;
    }
    s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;

    /* 1. Init WiFi hardware (esp_wifi_init, set_mode). */
    esp_err_t err = wifi_hw_init();
    if (err != ESP_OK)
        return err;

    /* 2. Register event handlers BEFORE esp_wifi_start.
     *
     * Register the specific events we actually handle rather than
     * ESP_EVENT_ANY_ID: the switch in wifi_event_handler only acts on
     * STA_START / STA_DISCONNECTED, so listening on ANY_ID would needlessly
     * wake the event-loop task for every WIFI_EVENT (SCAN_DONE, etc.).
     * Symmetric with the raw TX mode, which also registers STA_START
     * specifically.
     *
     * CAVEAT: esp_event_handler_unregister(base, ESP_EVENT_ANY_ID, h) does
     * NOT remove handlers registered for specific ids - it only clears the
     * base's "any-id" list. So the deinit path MUST unregister the exact
     * same three (base,id) pairs registered here. */
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK)
        return err;
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK)
        return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK)
        return err;

    /* 3. Configure STA (requires esp_wifi_init and set_mode). */
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);

    /* 4. Set hostname (DHCP/mDNS). */
    tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, hostname);
    ESP_LOGI(TAG, "Hostname set to '%s'", hostname);

    /* 5. Start WiFi hardware (generates WIFI_EVENT_STA_START). */
    err = wifi_hw_start(tx_power);
    if (err != ESP_OK)
        return err;

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi STA initialized (UDP mode), connecting to %s...", ssid);
    return ESP_OK;
}

/* ---- Raw 802.11 TX mode: no AP, just radio + channel ---- */
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

    /* Set up the context shared with the STA_START handler BEFORE registering
     * the handler / starting WiFi. The handler reads s_raw_ctx.channel and
     * writes s_raw_ctx.configured / .config_err. */
    s_raw_ctx.channel = channel;
    s_raw_ctx.configured = false;
    s_raw_ctx.config_err = ESP_OK;

    /* 1. Init WiFi hardware (esp_wifi_init, set_mode). */
    esp_err_t err = wifi_hw_init();
    if (err != ESP_OK)
        return err;

    /* 2. Register the STA_START handler BEFORE esp_wifi_start().
     *    esp_wifi_start() is asynchronous: it queues the start request and
     *    returns immediately - the radio comes up shortly afterwards in the
     *    WiFi driver task, which then posts WIFI_EVENT_STA_START. Registering
     *    before start() guarantees we don't race and miss the event.
     *
     *    The handler performs ALL radio configuration (set_channel,
     *    set_protocol, set_rate) - per the esp_wifi_set_protocol() API doc
     *    ("@attention Please call this API in SYSTEM_EVENT_STA_START event")
     *    the protocol call MUST happen inside the STA_START event. */
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                     wifi_raw_event_handler, NULL);
    if (err != ESP_OK)
        return err;

    /* 3. Start WiFi hardware (queues the radio bring-up; STA_START fires soon). */
    err = wifi_hw_start(tx_power);
    if (err != ESP_OK)
        return err;

    /* 4. Wait for the STA_START handler to finish configuring the radio.
     *    The handler sets WIFI_EVT_STA_STARTED only AFTER set_channel,
     *    set_protocol and set_rate have all run, so by the time this bit is
     *    set the radio is up AND fully configured. This is the critical fix
     *    for the "initial drop" at stream start: esp_wifi_start() returns
     *    before the radio is actually up, so without this wait the first
     *    few esp_wifi_80211_tx() calls would hit an uncalibrated radio and
     *    be dropped.
     *
     *    Typical bring-up time on ESP8266 is ~100-300 ms; the timeout is a
     *    safety net for a wedged radio, not the expected wait. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt,
                                           WIFI_EVT_STA_STARTED,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(WIFI_RAW_START_TIMEOUT_MS));
    if (!(bits & WIFI_EVT_STA_STARTED))
    {
        ESP_LOGE(TAG, "Timeout (%d ms) waiting for WIFI_EVENT_STA_START "
                      "(raw TX) - radio did not come up",
                 WIFI_RAW_START_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* 5. Surface any fatal config error captured by the handler
     *    (set_channel or set_protocol failure; set_rate is non-fatal and
     *    only logged inside the handler). */
    if (s_raw_ctx.config_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Raw TX radio configuration failed: %s",
                 esp_err_to_name(s_raw_ctx.config_err));
        return s_raw_ctx.config_err;
    }

    /* 6. Radio is up and fully configured. Mark "connected" so the pipeline
     *    tasks don't wait for WiFi. */
    xEventGroupSetBits(s_wifi_evt, WIFI_EVT_CONNECTED);

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized (Raw 802.11 TX mode, channel %d, "
                  "protocol=11B, rate=11 Mbps)",
             channel);
    return ESP_OK;
}

/* ---- Common API ---- */

esp_err_t wifi_sta_deinit(void)
{
    if (!s_initialized)
        return ESP_OK;

    /* Unregister event handlers. Each registration in wifi_sta_init() /
     * wifi_sta_init_raw() has a matching unregister here. unregister() is
     * safe even if the handler was never registered (no-op in that case).
     *
     * Note: we unregister by the EXACT (base,id) pair -
     * esp_event_handler_unregister(base, ESP_EVENT_ANY_ID, h) would NOT
     * remove handlers registered for specific ids (it only clears the
     * base's any-id list), so the pairs below must mirror the register()
     * calls exactly. */
    /* UDP-mode handlers (registered by wifi_sta_init). */
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START,
                                 wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                 wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                 wifi_event_handler);
    /* Raw-TX-mode handler (registered by wifi_sta_init_raw). */
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START,
                                 wifi_raw_event_handler);

    /* Stop any pending reconnect timer before shutting down the radio.
     * Without this, a STA_DISCONNECTED that arrived just before deinit
     * would schedule esp_wifi_connect() after esp_wifi_stop()/deinit(),
     * calling into a shut-down driver (esp_wifi_connect on a stopped
     * radio returns an error or crashes depending on SDK state). */
    if (s_reconnect_timer)
    {
        esp_timer_stop(s_reconnect_timer);
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

    s_initialized = false;
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
    esp_wifi_set_max_tx_power(tx_power * 4);
    ESP_LOGI(TAG, "TX power set to %u dBm", tx_power);
}

esp_err_t wifi_sta_reconfigure(const char *ssid, const char *password)
{
    if (!s_initialized || !ssid || !password)
        return ESP_ERR_INVALID_STATE;

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);

    esp_err_t err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
    if (err != ESP_OK)
        return err;

    xSemaphoreTake(s_backoff_mtx, portMAX_DELAY);
    s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
    xSemaphoreGive(s_backoff_mtx);

    return esp_wifi_connect();
}
