#ifndef UDP_STREAM_H
#define UDP_STREAM_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Stream sender - sends encoded audio packets to the receiver.
 *
 * Two modes:
 * 1. UDP mode (default): uses a standard UDP socket (lwIP).
 * 2. Raw 802.11 TX mode: uses esp_wifi_80211_tx to broadcast raw
 *    WiFi frames directly into the air (no router connection needed).
 *    The receiver must be in Monitor Mode to capture these frames.
 *
 * The mode is selected via udp_stream_init_raw() vs udp_stream_init().
 */

/* Standard UDP mode */
esp_err_t udp_stream_init(uint32_t host_ip, uint16_t host_port);

/* Raw 802.11 TX mode (broadcast on current WiFi channel) */
esp_err_t udp_stream_init_raw(void);

esp_err_t udp_stream_deinit(void);
bool      udp_stream_is_ready(void);
esp_err_t udp_stream_send(const uint8_t *data, size_t len);

#endif /* UDP_STREAM_H */
