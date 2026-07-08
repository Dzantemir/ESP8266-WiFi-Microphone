#ifndef WIFI_STA_H
#define WIFI_STA_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * WiFi STA mode with disconnect recovery.
 *
 * On disconnect, a dedicated background task (wifi_reconnect_task) performs
 * the reconnection — all esp_wifi_connect/disconnect/stop/start calls run in
 * that task's context, never from timer callbacks or event handlers (which
 * would deadlock the timer/WiFi tasks; see esp-idf issue #3458).
 *
 * Disconnect reasons are categorized:
 *   - temporary (BEACON_TIMEOUT, NO_AP_FOUND, ...): exponential backoff
 *     from WIFI_RECONNECT_BACKOFF_MIN_MS to WIFI_RECONNECT_BACKOFF_MAX_MS,
 *     reset to MIN on successful GOT_IP or SSID change.
 *   - auth failure (AUTH_FAIL, 4WAY_HANDSHAKE_TIMEOUT, ...): long delay.
 *   - connection failure (CONNECTION_FAIL, ASSOC_FAIL) or too many consecutive
 *     failures: full esp_wifi_stop()+start()+connect() to reset driver state.
 *   - intentional (ASSOC_LEAVE/AUTH_LEAVE): no reconnect (would loop).
 *
 * An IP-watchdog timer fires if DHCP doesn't deliver an IP within
 * IP_WATCHDOG_TIMEOUT_MS after STA_CONNECTED; its callback only sets an
 * event bit (non-blocking), and the reconnect task performs the recovery
 * (disconnect + lwIP DHCP reset + reconnect).
 */

esp_err_t wifi_sta_init(const char *ssid, const char *password,
                        const char *hostname, uint8_t tx_power);
esp_err_t wifi_sta_init_raw(uint8_t channel, uint8_t tx_power);
esp_err_t wifi_sta_deinit(void);
esp_err_t wifi_sta_reconfigure(const char *ssid, const char *password);
esp_err_t wifi_sta_wait_connected(uint32_t timeout_ms);
bool wifi_sta_is_connected(void);
void wifi_sta_set_tx_power(uint8_t tx_power);

#endif /* WIFI_STA_H */
