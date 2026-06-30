/*
 * Stream Mode Implementations
 * ===========================
 *
 * Two ops tables are defined here: one for UDP mode, one for Raw 802.11 TX.
 * Each table groups ALL mode-specific behavior into a single place.
 *
 * The rest of the codebase (main.c, pipeline tasks) calls stream_mode_ops()
 * and is completely unaware of which mode is active.
 */

#include "stream_mode.h"
#include "wifi_sta.h"
#include "svc_port.h"
#include "udp_stream.h"
#include "board_config.h"

#include "esp_log.h"

static const char *TAG = "stream_mode";

/* ====================================================================
 * UDP Mode - connect to AP, send audio via UDP socket to a server.
 * Uses service port (EASSP) for discovery/control.
 * ==================================================================== */

static esp_err_t udp_wifi_init(const device_config_t *cfg)
{
    esp_err_t err = wifi_sta_init(cfg->wifi_ssid, cfg->wifi_password,
                                  cfg->hostname, cfg->tx_power);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UDP: WiFi STA init failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t udp_wifi_wait_ready(const device_config_t *cfg)
{
    if (!wifi_sta_is_connected())
    {
        ESP_LOGW(TAG, "UDP: WiFi not connected, waiting up to %d ms",
                 WIFI_CONNECT_TIMEOUT_MS);
        esp_err_t err = wifi_sta_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "UDP: WiFi connect timeout");
            return ESP_ERR_INVALID_STATE;
        }
    }
    ESP_LOGI(TAG, "UDP: WiFi connected");
    return ESP_OK;
}

static esp_err_t udp_get_stream_dest(uint32_t *host, uint16_t *port)
{
    if (!svc_port_get_stream_dest(host, port))
    {
        ESP_LOGE(TAG, "UDP: no CONFIGURE received - no stream destination");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void udp_set_channels(uint8_t channels)
{
    svc_port_set_channels(channels);
}

static esp_err_t udp_svc_port_init(EventGroupHandle_t evt_grp, uint16_t port)
{
    if (svc_port_is_running())
    {
        return ESP_OK;
    }
    return svc_port_init(port, evt_grp);
}

static esp_err_t udp_transport_init(uint32_t host, uint16_t port)
{
    return udp_stream_init(host, port);
}

static void udp_on_stream_started(void)
{
    svc_port_clear_error();
    svc_port_notify_streaming_started();
}

static void udp_on_stream_stopped(void)
{
    svc_port_notify_streaming_stopped();
    svc_port_notify_stop_complete();
}

static const stream_mode_ops_t s_udp_ops = {
    .name = "UDP",
    .wifi_init = udp_wifi_init,
    .wifi_wait_ready = udp_wifi_wait_ready,
    .get_stream_dest = udp_get_stream_dest,
    .set_channels = udp_set_channels,
    .svc_port_init = udp_svc_port_init,
    .transport_init = udp_transport_init,
    .on_stream_started = udp_on_stream_started,
    .on_stream_stopped = udp_on_stream_stopped,
    .needs_wifi_association = true,
    .auto_start = false,
};

/* ====================================================================
 * Raw 802.11 TX Mode - no AP, broadcast raw WiFi frames on a fixed channel.
 * Receiver must be in Monitor Mode. No service port.
 * ==================================================================== */

static esp_err_t rawtx_wifi_init(const device_config_t *cfg)
{
    return wifi_sta_init_raw(cfg->wifi_channel, cfg->tx_power);
}

static esp_err_t rawtx_wifi_wait_ready(const device_config_t *cfg)
{
    /* Radio readiness is already ensured inside rawtx_wifi_init() ->
     * wifi_sta_init_raw(): it blocks until the STA_START event handler has
     * finished running set_channel/set_protocol(11B)/set_fixed_rate(11M)
     * (set_protocol MUST be called inside the STA_START event per its API
     * doc). By the time we get here the radio is calibrated AND configured,
     * so there is nothing left to wait for. */
    (void)cfg;
    return ESP_OK;
}

static esp_err_t rawtx_get_stream_dest(uint32_t *host, uint16_t *port)
{
    /* Raw TX broadcasts to FF:FF:FF:FF:FF:FF - no destination to resolve. */
    return ESP_OK;
}

static void rawtx_set_channels(uint8_t channels)
{
    /* No service port in raw TX mode. */
}

static esp_err_t rawtx_svc_port_init(EventGroupHandle_t evt_grp, uint16_t port)
{
    /* No service port in raw TX mode. */
    return ESP_OK;
}

static esp_err_t rawtx_transport_init(uint32_t host, uint16_t port)
{
    /* host/port are unused - raw TX builds its own 802.11 header. */
    (void)host;
    (void)port;
    return udp_stream_init_raw();
}

static void rawtx_on_stream_started(void)
{
    /* No service port to notify. */
}

static void rawtx_on_stream_stopped(void)
{
    /* No service port to notify. */
}

static const stream_mode_ops_t s_rawtx_ops = {
    .name = "Raw 802.11 TX",
    .wifi_init = rawtx_wifi_init,
    .wifi_wait_ready = rawtx_wifi_wait_ready,
    .get_stream_dest = rawtx_get_stream_dest,
    .set_channels = rawtx_set_channels,
    .svc_port_init = rawtx_svc_port_init,
    .transport_init = rawtx_transport_init,
    .on_stream_started = rawtx_on_stream_started,
    .on_stream_stopped = rawtx_on_stream_stopped,
    .needs_wifi_association = false,
    .auto_start = true,
};

/* ====================================================================
 * Active mode selection
 * ==================================================================== */

static const stream_mode_ops_t *s_active_ops = &s_udp_ops;

void stream_mode_init(const device_config_t *cfg)
{
    s_active_ops = cfg->rawtx_mode ? &s_rawtx_ops : &s_udp_ops;
    ESP_LOGI(TAG, "Stream mode: %s", s_active_ops->name);
}

const stream_mode_ops_t *stream_mode_ops(void)
{
    return s_active_ops;
}

bool stream_mode_is_rawtx(void)
{
    return s_active_ops == &s_rawtx_ops;
}
