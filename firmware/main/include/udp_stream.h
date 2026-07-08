#ifndef UDP_STREAM_H
#define UDP_STREAM_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * UDP transport — sends encoded audio packets to a receiver via a standard
 * UDP socket (lwIP). Requires WiFi AP association (router).
 *
 * This module is independent of tcp_stream.c and rawtx_stream.c.
 * Each audio frame = one UDP datagram (boundaries preserved by UDP).
 */

/* Open a UDP socket and set up the destination address.
 * FIX (L16): host_ip MUST be in NETWORK byte order (e.g. ip_addr_t.addr).
 * Passing a host-order IP would silently send to the wrong address. */
esp_err_t udp_stream_init(uint32_t host_ip, uint16_t host_port);

/* Close the UDP socket. */
esp_err_t udp_stream_deinit(void);

/* true if the UDP socket is open and ready for send. */
bool udp_stream_is_ready(void);

/* Send an audio frame as a single UDP datagram.
 * data = [pkt_header 16B][payload], len = 16 + payload. */
esp_err_t udp_stream_send(const uint8_t *data, size_t len);

#endif /* UDP_STREAM_H */
