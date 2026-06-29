/*
 * AT command parser for ESP8266 RTOS SDK v3.4.
 *
 * Uses UART0 for both AT command input and log output. Full duplex:
 * AT commands arrive on RX, log output goes to TX.
 *
 * Command format:
 *   AT              -> OK
 *   AT+CMD?         -> +CMD:value\r\nOK
 *   AT+CMD=val      -> OK (or ERROR)
 *
 * All commands must be terminated with \r\n (or just \r).
 *
 * Auto-save: AT+WIFI, AT+PORT, AT+TXPWR, AT+RATE, AT+BITS, AT+FMT, AT+CH
 * automatically save to NVS. No separate AT+SAVE command needed.
 *
 * AT+TXPWR applies immediately (no restart needed).
 * AT+WIFI applies immediately if WiFi driver is running.
 * AT+PORT requires restart (AT+RST) - service port can't be changed on the fly.
 *
 * Audio parameters (AT+RATE, AT+BITS, AT+FMT, AT+CH) apply on next stream start.
 */

#include "at_cmd.h"
#include "config_mgr.h"
#include "svc_port.h"
#include "svc_protocol.h"
#include "wifi_sta.h"
#include "i2s_capture.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <strings.h> /* strcasecmp */
#include <stdarg.h>

/* FreeRTOS MUST come before driver/uart.h and esp_wifi.h! */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "board_config.h"
#include "battery.h"
#include "stream_control.h"
#include "udp_stream.h"
extern uint32_t streaming_get_frame_ms(void);

static const char *TAG = "at_cmd";

#define UART_NUM UART_NUM_0
#define UART_BUF_SIZE 256
#define CMD_BUF_SIZE 256

/* ---- Forward declarations ---- */
static void at_task_fn(void *arg);
static void at_process_line(const char *line, int len);
static void at_send_str(const char *str);
static void at_send_ok(void);
static void at_send_error(void);
static void at_send_data(const char *fmt, ...);

/* Command handlers */
static void cmd_at(void);
static void cmd_rst(void);
static void cmd_gmr(void);
static void cmd_help(void);
static void cmd_wifi_query(void);
static void cmd_wifi_set(const char *args);
static void cmd_host_query(void);
static void cmd_host_set(const char *args);
static void cmd_port_query(void);
static void cmd_port_set(const char *args);
static void cmd_txpwr_query(void);
static void cmd_txpwr_set(const char *args);
static void cmd_bits_query(void);
static void cmd_bits_set(const char *args);
static void cmd_fmt_query(void);
static void cmd_fmt_set(const char *args);
static void cmd_ch_query(void);
static void cmd_ch_set(const char *args);
static void cmd_rate_query(void);
static void cmd_rate_set(const char *args);
static void cmd_gain_query(void);
static void cmd_gain_set(const char *args);
static void cmd_agc_query(void);
static void cmd_agc_set(const char *args);
static void cmd_codec_query(void);
static void cmd_codec_set(const char *args);
static void cmd_rawtx_query(void);
static void cmd_rawtx_set(const char *args);
static void cmd_wch_query(void);
static void cmd_wch_set(const char *args);
#if BATTERY_ENABLED
static void cmd_batt_query(void);
#endif
static void cmd_status(void);
static void cmd_factory(void);
static void cmd_hotrestart(void);

static volatile bool s_running = false;

esp_err_t at_cmd_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    esp_err_t err = uart_param_config(UART_NUM, &uart_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_running = true;
    xTaskCreate(at_task_fn, "at_cmd", TASK_STACK_AT, NULL, TASK_PRIO_AT, NULL);

    ESP_LOGI(TAG, "AT command interface initialized (UART0, %d 8N1)", UART_BAUD_RATE);
    return ESP_OK;
}

/* ---- UART helpers ---- */

static void at_send_str(const char *str)
{
    uart_write_bytes(UART_NUM, str, strlen(str));
}

static void at_send_ok(void)
{
    at_send_str("\r\nOK\r\n");
}

static void at_send_error(void)
{
    at_send_str("\r\nERROR\r\n");
}

static void at_send_data(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
    {
        uart_write_bytes(UART_NUM, buf, (size_t)n);
    }
}

/* ---- AT task ---- */

static void at_task_fn(void *arg)
{
    char cmd_buf[CMD_BUF_SIZE];
    int pos = 0;

    at_send_str("\r\n=== ESP8266 ADPCM Streamer ===\r\n");
    at_send_str("AT command interface ready (115200 8N1)\r\n");
    at_send_str("Type AT+HELP for command list\r\n");

    while (s_running)
    {
        uint8_t ch;
        int n = uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0)
            continue;

        /* Echo off (silent). */

        if (ch == '\r' || ch == '\n')
        {
            if (pos > 0)
            {
                cmd_buf[pos] = '\0';
                at_process_line(cmd_buf, pos);
                pos = 0;
            }
            continue;
        }

        if (pos < CMD_BUF_SIZE - 1)
        {
            cmd_buf[pos++] = (char)ch;
        }
        else
        {
            /* Overflow - discard char, wait for line end. */
            continue;
        }
    }

    vTaskDelete(NULL);
}

