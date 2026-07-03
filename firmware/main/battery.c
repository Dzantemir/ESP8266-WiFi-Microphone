/*
 * Battery monitoring - ported from ESP8285-WEBSERVER project
 * (https://github.com/Dzantemir/ESP8285-WEBSERVER).
 *
 * Uses ESP8266's ADC (TOUT pin, 0-1V, 10-bit) with an external voltage
 * divider to measure battery voltage. A background task periodically
 * checks the voltage and puts the device into deep sleep if it drops
 * below the critical threshold.
 *
 * When CONFIG_STREAMER_BATTERY_ENABLED is NOT set, all functions compile
 * to no-ops / return 0, so callers need no #ifdefs around battery_* calls.
 */
#if BATTERY_ENABLED

/* ---- System / SDK includes ---- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/adc.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "battery.h"

static const char *TAG = "battery";

/* Last measured voltage - written by monitor task, read by anyone.
 * 32-bit aligned write is atomic on Xtensa LX106, so no mutex needed. */
static volatile uint32_t s_last_mv = 0;

esp_err_t battery_init(void)
{
    adc_config_t adc_cfg = {
        .mode = ADC_READ_TOUT_MODE,
        .clk_div = 8,
    };
    esp_err_t err = adc_init(&adc_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Battery monitoring enabled (divider ratio=%u, critical=%u mV)",
             (unsigned)BATT_DIVIDER_RATIO, (unsigned)BATT_CRITICAL_MV);
    return ESP_OK;
}

uint32_t battery_get_voltage_mv(void)
{
    uint32_t adc_sum = 0;
    int valid_samples = 0;
    const int samples = BATT_ADC_SAMPLES;

    for (int i = 0; i < samples; i++)
    {
        uint16_t val = 0;
        if (adc_read(&val) != ESP_OK)
        {
            continue;
        }
        adc_sum += val;
        valid_samples++;
        vTaskDelay(pdMS_TO_TICKS(BATT_ADC_DELAY_MS));
    }

    if (valid_samples == 0)
    {
        ESP_LOGW(TAG, "ADC read failed for all %d samples", samples);
        return 0;
    }

    uint32_t adc_avg = adc_sum / valid_samples;
    uint32_t v_mv = (uint32_t)((adc_avg * (uint32_t)BATT_DIVIDER_RATIO) / 1024);
    return v_mv;
}

void battery_enter_deep_sleep(uint32_t minutes)
{
    ESP_LOGW(TAG, "Entering deep sleep for %u minutes", (unsigned)minutes);

    /* RF calibration on wake: 0=no cal, 1=cal, 2=cal but don't write to NVS.
     * Option 2 is safe and faster than 1, doesn't wear out NVS. */
    esp_deep_sleep_set_rf_option(2);
    esp_deep_sleep((uint64_t)minutes * 60ULL * 1000000ULL);

    /* Should never reach here - esp_deep_sleep() doesn't return. */
    ESP_LOGE(TAG, "esp_deep_sleep returned unexpectedly! Restarting...");
    esp_restart();
}

void battery_monitor_task(void *arg)
{
    /* First measurement immediately. */
    uint32_t v_batt = battery_get_voltage_mv();
    s_last_mv = v_batt;

    if (v_batt == 0)
    {
        ESP_LOGW(TAG, "Battery reading invalid (0 mV) - ADC not connected?");
    }
    else if (v_batt < BATT_BAD_MV)
    {
        ESP_LOGW(TAG, "Battery reading suspiciously low (%u mV < %u) - "
                      "divider disconnected?",
                 (unsigned)v_batt, (unsigned)BATT_BAD_MV);
    }
    else if (v_batt < BATT_CRITICAL_MV)
    {
        ESP_LOGW(TAG, "Battery CRITICAL (%u mV < %u) - deep sleeping",
                 (unsigned)v_batt, (unsigned)BATT_CRITICAL_MV);
        battery_enter_deep_sleep(BATT_SLEEP_MIN);
        return; /* never reached */
    }
    else
    {
        ESP_LOGI(TAG, "Battery OK: %u mV (%u%%)",
                 (unsigned)v_batt, (unsigned)battery_get_percent());
    }

    /* Periodic monitoring loop. */
    const TickType_t check_period = pdMS_TO_TICKS(BATT_CHECK_MIN * 60U * 1000U);
    while (1)
    {
        vTaskDelay(check_period);

        v_batt = battery_get_voltage_mv();
        s_last_mv = v_batt;

        if (v_batt == 0)
        {
            ESP_LOGW(TAG, "ADC read failed - skipping check");
            continue;
        }
        if (v_batt < BATT_BAD_MV)
        {
            ESP_LOGW(TAG, "Battery reading invalid (%u mV) - skipping", (unsigned)v_batt);
            continue;
        }
        if (v_batt < BATT_CRITICAL_MV)
        {
            ESP_LOGW(TAG, "Battery CRITICAL (%u mV < %u) - deep sleeping",
                     (unsigned)v_batt, (unsigned)BATT_CRITICAL_MV);
            battery_enter_deep_sleep(BATT_SLEEP_MIN);
            return; /* never reached */
        }

        ESP_LOGI(TAG, "Battery: %u mV (%u%%)",
                 (unsigned)v_batt, (unsigned)battery_get_percent());
    }
}

/* These are the same for both enabled/disabled - they read the cached
 * value, which is 0 when disabled or not yet measured. */
uint32_t battery_get_last_mv(void)
{
    return s_last_mv;
}

uint8_t battery_get_percent(void)
{
    uint32_t v = s_last_mv;
    if (v == 0)
        return 0;
    if (v <= BATT_CRITICAL_MV)
        return 0;
    if (v >= 4200)
        return 100;
    /* Linear interpolation between critical and 4.2V full. */
    return (uint8_t)((v - BATT_CRITICAL_MV) * 100U / (4200U - BATT_CRITICAL_MV));
}

#endif /* BATTERY_ENABLED */