/*
 * WiFi STA mode with exponential backoff reconnection.
 *
 * Supports two modes:
 * 1. UDP mode: connects to AP, gets IP via DHCP.
 * 2. Raw 802.11 TX mode: starts radio without AP connection,
 *    sets a fixed channel, and marks WiFi as "ready" for esp_wifi_80211_tx.
 */

#include "wifi_sta.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tcpip_adapter.h"

#include "board_config.h"
#include "svc_port.h"

static const char *TAG = "wifi_sta";

#define WIFI_EVT_CONNECTED (1 << 0)
#define WIFI_EVT_GOT_IP (1 << 1)

static EventGroupHandle_t s_wifi_evt = NULL;
static SemaphoreHandle_t s_backoff_mtx = NULL;
static uint32_t s_backoff_ms = WIFI_RECONNECT_BACKOFF_MIN_MS;
static bool s_initialized = false;

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
 * In raw TX mode we don't register it (no connect/disconnect events needed). */
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
            static esp_timer_handle_t reconnect_timer = NULL;
            if (!reconnect_timer)
            {
                const esp_timer_create_args_t timer_args = {
                    .callback = (void *)esp_wifi_connect,
                    .name = "wifi_reconnect"
                };
                esp_timer_create(&timer_args, &reconnect_timer);
            }
            esp_timer_stop(reconnect_timer);
            esp_timer_start_once(reconnect_timer, (uint64_t)delay * 1000);
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

    /* 2. Register event handlers BEFORE esp_wifi_start. */
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (err != ESP_OK)
        return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
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

    /* 1. Init WiFi hardware (esp_wifi_init, set_mode). */
    esp_err_t err = wifi_hw_init();
    if (err != ESP_OK)
        return err;

    /* 2. Start WiFi hardware first (channel can only be set after start). */
    err = wifi_hw_start(tx_power);
    if (err != ESP_OK)
        return err;

    /* Disable power saving - required for low-latency TX (both UDP and
     * Raw TX). PS_MIN/MAX adds 100-300ms beacon-aligned wake delays. */
    // err = esp_wifi_set_ps(WIFI_PS_NONE);
    // if (err != ESP_OK)
    // {
    //     ESP_LOGE(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(err));
    //     return err;
    // }

    /* 4. Set the WiFi channel for raw TX (must be after esp_wifi_start). */
    err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_channel(%d) failed: %s", channel, esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_set_protocol failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wifi_set_user_fixed_rate(FIXED_RATE_MASK_STA, WIFI_RATE_11M);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "wifi_set_user_fixed_rate failed: %s", esp_err_to_name(err));
        /* Non-fatal: continue with default rate. Stream will work but with
         * high drop rate for high-bitrate codecs (PCM 48kHz). */
    }
    else
    {
        ESP_LOGI(TAG, "Raw TX rate set to 54 Mbps");
    }

    /* 6. Mark as "connected" so the pipeline tasks don't wait for WiFi. */
    xEventGroupSetBits(s_wifi_evt, WIFI_EVT_CONNECTED);

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized (Raw 802.11 TX mode, channel %d)", channel);
    return ESP_OK;
}

/* ---- Common API ---- */

esp_err_t wifi_sta_deinit(void)
{
    if (!s_initialized)
        return ESP_OK;

    /* Unregister event handlers (safe even if not registered). */
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);

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