/* ---- Command processing ---- */

static void at_process_line(const char *line, int len)
{
    if (len == 0)
        return;

    /* Strip leading whitespace. */
    while (*line == ' ' || *line == '\t')
    {
        line++;
        len--;
    }

    if (strcasecmp(line, "AT") == 0)
    {
        cmd_at();
        return;
    }
    if (strncasecmp(line, "AT+", 3) != 0)
    {
        at_send_error();
        return;
    }

    /* Parse "AT+CMD?" or "AT+CMD=args". */
    const char *p = line + 3;
    const char *eq = strchr(p, '=');
    const char *q = strchr(p, '?');

    char cmd_name[32];
    int cmd_len;
    bool is_query = false;
    const char *args = NULL;

    if (q && (!eq || q < eq))
    {
        cmd_len = (int)(q - p);
        is_query = true;
    }
    else if (eq)
    {
        cmd_len = (int)(eq - p);
        args = eq + 1;
    }
    else
    {
        cmd_len = (int)strlen(p);
    }

    if (cmd_len <= 0 || cmd_len >= (int)sizeof(cmd_name))
    {
        at_send_error();
        return;
    }
    memcpy(cmd_name, p, cmd_len);
    cmd_name[cmd_len] = '\0';

    /* Dispatch. */
    if (strcasecmp(cmd_name, "RST") == 0)
    {
        if (is_query || args)
        {
            at_send_error();
            return;
        }
        cmd_rst();
    }
    else if (strcasecmp(cmd_name, "GMR") == 0)
    {
        if (is_query || args)
        {
            at_send_error();
            return;
        }
        cmd_gmr();
    }
    else if (strcasecmp(cmd_name, "HELP") == 0)
    {
        if (is_query || args)
        {
            at_send_error();
            return;
        }
        cmd_help();
    }
    else if (strcasecmp(cmd_name, "WIFI") == 0)
    {
        if (is_query)
            cmd_wifi_query();
        else if (args)
            cmd_wifi_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "HOST") == 0)
    {
        if (is_query)
            cmd_host_query();
        else if (args)
            cmd_host_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "PORT") == 0)
    {
        if (is_query)
            cmd_port_query();
        else if (args)
            cmd_port_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "TXPWR") == 0)
    {
        if (is_query)
            cmd_txpwr_query();
        else if (args)
            cmd_txpwr_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "BITS") == 0)
    {
        if (is_query)
            cmd_bits_query();
        else if (args)
            cmd_bits_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "FMT") == 0)
    {
        if (is_query)
            cmd_fmt_query();
        else if (args)
            cmd_fmt_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "CH") == 0)
    {
        if (is_query)
            cmd_ch_query();
        else if (args)
            cmd_ch_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "RATE") == 0)
    {
        if (is_query)
            cmd_rate_query();
        else if (args)
            cmd_rate_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "GAIN") == 0)
    {
        if (is_query)
            cmd_gain_query();
        else if (args)
            cmd_gain_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "AGC") == 0)
    {
        if (is_query)
            cmd_agc_query();
        else if (args)
            cmd_agc_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "CODEC") == 0)
    {
        if (is_query)
            cmd_codec_query();
        else if (args)
            cmd_codec_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "RAWTX") == 0)
    {
        if (is_query)
            cmd_rawtx_query();
        else if (args)
            cmd_rawtx_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "WCH") == 0)
    {
        if (is_query)
            cmd_wch_query();
        else if (args)
            cmd_wch_set(args);
        else
            at_send_error();
    }
#if BATTERY_ENABLED
    else if (strcasecmp(cmd_name, "BATT") == 0)
    {
        /* AT+BATT? - query battery voltage.
         * AT+BATT (no ?) - also accepted as query (convenience).
         * AT+BATT=<val> - ERROR (read-only parameter). */
        if (args)
        {
            at_send_error();
        }
        else
        {
            cmd_batt_query();
        }
    }
#endif
    else if (strcasecmp(cmd_name, "STATUS") == 0)
    {
        if (is_query || args)
        {
            at_send_error();
            return;
        }
        cmd_status();
    }
    else if (strcasecmp(cmd_name, "FACTORY") == 0)
    {
        if (is_query || args)
        {
            at_send_error();
            return;
        }
        cmd_factory();
    }
    else if (strcasecmp(cmd_name, "HOTRESTART") == 0)
    {
        if (is_query || args)
        {
            at_send_error();
            return;
        }
        cmd_hotrestart();
    }
    else
    {
        at_send_data("+ERR:unknown command \"%s\"\r\n", cmd_name);
        at_send_error();
    }
}

