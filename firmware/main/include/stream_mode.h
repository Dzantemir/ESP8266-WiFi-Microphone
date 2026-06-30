#ifndef STREAM_MODE_H
#define STREAM_MODE_H

/*
 * Stream Mode Abstraction
 * =======================
 *
 * Encapsulates ALL differences between UDP and Raw 802.11 TX modes behind
 * a single operations table (vtable pattern - same approach used by Linux
 * kernel file_operations, ESP-IDF hal_ops, etc.).
 *
 * Why: the original code had `if (cfg.rawtx_mode)` branches scattered
 * across main.c (app_main, start_streaming, stop_streaming, cleanup_on_fail,
 * udp_task_fn). This made the mode logic fragmented and hard to maintain.
 *
 * With this abstraction, main.c is fully mode-agnostic: it calls ops->xxx()
 * and doesn't care whether we're on UDP or Raw TX. Adding a third mode
 * (e.g. ESP-NOW) would only require adding a new ops table - zero changes
 * to main.c.
 *
 * Lifecycle:
 *   boot:        stream_mode_init(&cfg)   -> selects active ops table
 *   app_main:    ops->wifi_init()         -> init WiFi for this mode
 *                ops->svc_port_init()     -> init service port (UDP only)
 *   start:       ops->get_stream_dest()   -> resolve where to send
 *                ops->wifi_wait_ready()   -> ensure WiFi is ready
 *                ops->transport_init()    -> init UDP socket or raw TX
 *                ops->on_stream_started() -> notify svc_port (UDP only)
 *   stop/fail:   ops->on_stream_stopped() -> notify svc_port (UDP only)
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* Include actual type definitions instead of forward declarations.
 * Forward decls don't work here because:
 *  - device_config_t in config_mgr.h is an anonymous struct (no tag name)
 *  - EventGroupHandle_t in FreeRTOS is `void *`, not `struct EventGroupDef_t *`
 * Including the real headers avoids type conflicts. */
#include "config_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

typedef struct {
    /* Human-readable mode name for logging. */
    const char *name;

    /* Boot-time WiFi initialization.
     * UDP:   esp_wifi_init + connect to AP (non-blocking, events drive it).
     * RAWTX: esp_wifi_init + esp_wifi_start + block until the STA_START event
     *        handler has run set_channel/set_protocol(11B)/set_fixed_rate(11M)
     *        (per esp_wifi_set_protocol() docs, set_protocol MUST be called
     *        inside the STA_START event - so ALL radio config happens in the
     *        handler, and the "radio ready" bit is only signalled afterwards).
     *        This guarantees the radio is calibrated AND configured when the
     *        pipeline starts, so the first esp_wifi_80211_tx() succeeds
     *        instead of being dropped.
     * Called once from app_main(). May be called again from start_streaming()
     * if boot init failed. */
    esp_err_t (*wifi_init)(const device_config_t *cfg);

    /* Ensure WiFi is ready for streaming (called in start_streaming).
     * UDP:   blocks until AP association completes (with timeout).
     * RAWTX: no-op - radio readiness is already ensured inside wifi_init()
     *        (it blocks on WIFI_EVENT_STA_START). Returns ESP_OK immediately.
     * Returns ESP_OK when ready, error otherwise. */
    esp_err_t (*wifi_wait_ready)(const device_config_t *cfg);

    /* Resolve the stream destination address.
     * UDP:   queries svc_port for the server's IP/port (from CONFIGURE).
     * RAWTX: no-op (broadcast - no destination needed).
     * host/port outputs are only valid when returning ESP_OK for UDP mode. */
    esp_err_t (*get_stream_dest)(uint32_t *host, uint16_t *port);

    /* Notify mode-specific subsystems about the channel count.
     * UDP:   svc_port_set_channels().
     * RAWTX: no-op.
     * Called at boot AND at stream start. */
    void (*set_channels)(uint8_t channels);

    /* Initialize the service port (if this mode uses one).
     * UDP:   svc_port_init() if not already running.
     * RAWTX: no-op.
     * Idempotent - safe to call multiple times. */
    esp_err_t (*svc_port_init)(EventGroupHandle_t evt_grp, uint16_t port);

    /* Initialize the data transport.
     * UDP:   udp_stream_init(host, port) - create UDP socket.
     * RAWTX: udp_stream_init_raw() - build 802.11 MAC header. */
    esp_err_t (*transport_init)(uint32_t host, uint16_t port);

    /* Called after streaming has started successfully.
     * UDP:   svc_port_clear_error() + svc_port_notify_streaming_started().
     * RAWTX: no-op. */
    void (*on_stream_started)(void);

    /* Called when streaming stops (normal stop, start failure, or cleanup).
     * UDP:   svc_port_notify_streaming_stopped() + notify_stop_complete().
     * RAWTX: no-op. */
    void (*on_stream_stopped)(void);

    /* Whether the UDP sender task must wait for WiFi AP association before
     * sending packets.
     * UDP:   true - can't send UDP without an IP address.
     * RAWTX: false - raw TX works without AP association. */
    bool needs_wifi_association;

    /* Whether to auto-start streaming at boot (without waiting for a
     * CONFIGURE command from a server).
     * UDP:   false - waits for server to send CONFIGURE.
     * RAWTX: true  - starts immediately (no server to configure it). */
    bool auto_start;
} stream_mode_ops_t;

/* Select the active mode based on config.
 * Must be called once at boot, after config_mgr_init(). */
void stream_mode_init(const device_config_t *cfg);

/* Get the active mode's operations table.
 * Always returns a valid pointer (never NULL after stream_mode_init). */
const stream_mode_ops_t *stream_mode_ops(void);

/* Convenience: returns true if the active mode is Raw 802.11 TX. */
bool stream_mode_is_rawtx(void);

#endif /* STREAM_MODE_H */
