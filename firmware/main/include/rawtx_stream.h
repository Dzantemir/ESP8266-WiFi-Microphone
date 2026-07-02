#ifndef RAWTX_STREAM_H
#define RAWTX_STREAM_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Raw 802.11 TX transport — broadcasts raw WiFi data frames directly into
 * the air on the current channel (no router/AP association needed).
 * The receiver must be in Monitor Mode on the same channel.
 *
 * This module is independent of udp_stream.c and tcp_stream.c. It builds
 * its own 802.11 MAC header and sends via esp_wifi_80211_tx().
 *
 * Frame format on air:
 *   [802.11 MAC header 24B][audio payload]
 *   The audio payload is the same [16-byte pkt_header][codec data] as in
 *   UDP/TCP — no length-prefix (broadcast, no framing needed).
 */

/* Initialize Raw TX mode: build 802.11 MAC header, prepare for send.
 * No socket, no bind — uses esp_wifi_80211_tx() directly.
 * WiFi must be started (wifi_sta_init_raw) before calling this. */
esp_err_t rawtx_stream_init(void);

/* Tear down Raw TX state (frees nothing — header is static). */
esp_err_t rawtx_stream_deinit(void);

/* Always true after rawtx_stream_init() (no connection concept). */
bool rawtx_stream_is_ready(void);

/* Send an audio frame as a raw 802.11 data frame.
 * data = [pkt_header 16B][payload], len = 16 + payload (≤ 1400).
 * Prepends 24-byte MAC header with auto-incrementing sequence number. */
esp_err_t rawtx_stream_send(const uint8_t *data, size_t len);

#endif /* RAWTX_STREAM_H */