/* ---- Command implementations ---- */

static void cmd_at(void)
{
    at_send_ok();
}

static void cmd_rst(void)
{
    at_send_str("\r\nOK\r\nRestarting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(100)); /* flush UART */

    /* FIX: Properly stop streaming BEFORE esp_restart().
     *
     * We need BOTH:
     *   1. Clean pipeline shutdown (stop_streaming in main loop) - this
     *      drains queues, frees encoders, deinits I2S/UDP properly.
     *   2. Hardware deinit (safety net) - catches anything missed.
     *
     * Step 1: Signal STOP_REQ and POLL streaming_is_active() until it
     * returns false (main loop processed the stop). This is better than
     * blind vTaskDelay - exits immediately when done, up to 5s timeout.
     * Then Step 2: force-deinit any remaining hardware. */

    if (streaming_is_active())
    {
        streaming_request_stop();

        /* Poll until stop_streaming() completes (STREAM_EVT_ACTIVE cleared).
         * Max wait 5 seconds (50 x 100ms). stop_streaming internally waits
         * up to 3s per task, so 5s total is enough. */
        for (int i = 0; i < 50 && streaming_is_active(); i++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (streaming_is_active())
        {
            /* Timeout - main loop didn't process stop in 5s.
             * Force-clear the flag and proceed with hardware deinit. */
            ESP_LOGW(TAG, "AT+RST: stream stop timeout - force deinit");
        }
    }

    /* Step 2: Safety net - force hardware deinit regardless of step 1. */
    udp_stream_deinit();  /* close UDP socket / raw TX transport */
    i2s_capture_deinit(); /* stop I2S DMA, uninstall driver */
    wifi_sta_deinit();    /* stop WiFi radio, uninstall driver */

    vTaskDelay(pdMS_TO_TICKS(100)); /* let hardware settle */
    esp_restart();
}

static void cmd_gmr(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+GMR:ESP8266 ADPCM Streamer " FIRMWARE_VERSION "\r\n");
    at_send_data("+GMR:SDK ESP8266_RTOS_SDK v3.4\r\n");
    at_send_data("+GMR:Codec DVI4 IMA ADPCM (RFC 3551)\r\n");
    at_send_data("+GMR:Audio %u Hz, %d ms frames\r\n",
                 (unsigned)cfg.sample_rate, streaming_get_frame_ms());
    at_send_data("+GMR:Mic INMP441 I2S\r\n");
    at_send_ok();
}

static void cmd_help(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);

    at_send_str("\r\n+HELP:--- AT Command List ---\r\n");
    at_send_str("+HELP:AT             - test connection\r\n");
    at_send_str("+HELP:AT+RST         - restart device\r\n");
    at_send_str("+HELP:AT+GMR         - show version\r\n");
    at_send_str("+HELP:AT+HELP        - this help\r\n");
    at_send_str("+HELP:AT+WIFI?       - show WiFi settings\r\n");
    at_send_str("+HELP:AT+WIFI=s,pwd  - set WiFi (auto-save, applied immediately)\r\n");
    at_send_str("+HELP:AT+HOST?       - show DHCP hostname\r\n");
    at_send_str("+HELP:AT+HOST=name   - set hostname (auto-save, restart required)\r\n");
    at_send_str("+HELP:AT+PORT?       - show service/discovery port\r\n");
    at_send_str("+HELP:AT+PORT=n      - set service port (auto-save, restart required)\r\n");
    at_send_str("+HELP:AT+TXPWR?      - show WiFi TX power\r\n");
    at_send_str("+HELP:AT+TXPWR=n     - set TX power in dBm 0-20 (auto-save, immediate)\r\n");
    at_send_str("+HELP:AT+RATE?       - show sample rate\r\n");
    at_send_str("+HELP:AT+RATE=n      - set rate 8000/11025/16000/22050/32000/44100/48000\r\n");
    at_send_str("+HELP:AT+GAIN?       - show digital gain (0-64)\r\n");
    at_send_str("+HELP:AT+GAIN=n      - set gain 0-64 (0=bypass, 32=+30dB, auto-save, hotrestart)\r\n");
    at_send_str("+HELP:AT+AGC?        - show AGC mode (0=OFF, 1=LOW, 2=MEDIUM, 3=HIGH)\r\n");
    at_send_str("+HELP:AT+AGC=0|1|2|3 - set AGC preset (0=off 1=low hiss 2=balanced 3=fast)\r\n");
    at_send_str("+HELP:AT+CODEC?      - show codec (0=ADPCM, 1=PCM)\r\n");
    at_send_str("+HELP:AT+CODEC=0|1   - set codec (0=adpcm 1=pcm, auto-save, hotrestart)\r\n");
    at_send_str("+HELP:AT+RAWTX?      - show raw TX mode (0=UDP, 1=raw 802.11)\r\n");
    at_send_str("+HELP:AT+RAWTX=0|1   - set raw TX (0=udp 1=raw wifi, auto-save, reboot)\r\n");
    at_send_str("+HELP:AT+WCH?        - show wifi channel (1-13)\r\n");
    at_send_str("+HELP:AT+WCH=n       - set wifi channel 1-13 (auto-save, raw TX only)\r\n");
#if BATTERY_ENABLED
    at_send_str("+HELP:AT+BATT?       - show battery voltage and charge level\r\n");
#endif
    at_send_str("+HELP:AT+BITS?       - show I2S bits per sample\r\n");
    at_send_str("+HELP:AT+BITS=16|24  - set I2S bits (auto-save, hotrestart to apply)\r\n");
    at_send_str("+HELP:AT+FMT?        - show I2S communication format\r\n");
    at_send_str("+HELP:AT+FMT=0|1     - 0=Philips I2S  1=LSB (auto-save, hotrestart to apply)\r\n");
    at_send_str("+HELP:AT+CH?         - show I2S channel format\r\n");
    at_send_str("+HELP:AT+CH=0|1|2    - 0=left 1=right 2=stereo (auto-save, hotrestart to apply)\r\n");
    at_send_str("+HELP:AT+STATUS      - full device status\r\n");
    at_send_str("+HELP:AT+HOTRESTART   - restart stream to apply audio changes (no reboot)\r\n");
    at_send_str("+HELP:AT+FACTORY     - factory reset (restart required)\r\n");
    at_send_str("+HELP:--- Audio Parameters ---\r\n");
    at_send_data("+HELP:  Sample rate: %u Hz (use AT+RATE to change)\r\n", (unsigned)cfg.sample_rate);
    at_send_data("+HELP:  Frame duration: %d ms\r\n", streaming_get_frame_ms());
    at_send_data("+HELP:  Bits: %d-bit (use AT+BITS to change)\r\n", cfg.bits_per_sample);
    at_send_data("+HELP:  Gain: %u (use AT+GAIN to change, 0=bypass)\r\n", (unsigned)cfg.gain);
    at_send_data("+HELP:  AGC: mode %u (use AT+AGC to change)\r\n", (unsigned)cfg.agc_mode);
    at_send_data("+HELP:  CODEC: %u (use AT+CODEC to change, 0=ADPCM 1=PCM)\r\n", (unsigned)cfg.codec_mode);
    at_send_data("+HELP:  RAWTX: %u (use AT+RAWTX to change, 0=UDP 1=Raw 802.11)\r\n", (unsigned)cfg.rawtx_mode);
    if (cfg.rawtx_mode)
    {
        at_send_data("+HELP:  WCH: %u (use AT+WCH to change, 1-13)\r\n", (unsigned)cfg.wifi_channel);
    }
    at_send_ok();
}

static void cmd_wifi_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+WIFI:ssid=\"%s\"\r\n", cfg.wifi_ssid);
    at_send_data("+WIFI:password=\"%s\"\r\n", cfg.wifi_password);
    at_send_ok();
}

