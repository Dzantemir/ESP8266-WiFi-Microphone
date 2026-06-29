#ifndef SVC_PORT_H
#define SVC_PORT_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Service UDP port for device discovery and streaming control.
 *
 * Listens on UDP:3950 for EASSP protocol commands.
 *
 * Commands handled:
 *   DISCOVER  (idle)      -> respond with INFO
 *   DISCOVER  (streaming) -> reset watchdog timer (heartbeat)
 *   CONFIGURE             -> store stream destination, start streaming,
 *                            respond with INFO
 *
 * Outgoing:
 *   INFO  -> response to DISCOVER (idle) and CONFIGURE
 *   INFO  -> periodic ANNOUNCE broadcast when idle (every 1-5s random)
 *   INFO  -> periodic status during streaming (~1/sec to server)
 *
 * Watchdog:
 *   While streaming, server must send DISCOVER periodically as heartbeat.
 *   If no DISCOVER within SVC_WATCHDOG_TIMEOUT_MS (15s), ESP assumes
 *   server is dead and auto-stops streaming.
 */

/* Stream control event bits - used with FreeRTOS EventGroup */
#define STREAM_EVT_START_REQ  (1 << 0)
#define STREAM_EVT_STOP_REQ   (1 << 1)
#define STREAM_EVT_ACTIVE     (1 << 2)

/* Initialize the service port. Must be called after WiFi is connected. */
esp_err_t svc_port_init(uint16_t port, void *stream_evt_grp);

/* Check if the service port task is running. */
bool svc_port_is_running(void);

/* Notify that streaming has started (activates watchdog). */
void svc_port_notify_streaming_started(void);

/* Notify that streaming has stopped (deactivates watchdog). */
void svc_port_notify_streaming_stopped(void);

/* Notify that stop_streaming() has completed. */
void svc_port_notify_stop_complete(void);

/* Update packet statistics (called by UDP stream task). */
void svc_port_update_stats(uint32_t packets_sent);

/* Get the last configured stream destination.
 * Returns true if a valid CONFIGURE has been received. */
bool svc_port_get_stream_dest(uint32_t *host, uint16_t *port);

/* Set the channel count reported in INFO packets.
 * Called by main.c start_streaming() after resolving runtime channel
 * count from NVS config. */
void svc_port_set_channels(uint8_t channels);

/* Refresh broadcast address after IP change. */
void svc_port_update_broadcast(void);

/* Set error code. The next INFO packet will carry this error. */
void svc_port_set_error(uint8_t error_code);

/* Clear error code. */
void svc_port_clear_error(void);

/* Status snapshot for AT+STATUS command. */
typedef struct {
    bool     running;
    bool     streaming;
    uint8_t  error_code;
    uint8_t  mac[6];
    uint32_t server_stream_ip;
    uint16_t server_stream_port;
    uint32_t server_svc_ip;
    uint16_t server_svc_port;
    uint32_t packets_sent;
    int32_t  watchdog_remaining_ms;
} svc_port_status_t;

void svc_port_get_status(svc_port_status_t *status);

#endif /* SVC_PORT_H */
