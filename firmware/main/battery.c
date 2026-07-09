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

/* ---- System / SDK includes ---- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/adc.h"

/* ---- Project includes ---- */
#include "board_config.h"

#if BATTERY_ENABLED

#include "battery.h"

static const char *TAG = "battery";

/* Last measured voltage - written by monitor task, read by anyone.
 * 32-bit aligned write is atomic on Xtensa LX106, so no mutex needed. */
static volatile uint32_t s_last_mv = 0;

/* FIX (M26): the ESP8266 ADC driver is NOT reentrant. battery_get_voltage_mv
 * was previously callable from the AT task (cmd_batt_query) while the
 * monitor task was inside adc_read(), producing garbage readings or driver
 * corruption. Serialize all adc_read() calls with this mutex.
 *
 * FIX (AUDIT-C2): the previous lazy-init in ensure_adc_mutex() had a race -
 * two concurrent callers (AT task + monitor task) could both observe NULL,
 * both create a mutex, and use different handles -> no mutual exclusion.
 * The mutex is now created once in battery_init() before any caller can
 * touch it. battery_get_voltage_mv() logs+returns the cached value if
 * battery_init() was not called. */
static SemaphoreHandle_t s_adc_mutex = NULL;

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

    s_adc_mutex = xSemaphoreCreateMutex();
    if (!s_adc_mutex)
    {
        ESP_LOGE(TAG, "Failed to create ADC mutex");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Battery monitoring enabled (divider ratio=%u, critical=%u mV)",
             (unsigned)BATT_DIVIDER_RATIO, (unsigned)BATT_CRITICAL_MV);
    return ESP_OK;
}

uint32_t battery_get_voltage_mv(void)
{
    /* FIX (M26): serialize ADC access so the AT task and monitor task can't
     * both call adc_read() at the same time. */
    if (!s_adc_mutex ||
        xSemaphoreTake(s_adc_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        ESP_LOGW(TAG, "battery_get_voltage_mv: ADC mutex unavailable "
                      "(battery_init not called or mutex timeout)");
        return s_last_mv; /* return last cached value */
    }

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

    xSemaphoreGive(s_adc_mutex);

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
    else if (v_batt < BATT_START_MV)
    {
        /* FIX (H14): enforce BATT_START_MV at boot. board_config.h documents
         * this as "below on boot -> don't start" but the previous code only
         * checked CRITICAL_MV, allowing the device to boot and stream with
         * a nearly-dead battery (between CRITICAL=3700 and START=3900),
         * draining it further into deep-sleep territory. */
        ESP_LOGW(TAG, "Battery LOW (%u mV < %u) - deep sleeping",
                 (unsigned)v_batt, (unsigned)BATT_START_MV);
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
    /* FIX (M27): use vTaskDelayUntil for accurate periodic timing. vTaskDelay
     * adds the measurement time (~750 ms) to the period, drifting ~40% over
     * time at BATT_CHECK_MIN=1. */
    TickType_t last_wake = xTaskGetTickCount();
    while (1)
    {
        vTaskDelayUntil(&last_wake, check_period);

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