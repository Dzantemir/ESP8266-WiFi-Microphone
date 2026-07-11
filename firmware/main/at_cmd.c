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

/* ---- System / SDK includes ---- */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <strings.h> /* strcasecmp */
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
/* FIX (AUDIT-MEDIUM): explicit include - the file uses tcpip_adapter_ip_info_t
 * and tcpip_adapter_get_ip_info() directly. Was relying on transitive
 * inclusion from esp_wifi.h, which is fragile across SDK upgrades. */
#include "tcpip_adapter.h"

/* ---- Project includes ---- */
#include "board_config.h"
#include "at_cmd.h"
#include "config_mgr.h"
#include "svc_port.h"
#include "svc_protocol.h"
#include "wifi_sta.h"
#include "i2s_capture.h"
#include "battery.h"
#include "stream_control.h"
#include "stream_mode.h"
extern uint32_t streaming_get_frame_ms(void);

static const char *TAG = "at_cmd";

#define UART_NUM UART_NUM_0
/* FIX (M25): increase UART RX buffer to 1024 so a long AT handler
 * (e.g. cmd_batt_query ~750ms before H13 fix, or cmd_status ~50ms with
 * many at_send_data calls) doesn't overflow the RX queue when the host
 * sends another command during that window. At 115200 baud, 512 bytes
 * fills in 44ms - easily exceeded. 1024 gives ~90ms of headroom. */
#define UART_BUF_SIZE 512
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
static void cmd_wch_query(void);
static void cmd_wch_set(const char *args);
static void cmd_xport_query(void);
static void cmd_xport_set(const char *args);
static void cmd_timing_query(void);
static void cmd_timing_set(const char *args);
#if BATTERY_ENABLED
static void cmd_batt_query(void);
#endif
static void cmd_status(void);
static void cmd_factory(void);
static void cmd_hotrestart(void);
/* FIX (UART0/B9): AT+LOG command to mute/unmute ESP_LOG output on UART0.
 * Since AT and ESP_LOG share UART0, log lines mixed with AT responses can
 * break host-side AT parsers. AT+LOG=0 silences logs; AT+LOG=1 restores. */
static void cmd_log_set(const char *args);
static void cmd_log_query(void);

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
        /* FIX (C1): vsnprintf returns the number of chars that WOULD have
         * been written, which may exceed sizeof(buf)-1 on truncation. Cap
         * the write length to the actual buffer size to avoid reading
         * uninitialized stack memory past buf[]. */
        size_t write_len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
        uart_write_bytes(UART_NUM, buf, write_len);
        /* FIX (GROK-26): if the output was truncated, append a visible
         * ellipsis marker so the user knows the response is incomplete.
         * Without this, a long +STATUS:... response would silently lose
         * its tail and the user couldn't tell whether the data ended
         * naturally or was cut off. The ellipsis fits because write_len
         * was capped at sizeof(buf)-1, leaving one byte for '\0'; we
         * overwrite the last 3 chars with "..." to stay in-bounds. */
        if ((size_t)n > sizeof(buf) - 1 && write_len >= 3)
        {
            uart_write_bytes(UART_NUM, "...[truncated]\r\n", 16);
        }
    }
}

/* ---- AT task ---- */

