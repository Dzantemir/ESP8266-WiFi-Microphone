#ifndef STREAM_MODE_H
#define STREAM_MODE_H

/*
 * Stream Mode Abstraction
 * =======================
 *
 * Encapsulates ALL differences between transport modes (UDP / TCP / Raw 802.11 TX)
 * behind a single operations table (vtable pattern - same approach used by Linux
 * kernel file_operations, ESP-IDF hal_ops, etc.).
 *
 * With this abstraction, main.c is fully mode-agnostic: it calls ops->xxx()
 * and doesn't care whether we're on UDP, TCP, or Raw TX. Adding a new transport
 * would only require adding a new ops table - zero changes to main.c.
 *
 * Each transport module (udp_stream.c, tcp_stream.c, rawtx_stream.c) is
 * independent — no shared state, no if-branches on transport type.
 *
 * Lifecycle:
 *   boot:        stream_mode_init(&cfg)     -> selects active ops table
 *   app_main:    ops->wifi_init()           -> init WiFi for this mode
 *                ops->svc_port_init()       -> init service port (UDP/TCP only)
 *   start:       ops->get_stream_dest()     -> resolve where to send
 *                ops->wifi_wait_ready()     -> ensure WiFi is ready
 *                ops->transport_init()      -> init transport (socket/listener/raw)
 *                ops->on_stream_started()   -> notify svc_port (UDP/TCP only)
 *   send loop:   ops->is_ready()            -> check transport readiness
 *                ops->send(data, len)       -> send one audio frame
 *   stop/fail:   ops->close_client()        -> close active conn (keep listener)
 *                ops->on_stream_stopped()   -> notify svc_port (UDP/TCP only)
 *   teardown:    ops->deinit()              -> full close (listener + client + task)
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

    /* ---- WiFi lifecycle ---- */

    /* Boot-time WiFi initialization.
     * UDP/TCP: esp_wifi_init + connect to AP (non-blocking, events drive it).
     * RAWTX:   esp_wifi_init + esp_wifi_start + block until the STA_START event
     *          handler has run set_channel/set_protocol(11B)/set_fixed_rate(11M).
     * Called once from app_main(). May be called again from start_streaming()
     * if boot init failed. */
    esp_err_t (*wifi_init)(const device_config_t *cfg);

    /* Ensure WiFi is ready for streaming (called in start_streaming).
     * UDP/TCP: blocks until AP association completes (with timeout).
     * RAWTX:   no-op - radio readiness is already ensured inside wifi_init().
     * Returns ESP_OK when ready, error otherwise. */
    esp_err_t (*wifi_wait_ready)(const device_config_t *cfg);

    /* ---- Service port / discovery ---- */

    /* Notify mode-specific subsystems about the channel count.
     * UDP/TCP: svc_port_set_channels().
     * RAWTX:   no-op.
     * Called at boot AND at stream start. */
    void (*set_channels)(uint8_t channels);

    /* Initialize the service port (if this mode uses one).
     * UDP/TCP: svc_port_init() if not already running.
     * RAWTX:   no-op.
     * Idempotent - safe to call multiple times. */
    esp_err_t (*svc_port_init)(EventGroupHandle_t evt_grp, uint16_t port);

    /* Resolve the stream destination address.
     * UDP/TCP: queries svc_port for the server's IP/port (from CONFIGURE).
     * RAWTX:   no-op (broadcast - no destination needed).
     * host/port outputs are only valid when returning ESP_OK for UDP/TCP mode. */
    esp_err_t (*get_stream_dest)(uint32_t *host, uint16_t *port);

    /* ---- Transport lifecycle ---- */

    /* Initialize the data transport.
     * UDP:   udp_stream_init(host, port) - create UDP socket.
     * TCP:   tcp_stream_init_listen(port) - open listening socket + accept task.
     * RAWTX: rawtx_stream_init() - build 802.11 MAC header. */
    esp_err_t (*transport_init)(uint32_t host, uint16_t port);

    /* Check if the transport is ready to send (has an active connection).
     * UDP:   socket is open.
     * TCP:   a client is connected.
     * RAWTX: always true after init (broadcast, no connection). */
    bool (*is_ready)(void);

    /* Send one audio frame.
     * UDP:   sendto() as a single datagram.
     * TCP:   blocking send() with length-prefix framing.
     * RAWTX: esp_wifi_80211_tx() with 802.11 MAC header. */
    esp_err_t (*send)(const uint8_t *data, size_t len);

    /* Close the active client connection but KEEP the listening socket alive
     * (TCP). For UDP/RAWTX: same as deinit (no persistent listener).
     * Used by stop_streaming() to allow quick restart without EADDRINUSE. */
    void (*close_client)(void);

    /* Full teardown: close listener + client + stop accept task (TCP).
     * For UDP/RAWTX: close socket / free state.
     * Used by cleanup_on_fail() and transport mode switch. */
    esp_err_t (*deinit)(void);

    /* ---- Stream lifecycle notifications ---- */

    /* Called after streaming has started successfully.
     * UDP/TCP: svc_port_clear_error() + svc_port_notify_streaming_started().
     * RAWTX:   no-op. */
    void (*on_stream_started)(void);

    /* Called when streaming stops (normal stop, start failure, or cleanup).
     * UDP/TCP: svc_port_notify_streaming_stopped() + notify_stop_complete().
     * RAWTX:   no-op. */
    void (*on_stream_stopped)(void);

    /* ---- Mode flags ---- */

    /* Whether the sender task must wait for WiFi AP association before
     * sending packets.
     * UDP/TCP: true - can't send without an IP address.
     * RAWTX:   false - raw TX works without AP association. */
    bool needs_wifi_association;

    /* Whether to auto-start streaming at boot (without waiting for a
     * CONFIGURE command from a server).
     * UDP/TCP: false - waits for server to send CONFIGURE.
     * RAWTX:   true  - starts immediately (no server to configure it). */
    bool auto_start;

    /* Whether this mode uses the EASSP service port for discovery/control.
     * UDP/TCP: true  - svc_port_init() starts the EASSP listener.
     * RAWTX:   false - no service port (broadcast, no server to discover us). */
    bool uses_svc_port;
} stream_mode_ops_t;

/* Select the active mode based on config.transport_mode.
 * Must be called once at boot, after config_mgr_init(). */
void stream_mode_init(const device_config_t *cfg);

/* Get the active mode's operations table.
 * Always returns a valid pointer (never NULL after stream_mode_init). */
const stream_mode_ops_t *stream_mode_ops(void);

/* FIX (split): transport-type query for transport-specific timeouts.
 * Returns the current transport mode (0=UDP, 1=TCP, 2=RawTX). Used by
 * main.c stop_streaming() to pick STREAM_STOP_TIMEOUT_UDP_MS vs
 * STREAM_STOP_TIMEOUT_TCP_MS (RawTX has no send timeout, uses UDP value
 * as a safe default). */
uint8_t stream_mode_current_transport(void);

/* ---- Transport-agnostic wrappers (thin proxies to ops table) ----
 * main.c calls these. No if-branches on transport type — pure vtable dispatch. */
bool      transport_is_ready(void);
esp_err_t transport_send(const uint8_t *data, size_t len);
esp_err_t transport_deinit(void);
void      transport_close_client(void);

#endif /* STREAM_MODE_H */