static void cmd_wifi_set(const char *args)
{
    /* Format: ssid,password */
    char buf[128];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *comma = strchr(buf, ',');
    if (!comma)
    {
        at_send_data("+ERR:format is ssid,password\r\n");
        at_send_error();
        return;
    }
    *comma = '\0';
    const char *ssid = buf;
    const char *pwd = comma + 1;

    if (!ssid[0] || !pwd[0])
    {
        at_send_data("+ERR:ssid and password required\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_wifi(ssid, pwd);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_wifi failed\r\n");
        at_send_error();
        return;
    }

    /* Применяем немедленно - переподключаемся к новой AP. */
    err = wifi_sta_reconfigure(ssid, pwd);
    if (err != ESP_OK)
    {
        at_send_data("+WIFI:saved but reconfigure failed (restart required)\r\n");
        at_send_ok();
        return;
    }

    at_send_data("+WIFI:set to ssid=\"%s\" (saved, reconnecting)\r\n", ssid);
    at_send_ok();
}

static void cmd_host_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+HOST:\"%s\"\r\n", cfg.hostname);
    at_send_ok();
}

static void cmd_host_set(const char *args)
{
    /* Format: hostname (alphanumeric + hyphens, 1-32 chars) */
    if (!args || !args[0])
    {
        at_send_data("+ERR:hostname required\r\n");
        at_send_error();
        return;
    }
    if (strlen(args) >= 32)
    {
        at_send_data("+ERR:hostname too long (max 32 chars)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_hostname(args);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:invalid hostname (use alphanumeric + hyphens)\r\n");
        at_send_error();
        return;
    }

    at_send_data("+HOST:set to \"%s\" (saved, restart required)\r\n", args);
    at_send_ok();
}