static void at_task_fn(void *arg)
{
    char cmd_buf[CMD_BUF_SIZE];
    int pos = 0;
    /* FIX (GROK-25): one-shot flag so we report command-buffer overflow
     * once per over-long line, not once per discarded byte. Reset on
     * every CR/LF so a subsequent long command can also report. */
    bool overflow_reported = false;
    /* FIX (GROK-G11-16): discard flag — once overflow is detected, discard
     * ALL remaining bytes of the current line until CR/LF. Previously the
     * truncated buffer was still dispatched to at_process_line on the next
     * CR/LF, which could parse a valid-but-unintended command (e.g.
     * "AT+RATE=48000,extra" truncated to "AT+RATE=48000" would set the rate
     * to 48000 instead of returning an error). Now the line is fully
     * discarded. */
    bool overflow_discard = false;

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
            /* Only dispatch if we were NOT in discard mode. If overflow_discard
             * is set, the line was too long and we intentionally dropped the
             * rest — do NOT process the partial buffer. */
            if (pos > 0 && !overflow_discard)
            {
                cmd_buf[pos] = '\0';
                at_process_line(cmd_buf, pos);
            }
            pos = 0;
            /* Reset overflow flags for the next command line. */
            overflow_reported = false;
            overflow_discard = false;
            continue;
        }

        /* If we're discarding an over-long line, skip the buffer write. */
        if (overflow_discard)
            continue;

        if (pos < CMD_BUF_SIZE - 1)
        {
            cmd_buf[pos++] = (char)ch;
        }
        else
        {
            /* Overflow - discard char, wait for line end.
             * FIX (GROK-25): emit an ERROR so the user knows their
             * command was truncated. Use a one-shot flag so we don't spam
             * ERROR for every discarded byte of a long paste.
             * FIX (GROK-G11-16): set overflow_discard so the rest of the
             * line is dropped and at_process_line is NOT called with the
             * truncated buffer (which could parse a valid-but-unintended
             * command). */
            if (!overflow_reported)
            {
                at_send_data("+ERR:command too long (max %d chars), discarded\r\n",
                             (int)(CMD_BUF_SIZE - 1));
                overflow_reported = true;
                overflow_discard = true;
            }
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

    /* FIX (AUDIT-LOW): re-check len after the strip. A whitespace-only line
     * (e.g. "AT \r") would otherwise fall through to strncasecmp and return
     * ERROR, violating ITU-T V.250 which says AT commands with only
     * whitespace should be silently ignored. */
    if (len == 0)
        return;

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
    else if (strcasecmp(cmd_name, "WCH") == 0)
    {
        if (is_query)
            cmd_wch_query();
        else if (args)
            cmd_wch_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "XPORT") == 0)
    {
        if (is_query)
            cmd_xport_query();
        else if (args)
            cmd_xport_set(args);
        else
            at_send_error();
    }
    else if (strcasecmp(cmd_name, "TIMING") == 0)
    {
        if (is_query)
            cmd_timing_query();
        else if (args)
            cmd_timing_set(args);
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
    else if (strcasecmp(cmd_name, "LOG") == 0)
    {
        /* FIX (UART0/B9): AT+LOG=0/1 or AT+LOG? */
        if (is_query && !args)
        {
            cmd_log_query();
        }
        else if (args && !is_query)
        {
            cmd_log_set(args);
        }
        else
        {
            at_send_error();
        }
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

    /* FIX (C2): Don't bypass the main loop. The previous implementation
     * force-deinitialized transport/I2S/WiFi from the AT task (priority 1,
     * same as main loop) while the main loop could be inside
     * start_streaming() for up to ~16s (WIFI_CONNECT_TIMEOUT_MS=15s + setup).
     * That freed resources in use by the main task -> crash, lwIP
     * corruption, hard fault before esp_restart() fired.
     *
     * FIX (AUDIT-H8): even with the 5s wait above, if the main loop is still
     * inside start_streaming() when the wait times out, calling
     * transport_deinit/i2s_capture_deinit/wifi_sta_deinit from THIS task
     * re-introduces exactly the C2 crash. The safe path is to just call
     * esp_restart() - the SDK will tear down everything cleanly as part of
     * the reboot, and the main loop never sees its resources disappear
     * mid-operation. We still call streaming_request_stop() first to give
     * the pipeline a chance to wind down gracefully (less chance of
     * orphaned lwIP mutexes across the reboot). */
    if (streaming_is_active())
    {
        streaming_request_stop();
        /* Poll streaming_is_active() until the main loop has finished
         * stop_streaming(). Max wait 5s (50 x 100ms). */
        for (int i = 0; i < 50 && streaming_is_active(); i++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (streaming_is_active())
        {
            ESP_LOGW(TAG, "AT+RST: stream still active after 5s - rebooting anyway "
                          "(SDK will tear down hardware)");
        }
    }

    /* Reboot. The SDK tears down WiFi/I2S/transport as part of reboot,
     * which is safe even if the main loop is mid-start_streaming(). */
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
    at_send_str("+HELP:AT+HOST=name   - set hostname (max 23 chars, auto-save, restart)\r\n");
    at_send_str("+HELP:AT+PORT?       - show service/discovery port\r\n");
    at_send_str("+HELP:AT+PORT=n      - set service port (auto-save, restart required)\r\n");
    at_send_str("+HELP:AT+TXPWR?      - show WiFi TX power\r\n");
    at_send_str("+HELP:AT+TXPWR=n     - set TX power in dBm 0-20 (auto-save, immediate)\r\n");
    at_send_str("+HELP:AT+RATE?       - show sample rate\r\n");
    at_send_str("+HELP:AT+RATE=n      - set rate 8000/11025/16000/22050/32000/44100/48000\r\n");
    at_send_str("+HELP:AT+GAIN?       - show digital gain (0-64)\r\n");
    at_send_str("+HELP:AT+GAIN=n      - set gain 0-64 (0=bypass, 32=+30dB, 16bit:use 4-8)\r\n");
    at_send_str("+HELP:AT+AGC?        - show AGC mode (0-8 presets)\r\n");
    at_send_str("+HELP:AT+AGC=0..8    - set AGC preset (0=off 1=studio 2=podcast 3=balanced 4=fast 5=noisy 6=music 7=limiter 8=surv)\r\n");
    at_send_str("+HELP:AT+CODEC?      - show codec (0=ADPCM, 1=PCM)\r\n");
    at_send_str("+HELP:AT+CODEC=0|1   - set codec (0=adpcm 1=pcm, auto-save, hotrestart)\r\n");
    at_send_str("+HELP:AT+WCH?        - show wifi channel (1-13)\r\n");
    at_send_str("+HELP:AT+WCH=n       - set wifi channel 1-13 (auto-save, raw TX only)\r\n");
    at_send_str("+HELP:AT+XPORT?      - show transport (0=UDP 1=TCP 2=RawTX)\r\n");
    at_send_str("+HELP:AT+XPORT=0|1|2 - set transport (HOTRESTART/RST to apply)\r\n");
    at_send_str("+HELP:AT+TIMING?     - show I2S RX input delays (sd,ws,bck)\r\n");
    at_send_str("+HELP:AT+TIMING=s,w,b - set I2S RX delays 0-3 each (auto-save, hotrestart)\r\n");
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
    at_send_data("+HELP:  Bits: %u-bit (use AT+BITS to change)\r\n", cfg.bits_per_sample);
    at_send_data("+HELP:  Gain: %u (use AT+GAIN to change, 0=bypass)\r\n", (unsigned)cfg.gain);
    {
        const agc_preset_t *p = (cfg.agc_mode < AGC_MODE_COUNT) ?
            &AGC_PRESETS[cfg.agc_mode] : &AGC_PRESETS[0];
        at_send_data("+HELP:  AGC: %u %s (use AT+AGC to change, 0-8)\r\n",
                     (unsigned)cfg.agc_mode, p->name);
    }
    at_send_data("+HELP:  CODEC: %u (use AT+CODEC to change, 0=ADPCM 1=PCM)\r\n", (unsigned)cfg.codec_mode);
    {
        const char *xname = "UDP";
        if (cfg.transport_mode == TRANSPORT_MODE_TCP) xname = "TCP";
        else if (cfg.transport_mode == TRANSPORT_MODE_RAWTX) xname = "Raw 802.11 TX";
        at_send_data("+HELP:  Transport: %u %s (use AT+XPORT to change, 0=UDP 1=TCP 2=RawTX)\r\n",
                     (unsigned)cfg.transport_mode, xname);
    }
    if (cfg.transport_mode == TRANSPORT_MODE_RAWTX)
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
    /* FIX (H11): mask the WiFi password. The AT interface has no
     * authentication, so anyone with serial access could read the plaintext
     * password. Show only the length and a placeholder. */
    size_t plen = strlen(cfg.wifi_password);
    at_send_data("+WIFI:password=<%u chars, hidden>\r\n", (unsigned)plen);
    at_send_ok();
}

static void cmd_wifi_set(const char *args)
{
    /* Format: ssid,password */
    /* FIX (M23): check input length BEFORE truncating. The previous code
     * silently truncated args to 127 chars, then strchr() could find a
     * comma in the truncated portion while the real password was lost.
     * config_set_wifi would then reject with INVALID_SIZE, but the error
     * message didn't say "input too long". */
    if (strlen(args) >= 127)
    {
        at_send_data("+ERR:input too long (max 127 chars total)\r\n");
        at_send_error();
        return;
    }
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

    /* FIX (AUDIT-MEDIUM): validate WPA2 password length per IEEE 802.11i
     * (8-63 chars). Without this, a 1-7 char password is accepted, saved
     * to NVS, and the device silently fails at WiFi association with no
     * clear error. The old credentials are already overwritten, so the
     * device is bricked WiFi-wise until a valid password is set. */
    {
        size_t plen = strlen(pwd);
        if (plen < 8 || plen > 63)
        {
            at_send_data("+ERR:password must be 8-63 chars (got %u)\r\n",
                         (unsigned)plen);
            at_send_error();
            return;
        }
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
    if (strlen(args) >= 24)
    {
        at_send_data("+ERR:hostname too long (max 23 chars)\r\n");
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
    at_send_data("+BITS:%u\r\n", cfg.bits_per_sample);
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

    at_send_data("+BITS:set to %d (saved, use AT+HOTRESTART to apply)\r\n", (int)bits);
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

    at_send_data("+FMT:set to %d (%s, saved, use AT+HOTRESTART to apply)\r\n",
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
    /* FIX (GROK-15): validate the entire argument was consumed.
     * Without this, "AT+CH=2foo" parses 2 and silently ignores "foo",
     * configuring stereo instead of returning an error. All other
     * numeric AT setters (cmd_rate_set, cmd_port_set, cmd_bits_set,
     * cmd_codec_set, cmd_xport_set, cmd_gain_set, cmd_agc_set) already
     * have this check — cmd_ch_set was the only one missing it. */
    if (endptr == args || *endptr != '\0')
    {
        at_send_data("+ERR:ch must be a number (0, 1, or 2)\r\n");
        at_send_error();
        return;
    }
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

    at_send_data("+CH:set to %d (saved, use AT+HOTRESTART to apply)\r\n", (int)ch);
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

    at_send_data("+RATE:set to %u (saved, use AT+HOTRESTART to apply)\r\n", (unsigned)rate);
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

    at_send_data("+GAIN:set to %u (saved, use AT+HOTRESTART to apply)\r\n", (unsigned)gain);
    at_send_ok();
}

static void cmd_agc_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    if (cfg.agc_mode < AGC_MODE_COUNT)
    {
        const agc_preset_t *p = &AGC_PRESETS[cfg.agc_mode];
        at_send_data("+AGC:%u (%s, attack=%u, release=%u)\r\n",
                     (unsigned)cfg.agc_mode, p->name,
                     (unsigned)p->attack, (unsigned)p->release);
    }
    else
    {
        at_send_data("+AGC:%u (unknown)\r\n", (unsigned)cfg.agc_mode);
    }
    at_send_ok();
}

static void cmd_agc_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' || val < 0 || val >= AGC_MODE_COUNT)
    {
        at_send_data("+ERR:agc must be 0-8 (0=OFF 1=Studio 2=Podcast 3=Balanced "
                     "4=Fast 5=Noisy 6=Music 7=Limiter 8=Surveillance)\r\n");
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

    const agc_preset_t *p = &AGC_PRESETS[val];
    at_send_data("+AGC:set to %ld (%s, attack=%u, release=%u, "
                 "saved, use AT+HOTRESTART to apply)\r\n",
                 val, p->name, (unsigned)p->attack, (unsigned)p->release);
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
    at_send_data("+CODEC:set to %ld (%s, saved, use AT+HOTRESTART to apply)\r\n",
                 val, name);
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

/* AT+XPORT? — показать текущий транспорт (0=UDP, 1=TCP, 2=RawTX).
 * AT+XPORT=n — установить, авто-сохранение в NVS, применить HOTRESTART. */
static void cmd_xport_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    const char *name;
    switch (cfg.transport_mode)
    {
    case TRANSPORT_MODE_TCP:   name = "TCP"; break;
    case TRANSPORT_MODE_RAWTX: name = "Raw 802.11 TX"; break;
    case TRANSPORT_MODE_UDP:
    default:                   name = "UDP"; break;
    }
    at_send_data("+XPORT:%u (%s)\r\n", (unsigned)cfg.transport_mode, name);
    at_send_ok();
}

static void cmd_xport_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0' ||
        val < 0 || val > TRANSPORT_MODE_RAWTX)
    {
        at_send_data("+ERR:xport must be 0=UDP, 1=TCP, 2=RawTX\r\n");
        at_send_error();
        return;
    }

    /* FIX (AUDIT-XPORT-CRASH): capture the CURRENTLY ACTIVE transport
     * BEFORE overwriting NVS. The message we print depends on whether the
     * transition crosses the WiFi-mode boundary (AP-associated vs. raw-radio):
     *   - UDP <-> TCP (both AP-associated): applied via AT+HOTRESTART or AT+RST.
     *     Server stop+start does NOT apply it (stream continues on old transport).
     *   - Any transition involving RAWTX (in either direction): AT+RST is
     *     required, because the WiFi driver must be reinit in a different
     *     mode and on-the-fly deinit crashes on ESP8266 RTOS SDK v3.4.
     * 'active' is what the device is actually running right now (set at
     * boot or last successful start_streaming). If the user did XPORT but
     * not HOTRESTART yet, 'active' still reflects the old mode — that's
     * what we want for the message. */
    uint8_t active = stream_mode_current_transport();
    bool crosses_wifi_boundary = (active == TRANSPORT_MODE_RAWTX) ||
                                 (val == TRANSPORT_MODE_RAWTX);

    esp_err_t err = config_set_transport_mode((uint8_t)val);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_transport_mode failed\r\n");
        at_send_error();
        return;
    }

    /* FIX (AUDIT-XPORT-CRASH): no on-the-fly WiFi mode switching.
     * - UDP(0) / TCP(1): both use the same AP-associated WiFi mode. The
     *   change is applied ONLY by AT+HOTRESTART (user explicit) or AT+RST.
     *   Server stop+start does NOT apply it - the stream continues on the
     *   old transport until the user explicitly requests the change.
     * - RAWTX(2): uses a fundamentally different WiFi mode (raw radio, no
     *   AP, fixed channel + 11B protocol + 11M rate). Switching to OR FROM
     *   RAWTX requires a full WiFi driver reinit, which on ESP8266 RTOS
     *   SDK v3.4 has a race condition that crashes. The user must do
     *   AT+RST (full reboot) to apply. */
    if (crosses_wifi_boundary)
    {
        at_send_data("+XPORT:set to %ld (saved, use AT+RST to apply)\r\n", val);
    }
    else
    {
        /* UDP <-> TCP: applied via AT+HOTRESTART or AT+RST. Server stop+start
         * does NOT apply it. */
        at_send_data("+XPORT:set to %ld (saved, use AT+HOTRESTART or AT+RST to apply)\r\n", val);
    }
    at_send_ok();
}

/* AT+TIMING? — вывести текущие задержки I2S RX.
 * AT+TIMING=sd,ws,bck — установить (0..3 каждый, сохраняется в NVS).
 * Применяется при следующем AT+HOTRESTART (или старте стрима). */
static void cmd_timing_query(void)
{
    device_config_t cfg;
    config_get_copy(&cfg);
    at_send_data("+TIMING:sd=%u,ws=%u,bck=%u\r\n",
                 (unsigned)cfg.i2s_timing_sd_delay,
                 (unsigned)cfg.i2s_timing_ws_delay,
                 (unsigned)cfg.i2s_timing_bck_delay);
    at_send_ok();
}

static void cmd_timing_set(const char *args)
{
    /* Format: sd,ws,bck - three integers 0..3 separated by commas. */
    /* FIX (M24): increase buf to 64 so "INT_MAX,INT_MAX,INT_MAX" (35 chars)
     * doesn't get silently truncated, which would make strtol parse a
     * truncated number for the third field. */
    char buf[64];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    char *endptr = NULL;
    long sd = strtol(p, &endptr, 10);
    if (endptr == p || *endptr != ',')
    {
        at_send_data("+ERR:format is sd,ws,bck (each 0-3)\r\n");
        at_send_error();
        return;
    }
    p = endptr + 1;
    long ws = strtol(p, &endptr, 10);
    if (endptr == p || *endptr != ',')
    {
        at_send_data("+ERR:format is sd,ws,bck (each 0-3)\r\n");
        at_send_error();
        return;
    }
    p = endptr + 1;
    long bck = strtol(p, &endptr, 10);
    if (endptr == p || *endptr != '\0')
    {
        at_send_data("+ERR:format is sd,ws,bck (each 0-3)\r\n");
        at_send_error();
        return;
    }

    if (sd < 0 || sd > I2S_TIMING_DELAY_MAX ||
        ws < 0 || ws > I2S_TIMING_DELAY_MAX ||
        bck < 0 || bck > I2S_TIMING_DELAY_MAX)
    {
        at_send_data("+ERR:each value must be 0-%d\r\n", I2S_TIMING_DELAY_MAX);
        at_send_error();
        return;
    }

    esp_err_t err = config_set_i2s_timing((uint8_t)sd, (uint8_t)ws, (uint8_t)bck);
    if (err != ESP_OK)
    {
        at_send_data("+ERR:config_set_i2s_timing failed\r\n");
        at_send_error();
        return;
    }

    at_send_data("+TIMING:set to sd=%ld,ws=%ld,bck=%ld (saved, use AT+HOTRESTART to apply)\r\n",
                 sd, ws, bck);
    at_send_ok();
}

#if BATTERY_ENABLED
static void cmd_batt_query(void)
{
    /* AT+BATT? or AT+BATT - returns last measured battery voltage + percent.
     * FIX (H13): use the cached value (battery_get_last_mv) updated by the
     * monitor task every BATT_CHECK_MIN minutes. The previous code called
     * battery_get_voltage_mv() which takes 15 samples x 50 ms = 750 ms,
     * blocking the AT task. During that time the UART RX buffer (512 bytes)
     * could overflow if the host sent another command. */
    uint32_t v_mv = battery_get_last_mv();
    if (v_mv == 0)
    {
        /* No cached value yet (boot just happened, monitor task hasn't run).
         * Fall back to a fresh read. */
        v_mv = battery_get_voltage_mv();
    }
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
     * AT+CH, AT+GAIN, AT+AGC, AT+CODEC, AT+WCH) take effect immediately
     * without a full ~1s reboot.
     *
     * AT+XPORT behavior:
     *   - UDP <-> TCP (XPORT=0/1): applied on the fly by HOTRESTART (both
     *     share the same AP-associated WiFi path, only sockets are swapped).
     *   - Any transition involving RAWTX (XPORT=2): NOT applied by HOTRESTART.
     *     start_streaming() keeps the OLD transport and logs a warning.
     *     AT+HOTRESTART still applies all OTHER settings (audio, WiFi ch).
     *     To actually switch to/from RAWTX, the user must do AT+RST.
     *
     * IMPORTANT: server-initiated stop+start (CMD_STOP + CONFIGURE) does NOT
     * apply a pending AT+XPORT change. Only AT+HOTRESTART (explicit user
     * action) or AT+RST (reboot) may apply it. This prevents the server
     * from triggering an unwanted transport switch when it does its routine
     * stop+start (e.g. after a TCP send error).
     *
     * ONLY works when the stream is currently active. If the stream is
     * already stopped (e.g., after CMD_STOP from server), this command
     * does nothing - the saved params will apply naturally on the next
     * stream start. This prevents HOTRESTART from overriding an
     * intentional stop. */
    if (!streaming_is_active())
    {
        /* FIX (L32): return ERROR instead of OK when the request did
         * nothing. The user expects HOTRESTART to actually restart; saying
         * OK is misleading. */
        at_send_data("+ERR:HOTRESTART:stream not active - params saved, "
                     "will apply on next start\r\n");
        at_send_error();
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
    at_send_data("+STATUS:frame_ms=%u (fixed)\r\n", streaming_get_frame_ms());
    at_send_data("+STATUS:bits_per_sample=%u\r\n", cfg.bits_per_sample);
    at_send_data("+STATUS:gain=%u (0=bypass, 32=+30dB)\r\n", (unsigned)cfg.gain);
    {
        const agc_preset_t *ap = (cfg.agc_mode < AGC_MODE_COUNT) ?
            &AGC_PRESETS[cfg.agc_mode] : &AGC_PRESETS[0];
        at_send_data("+STATUS:agc=%u (%s, attack=%u, release=%u)\r\n",
                     (unsigned)cfg.agc_mode, ap->name,
                     (unsigned)ap->attack, (unsigned)ap->release);
    }
    at_send_data("+STATUS:codec=%u (%s)\r\n", (unsigned)cfg.codec_mode,
                 cfg.codec_mode == CODEC_MODE_PCM ? "PCM" : "ADPCM");
    {
        const char *xname = "UDP";
        if (cfg.transport_mode == TRANSPORT_MODE_TCP) xname = "TCP";
        else if (cfg.transport_mode == TRANSPORT_MODE_RAWTX) xname = "Raw 802.11 TX";
        at_send_data("+STATUS:transport=%u (%s)\r\n",
                     (unsigned)cfg.transport_mode, xname);
    }
    if (cfg.transport_mode == TRANSPORT_MODE_RAWTX)
    {
        at_send_data("+STATUS:wifi_channel=%u\r\n", (unsigned)cfg.wifi_channel);
    }
    at_send_data("+STATUS:i2s_timing=sd=%u,ws=%u,bck=%u\r\n",
                 (unsigned)cfg.i2s_timing_sd_delay,
                 (unsigned)cfg.i2s_timing_ws_delay,
                 (unsigned)cfg.i2s_timing_bck_delay);
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
    {
        /* Bitrate depends on codec: ADPCM = 4 bits/sample, PCM = bits_per_sample
         * bits/sample. Previously always used 4, showing wrong bitrate for PCM. */
        unsigned bits_per_codec = (cfg.codec_mode == CODEC_MODE_PCM)
                                      ? cfg.bits_per_sample : 4;
        at_send_data("+STATUS:bitrate=%u\r\n",
                     (unsigned)(cfg.sample_rate * bits_per_codec *
                                channel_format_to_count(cfg.channel_format)));
    }
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

    /* FIX (GROK-8): apply the just-restored default WiFi credentials at
     * RUNTIME, not only to NVS. Without this, the device keeps running
     * on the OLD (user-configured) SSID until the next reboot, while NVS
     * now holds the factory-default SSID — memory WiFi != NVS WiFi, and
     * a subsequent NVS load would silently switch networks. This mirrors
     * what cmd_wifi does after config_set_wifi(): call wifi_sta_reconfigure
     * with the freshly-written SSID/password so the device actually
     * disconnects from the old AP and joins the new (factory-default) one. */
    device_config_t cfg;
    config_get_copy(&cfg);
    esp_err_t wifi_err = wifi_sta_reconfigure(cfg.wifi_ssid, cfg.wifi_password);
    /* Wipe the plaintext password from the stack copy. */
    memset(&cfg, 0, sizeof(cfg));

    /* FIX (L33): if a stream is currently active, request a restart so the
     * factory defaults actually take effect (audio config + transport). */
    if (streaming_is_active())
    {
        streaming_request_restart();
        if (wifi_err == ESP_OK)
            at_send_str("\r\n+FACTORY:defaults restored, WiFi reconnecting, stream restarting\r\n");
        else
            at_send_str("\r\n+FACTORY:defaults restored (WiFi reconfigure pending), stream restarting\r\n");
    }
    else
    {
        if (wifi_err == ESP_OK)
            at_send_str("\r\n+FACTORY:defaults restored, WiFi reconnecting\r\n");
        else
            at_send_str("\r\n+FACTORY:defaults restored (restart required - WiFi reconfigure failed)\r\n");
    }
    at_send_ok();
}

/* FIX (UART0/B9): AT+LOG implementation.
 *
 * Problem: AT commands and ESP_LOG output share UART0 on ESP8266 (UART1 is
 * TX-only on GPIO2, a boot strap pin). Log lines (e.g. "I (1234) main: ...")
 * mixed with AT responses (e.g. "OK\r\n+STATUS:...") break host-side AT
 * parsers that expect strict +CMD:/OK/ERROR framing.
 *
 * Solution: AT+LOG=0 silences all ESP_LOG output by setting every tag's
 * level to ESP_LOG_NONE. AT+LOG=1 restores the previous levels. The default
 * level is controlled by CONFIG_LOG_DEFAULT_LEVEL (menuconfig).
 *
 * This is the cheapest fix that doesn't require hardware changes (unlike
 * routing logs to UART1, which needs GPIO2 free). Production builds can
 * also set CONFIG_LOG_DEFAULT_LEVEL=0 to disable logs entirely at compile
 * time. */

/* Track whether logs are currently muted, so AT+LOG? can report the state
 * and so AT+LOG=1 can restore. We don't save the per-tag levels — we just
 * restore to CONFIG_LOG_DEFAULT_LEVEL (the compile-time default). */
static bool s_logs_muted = false;

static void cmd_log_set(const char *args)
{
    char *endptr = NULL;
    long val = strtol(args, &endptr, 10);
    if (endptr == args || *endptr != '\0')
    {
        at_send_data("+ERR:LOG must be 0 (mute) or 1 (unmute)\r\n");
        at_send_error();
        return;
    }

    if (val == 0)
    {
        /* Mute: set all tags to ESP_LOG_NONE. */
        esp_log_level_set("*", ESP_LOG_NONE);
        s_logs_muted = true;
        at_send_data("+LOG:logs muted (ESP_LOG_NONE)\r\n");
        at_send_ok();
    }
    else if (val == 1)
    {
        /* Unmute: restore to the compile-time default level. */
#if defined(CONFIG_LOG_DEFAULT_LEVEL)
        esp_log_level_set("*", (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
#else
        esp_log_level_set("*", ESP_LOG_INFO);
#endif
        s_logs_muted = false;
        at_send_data("+LOG:logs restored\r\n");
        at_send_ok();
    }
    else
    {
        at_send_data("+ERR:LOG must be 0 (mute) or 1 (unmute)\r\n");
        at_send_error();
    }
}

static void cmd_log_query(void)
{
    at_send_data("+LOG:%s\r\n", s_logs_muted ? "0 (muted)" : "1 (enabled)");
    at_send_ok();
}
