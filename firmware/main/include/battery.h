#ifndef BATTERY_H
#define BATTERY_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Battery monitoring module - ported from ESP8285-WEBSERVER project.
 *
 * ESP8266 has a single ADC (TOUT pin, 0-1V, 10-bit). A voltage divider
 * on the battery side scales V_batt into this range. The module:
 *   - Initializes ADC (call battery_init() once at startup)
 *   - Reads V_batt via ADC averaging (battery_get_voltage_mv())
 *   - Runs a background task that checks voltage every BATT_CHECK_MIN
 *     minutes and enters deep sleep if V_batt < BATT_CRITICAL_MV
 *
 * When BATTERY_ENABLED=0 (CONFIG_STREAMER_BATTERY_ENABLED not set), all
 * functions become no-ops / return 0, so all functions are available (stubs when disabled).
 *
 * THREAD SAFETY:
 *   - battery_get_voltage_mv() is NOT thread-safe with concurrent ADC
 *     access. Only the battery_monitor_task should call it, OR call it
 *     from a single thread with the monitor task stopped.
 *   - battery_get_last_mv() IS thread-safe (reads a cached atomic value).
 */

/* Initialize ADC hardware. Safe to call even when battery is disabled
 * (becomes no-op). Call once in app_main before starting monitor task. */
esp_err_t battery_init(void);

/* Read current battery voltage in millivolts. Averages BATT_ADC_SAMPLES
 * readings with BATT_ADC_DELAY_MS between them. Takes ~750ms by default.
 * Returns 0 if ADC read fails or battery disabled.
 * Returns V_batt in mV otherwise (e.g. 3950 = 3.95V). */
uint32_t battery_get_voltage_mv(void);

/* Get last measured voltage (cached by monitor task). Thread-safe.
 * Returns 0 if no measurement has been taken yet or battery disabled. */
uint32_t battery_get_last_mv(void);

/* Get battery state as percentage (0-100). Linear interpolation between
 * BATT_CRITICAL_MV (0%) and 4200mV (100%, full Li-Ion). Returns 0 if
 * disabled or no reading yet. */
uint8_t battery_get_percent(void);

/* Background monitor task. Reads voltage every BATT_CHECK_MIN minutes.
 * If V_batt < BATT_CRITICAL_MV (and reading is valid), enters deep sleep
 * for BATT_SLEEP_MIN minutes to preserve battery.
 * Call via xTaskCreate(battery_monitor_task, "bat", BATT_TASK_STACK, ...).
 * When disabled, the task immediately self-deletes. */
void battery_monitor_task(void *arg);

/* Enter deep sleep for the given number of minutes. On wake, ESP8266
 * restarts from app_main. Used by monitor task on critical battery. */
void battery_enter_deep_sleep(uint32_t minutes);

#endif /* BATTERY_H */