static void cmd_port_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+PORT:%u\r\n", (unsigned)cfg.svc_port);
    at_send_ok();
}

static void cmd_port_set(const char *args)
{
    char *endptr = NULL;
    long port = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || port < 1 || port > 65535)
    {
        at_send_data("+ERR:port must be 1-65535\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_svc_port((uint16_t)port);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_svc_port failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+PORT:set to %u (saved, restart required)\r\n", (unsigned)port);
    at_send_ok();
}

static void cmd_txpwr_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+TXPWR:%u dBm (max %u)\r\n", cfg.tx_power, WIFI_TX_POWER_MAX);
    at_send_ok();
}

static void cmd_txpwr_set(const char *args)
{
    char *endptr = NULL;
    long power = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || power < WIFI_TX_POWER_MIN || power > WIFI_TX_POWER_MAX)
    {
        at_send_data("+ERR:txpwr must be %d-%d dBm\r\n", WIFI_TX_POWER_MIN, WIFI_TX_POWER_MAX);
        at_send_error();
        return;
    }

    esp_err_t err = config_set_tx_power((uint8_t)power);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_tx_power failed\r\n");
        at_send_error();
        return;
    }

    /* Apply immediately. */
    wifi_sta_set_tx_power((uint8_t)power);

    at_send_data("+TXPWR:set to %u dBm (saved, applied)\r\n", (unsigned)power);
    at_send_ok();
}

static void cmd_bits_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+BITS:%d\r\n", cfg.bits_per_sample);
    at_send_ok();
}

static void cmd_bits_set(const char *args)
{
    char *endptr = NULL;
    long bits = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || (bits != 16 && bits != 24))
    {
        at_send_data("+ERR:bits must be 16 or 24\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_bits_per_sample((uint8_t)bits);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_bits failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+BITS:set to %d (saved, saved, use AT+HOTRESTART to apply)\r\n", (int)bits);
    at_send_ok();
}

static void cmd_fmt_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+FMT:%d (%s)\r\n", cfg.comm_format,
                 cfg.comm_format == I2S_CFMT_LSB ? "LSB" : "Philips");
    at_send_ok();
}

static void cmd_fmt_set(const char *args)
{
    char *endptr = NULL;
    long fmt = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || (fmt != I2S_CFMT_PHILIPS && fmt != I2S_CFMT_LSB))
    {
        at_send_data("+ERR:fmt must be 0 (Philips) or 1 (LSB)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_comm_format((uint8_t)fmt);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_fmt failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+FMT:set to %d (%s, saved, saved, use AT+HOTRESTART to apply)\r\n",
                 (int)fmt, fmt == I2S_CFMT_LSB ? "LSB" : "Philips");
    at_send_ok();
}

static void cmd_ch_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    /* Выводим user-friendly 0/1/2 (как в AT+CH=n), а не сырые 4/3/0 SDK. */
    int user_val;
    const char *desc;
    switch (cfg.channel_format)
    {
    case I2S_CHFMT_LEFT:
        user_val = 0;
        desc = "left";
        break;
    case I2S_CHFMT_RIGHT:
        user_val = 1;
        desc = "right";
        break;
    case I2S_CHFMT_STEREO:
        user_val = 2;
        desc = "stereo";
        break;
    default:
        user_val = -1;
        desc = "unknown";
        break;
    }
    at_send_data("+CH:%d (%s, %u ch)\r\n", user_val, desc,
                 (unsigned)channel_format_to_count(cfg.channel_format));
    at_send_ok();
}

static void cmd_ch_set(const char *args)
{
    char *endptr = NULL;
    long ch = strtol(args, &endptr, 10);
    uint8_t fmt;
    switch (ch)
    {
    case 0:
        fmt = I2S_CHFMT_LEFT;
        break;
    case 1:
        fmt = I2S_CHFMT_RIGHT;
        break;
    case 2:
        fmt = I2S_CHFMT_STEREO;
        break;
    default:
        at_send_data("+ERR:ch must be 0 (left), 1 (right), or 2 (stereo)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_channel_format(fmt);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_ch failed\r\n");
        at_send_error();
        return;
    }

    /* Update svc_port::s_channels immediately so the next DISCOVER gets
     * the correct channel count. Without this, INFO packets still carry
     * the old channels value until the next stream start, and the server
     * opens WaveOut with the wrong nCh -> rejects all audio packets. */
    int new_ch = i2s_capture_channel_count(fmt);
    svc_port_set_channels((uint8_t)new_ch);

    at_send_data("+CH:set to %d (saved, saved, use AT+HOTRESTART to apply)\r\n", (int)ch);
    at_send_ok();
}

