#ifndef WIFI_STA_H
#define WIFI_STA_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * WiFi STA mode with exponential backoff reconnection.
 *
 * On disconnect, retries with doubling delay from
 * WIFI_RECONNECT_BACKOFF_MIN_MS to WIFI_RECONNECT_BACKOFF_MAX_MS.
 * Reset to MIN on successful connection or SSID change.
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