static void cmd_rate_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+RATE:%u\r\n", (unsigned)cfg.sample_rate);
    at_send_ok();
}

static void cmd_rate_set(const char *args)
{
    char *endptr = NULL;
    long rate = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0')
    {
        at_send_data("+ERR:rate must be a number\r\n");
        at_send_error();
        return;
    }
    if (!sample_rate_is_valid((uint32_t)rate))
    {
        at_send_data("+ERR:rate must be 8000/11025/16000/22050/32000/44100/48000\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_sample_rate((uint32_t)rate);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_sample_rate failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+RATE:set to %u (saved, saved, use AT+HOTRESTART to apply)\r\n", (unsigned)rate);
    at_send_ok();
}

static void cmd_gain_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+GAIN:%u\r\n", (unsigned)cfg.gain);
    at_send_ok();
}

static void cmd_gain_set(const char *args)
{
    char *endptr = NULL;
    long gain = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || gain < 0 || gain > 64)
    {
        at_send_data("+ERR:gain must be 0-64 (0=bypass, 32=+30dB)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_gain((uint8_t)gain);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_gain failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+GAIN:set to %u (saved, saved, use AT+HOTRESTART to apply)\r\n", (unsigned)gain);
    at_send_ok();
}

static void cmd_agc_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    const char *name;
    const char *desc;
    switch (cfg.agc_mode)
    {
    case AGC_MODE_OFF:
        name = "OFF";
        desc = "fixed gain (use AT+GAIN)";
        break;
    case AGC_MODE_LOW:
        name = "LOW";
        desc = "attack=50, release=10 - minimal hiss";
        break;
    case AGC_MODE_MEDIUM:
        name = "MEDIUM";
        desc = "attack=75, release=20 - balanced";
        break;
    case AGC_MODE_HIGH:
        name = "HIGH";
        desc = "attack=90, release=50 - fast reaction";
        break;
    default:
        name = "?";
        desc = "unknown";
        break;
    }
    at_send_data("+AGC:%u (%s - %s)\r\n", (unsigned)cfg.agc_mode, name, desc);
    at_send_ok();
}

static void cmd_agc_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || val < 0 || val > AGC_MODE_HIGH)
    {
        at_send_data("+ERR:agc must be 0=OFF, 1=LOW, 2=MEDIUM, 3=HIGH\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_agc_mode((uint8_t)val);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_agc_mode failed\r\n");
        at_send_error();
        return;
    }

    const char *name;
    switch (val)
    {
    case AGC_MODE_OFF:
        name = "OFF (use fixed gain)";
        break;
    case AGC_MODE_LOW:
        name = "LOW (minimal hiss, slow)";
        break;
    case AGC_MODE_MEDIUM:
        name = "MEDIUM (balanced)";
        break;
    case AGC_MODE_HIGH:
        name = "HIGH (fast, more hiss)";
        break;
    default:
        name = "?";
        break;
    }
    at_send_data("+AGC:set to %ld (%s, saved, saved, use AT+HOTRESTART to apply)\r\n",
                 val, name);
    if (val != AGC_MODE_OFF)
    {
        at_send_data("+AGC:target=-18dBFS, max=+36dB, noise gate=-42dBFS\r\n");
    }
    at_send_ok();
}

static void cmd_codec_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    const char *name = (cfg.codec_mode == CODEC_MODE_PCM) ? "PCM" : "ADPCM";
    const char *desc = (cfg.codec_mode == CODEC_MODE_PCM)
                           ? "raw 16/24-bit, no compression"
                           : "DVI4 IMA, 4 bits/sample";
    at_send_data("+CODEC:%u (%s - %s)\r\n",
                 (unsigned)cfg.codec_mode, name, desc);
    at_send_ok();
}

static void cmd_codec_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' ||
        val < CODEC_MODE_ADPCM || val > CODEC_MODE_PCM)
    {
        at_send_data("+ERR:codec must be 0 (ADPCM) or 1 (PCM)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_codec_mode((uint8_t)val);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_codec_mode failed\r\n");
        at_send_error();
        return;
    }

    const char *name = (val == CODEC_MODE_PCM) ? "PCM (raw 16/24-bit)"
                                               : "ADPCM (DVI4 IMA)";
    at_send_data("+CODEC:set to %ld (%s, saved, saved, use AT+HOTRESTART to apply)\r\n",
                 val, name);
    at_send_ok();
}

static void cmd_rawtx_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    const char *name = cfg.rawtx_mode ? "Raw 802.11 TX" : "UDP via WiFi";
    at_send_data("+RAWTX:%u (%s)\r\n", (unsigned)cfg.rawtx_mode, name);
    at_send_ok();
}

static void cmd_rawtx_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || val < 0 || val > 1)
    {
        at_send_data("+ERR:rawtx must be 0 (UDP) or 1 (Raw 802.11)\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_rawtx_mode((uint8_t)val);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_rawtx_mode failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+RAWTX:set to %ld (%s, saved, REBOOT REQUIRED)\r\n",
                 val, val ? "Raw 802.11 TX" : "UDP via WiFi");
    at_send_ok();
}

static void cmd_wch_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+WCH:%u (channel %u)\r\n",
                 (unsigned)cfg.wifi_channel, (unsigned)cfg.wifi_channel);
    at_send_ok();
}

static void cmd_wch_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || val < 1 || val > 13)
    {
        at_send_data("+ERR:wch must be 1-13\r\n");
        at_send_error();
        return;
    }

    esp_err_t err = config_set_wifi_channel((uint8_t)val);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_wifi_channel failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+WCH:set to %ld (saved, applies on next boot/raw TX start)\r\n", val);
    at_send_ok();
}

#if BATTERY_ENABLED
static void cmd_batt_query(void)
{
    /* AT+BATT? or AT+BATT - returns last measured battery voltage + percent.
     * Triggers a fresh ADC reading (takes ~750ms) for accuracy. */
    uint32_t v_mv = battery_get_voltage_mv();
    uint8_t pct = battery_get_percent();

    if (v_mv == 0)
    {
        at_send_data("+BATT:ADC not connected (reading invalid)\r\n");
    }
    else if (v_mv < BATT_BAD_MV)
    {
        at_send_data("+BATT:%u mV (invalid reading, divider disconnected?)\r\n",
                     (unsigned)v_mv);
    }
    else
    {
        const char *state;
        if (v_mv < BATT_CRITICAL_MV)
        {
            state = "CRITICAL - deep sleep pending";
        }
        else if (v_mv < BATT_START_MV)
        {
            state = "LOW";
        }
        else
        {
            state = "OK";
        }
        at_send_data("+BATT:%u mV (%u%%, %s)\r\n",
                     (unsigned)v_mv, (unsigned)pct, state);
    }
    at_send_ok();
}
#endif

static void cmd_hotrestart(void)
{
    /* AT+HOTRESTART - restart the stream without rebooting the device.
     *
     * Stops the current stream and starts a new one with the current NVS
     * config. This lets audio parameter changes (AT+RATE, AT+BITS, AT+FMT,
     * AT+CH, AT+GAIN, AT+AGC, AT+CODEC) take effect immediately without a
     * full ~1s reboot.
     *
     * ONLY works when the stream is currently active. If the stream is
     * already stopped (e.g., after CMD_STOP from server), this command
     * does nothing - the saved params will apply naturally on the next
     * stream start. This prevents HOTRESTART from overriding an
     * intentional stop.
     *
     * Does NOT reload NVS-only boot params like WiFi SSID, rawtx_mode,
     * or svc_port - for those, use AT+RST (full reboot). */
    if (!streaming_is_active())
    {
        at_send_data("+HOTRESTART:stream not active - params saved, "
                     "will apply on next start\r\n");
        at_send_ok();
        return;
    }
    if (streaming_request_restart())
    {
        at_send_data("+HOTRESTART:stream restart requested (stop+start)\r\n");
        at_send_ok();
    }
    else
    {
        at_send_data("+ERR:stream control not initialized\r\n");
        at_send_error();
    }
}

static void cmd_status(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    svc_port_status_t st;
    svc_port_get_status(&st);

    at_send_data("+STATUS:firmware=" FIRMWARE_VERSION "\r\n");
    at_send_data("+STATUS:wifi_ssid=\"%s\"\r\n", cfg.wifi_ssid);
    at_send_data("+STATUS:hostname=\"%s\"\r\n", cfg.hostname);
    at_send_data("+STATUS:wifi_connected=%s\r\n", wifi_sta_is_connected() ? "YES" : "NO");
    if (wifi_sta_is_connected())
    {
        tcpip_adapter_ip_info_t ip_info;
        if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info) == ESP_OK)
        {
            at_send_data("+STATUS:ip=%d.%d.%d.%d\r\n",
                         (int)((ip_info.ip.addr >> 0) & 0xFF),
                         (int)((ip_info.ip.addr >> 8) & 0xFF),
                         (int)((ip_info.ip.addr >> 16) & 0xFF),
                         (int)((ip_info.ip.addr >> 24) & 0xFF));
        }
        else
        {
            at_send_data("+STATUS:ip=error\r\n");
        }
    }
    at_send_data("+STATUS:svc_port=%u\r\n", (unsigned)cfg.svc_port);
    at_send_data("+STATUS:svc_running=%s\r\n", st.running ? "YES" : "NO");
    at_send_data("+STATUS:svc_protocol=EASSP v%d (0xEA%02X)\r\n", EASSP_VER, EASSP_MAGIC1);
    at_send_data("+STATUS:svc_commands=DISCOVER,CONFIGURE,STOP\r\n");
    at_send_data("+STATUS:mac=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                 st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        at_send_data("+STATUS:wifi_rssi=%d dBm\r\n", ap_info.rssi);
    }
    at_send_data("+STATUS:wifi_tx_power=%u dBm\r\n", cfg.tx_power);
    at_send_data("+STATUS:streaming=%s\r\n", st.streaming ? "YES" : "NO");
    at_send_data("+STATUS:sample_rate=%u\r\n", (unsigned)cfg.sample_rate);
    at_send_data("+STATUS:frame_ms=%d (fixed)\r\n", streaming_get_frame_ms());
    at_send_data("+STATUS:bits_per_sample=%d\r\n", cfg.bits_per_sample);
    at_send_data("+STATUS:gain=%u (0=bypass, 32=+30dB)\r\n", (unsigned)cfg.gain);
    at_send_data("+STATUS:agc=%u (0=OFF 1=LOW 2=MEDIUM 3=HIGH)\r\n", (unsigned)cfg.agc_mode);
    at_send_data("+STATUS:codec=%u (%s)\r\n", (unsigned)cfg.codec_mode,
                 cfg.codec_mode == CODEC_MODE_PCM ? "PCM" : "ADPCM");
    at_send_data("+STATUS:rawtx=%u (%s)\r\n", (unsigned)cfg.rawtx_mode,
                 cfg.rawtx_mode ? "Raw 802.11 TX" : "UDP");
    if (cfg.rawtx_mode)
    {
        at_send_data("+STATUS:wifi_channel=%u\r\n", (unsigned)cfg.wifi_channel);
    }
#if BATTERY_ENABLED
    {
        uint32_t batt_mv = battery_get_last_mv();
        uint8_t batt_pct = battery_get_percent();
        if (batt_mv == 0)
        {
            at_send_data("+STATUS:battery=not measured yet\r\n");
        }
        else
        {
            at_send_data("+STATUS:battery=%u mV (%u%%)\r\n",
                         (unsigned)batt_mv, (unsigned)batt_pct);
        }
    }
#else
    at_send_data("+STATUS:battery=disabled (menuconfig)\r\n");
#endif
    at_send_data("+STATUS:comm_format=%d (%s)\r\n", cfg.comm_format,
                 cfg.comm_format == I2S_CFMT_LSB ? "LSB" : "Philips");
    {
        const char *ch_desc;
        switch (cfg.channel_format)
        {
        case I2S_CHFMT_LEFT:
            ch_desc = "left";
            break;
        case I2S_CHFMT_RIGHT:
            ch_desc = "right";
            break;
        case I2S_CHFMT_STEREO:
            ch_desc = "stereo";
            break;
        default:
            ch_desc = "unknown";
            break;
        }
        at_send_data("+STATUS:channel_format=%d (%s, %u ch)\r\n",
                     cfg.channel_format, ch_desc,
                     (unsigned)channel_format_to_count(cfg.channel_format));
    }
    at_send_data("+STATUS:bitrate=%u\r\n",
                 (unsigned)(cfg.sample_rate * 4 * channel_format_to_count(cfg.channel_format)));
    at_send_data("+STATUS:error=%s (%d)\r\n",
                 st.error_code == 0 ? "NONE" : st.error_code == 1 ? "MEMORY"
                                           : st.error_code == 2   ? "I2S"
                                           : st.error_code == 3   ? "CODEC"
                                           : st.error_code == 4   ? "NETWORK"
                                           : st.error_code == 5   ? "WATCHDOG"
                                           : st.error_code == 6   ? "CONFIG"
                                                                  : "UNKNOWN",
                 st.error_code);
    if (st.streaming)
    {
        at_send_data("+STATUS:watchdog=%d ms remaining\r\n", st.watchdog_remaining_ms);
    }
    else
    {
        at_send_data("+STATUS:watchdog=OFF (not streaming)\r\n");
    }
    at_send_data("+STATUS:packets_sent=%u\r\n", (unsigned)st.packets_sent);
    at_send_data("+STATUS:free_heap=%u\r\n", (unsigned)esp_get_free_heap_size());
    at_send_ok();
}

static void cmd_factory(void)
{
    esp_err_t err = config_factory_reset();
    if (err != ESP_OK)
    {
        at_send_data("+ERR:factory reset failed\r\n");
        at_send_error();
        return;
    }
    at_send_str("\r\n+FACTORY:defaults restored (restart required)\r\n");
    at_send_ok();
}
