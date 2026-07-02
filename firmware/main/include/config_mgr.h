#ifndef CONFIG_MGR_H
#define CONFIG_MGR_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Configuration manager with NVS persistence.
 *
 * Stores WiFi, service port, and audio I2S settings in flash via NVS.
 * Sample rate, channels, I2S bit depth, and comm format are runtime-
 * configurable (AT+RATE, AT+CH, AT+BITS, AT+FMT); frame duration and
 * codec are fixed in board_config.h.
 *
 * Thread safety: a mutex protects the config struct. Get operations
 * return a copy via config_get_copy(). Set operations lock the mutex,
 * modify, and mark dirty.
 */

/* I2S communication format */
#define I2S_CFMT_PHILIPS  0   /* Standard I2S (MSB shift) */
#define I2S_CFMT_LSB      1   /* Left-justified (no shift) */

/* Channel format (matches board_config.h I2S_CHANNEL_FORMAT) */
#define I2S_CHFMT_LEFT    4   /* ONLY_LEFT (mono, L/R=GND) */
#define I2S_CHFMT_RIGHT   3   /* ONLY_RIGHT (mono, L/R=VDD) */
#define I2S_CHFMT_STEREO  0   /* RIGHT_LEFT (stereo) */

typedef struct {
    char     wifi_ssid[33];
    char     wifi_password[65];
    char     hostname[33];         /* DHCP hostname (1-32 chars) */
    uint8_t  tx_power;          /* dBm */
    uint16_t svc_port;          /* EASSP UDP port */
    uint32_t sample_rate;       /* Hz */
    uint8_t  bits_per_sample;   /* 16 or 24 */
    uint8_t  comm_format;       /* I2S_CFMT_* */
    uint8_t  channel_format;    /* I2S_CHFMT_* */
    uint8_t  gain;              /* Fixed gain (0=bypass, 1-64), used when AGC off */
    uint8_t  agc_mode;          /* 0=OFF (fixed gain), 1=LOW, 2=MEDIUM, 3=HIGH */
    uint8_t  codec_mode;        /* 0=ADPCM, 1=PCM (AT+CODEC) */
    uint8_t  wifi_channel;      /* 1-13 (AT+WCH, used in RawTX transport) */
    uint8_t  transport_mode;    /* 0=UDP, 1=TCP, 2=Raw 802.11 TX (AT+XPORT) */
    uint8_t  i2s_timing_sd_delay;  /* 0..3, AT+TIMING sd_in_delay */
    uint8_t  i2s_timing_ws_delay;  /* 0..3, AT+TIMING ws_in_delay */
    uint8_t  i2s_timing_bck_delay; /* 0..3, AT+TIMING bck_in_delay */
} device_config_t;

/* Initialize config manager and load from NVS */
esp_err_t config_mgr_init(void);

/* Get a thread-safe copy of the current config */
void config_get_copy(device_config_t *cfg);

/* Setters - each saves to NVS immediately */
esp_err_t config_set_wifi(const char *ssid, const char *password);
esp_err_t config_set_hostname(const char *hostname);
esp_err_t config_set_tx_power(uint8_t tx_power);
esp_err_t config_set_svc_port(uint16_t port);
esp_err_t config_set_sample_rate(uint32_t rate);
esp_err_t config_set_bits_per_sample(uint8_t bits);
esp_err_t config_set_comm_format(uint8_t fmt);
esp_err_t config_set_channel_format(uint8_t fmt);
esp_err_t config_set_gain(uint8_t gain);     /* 0-64 (0 = bypass, no gain) */
esp_err_t config_set_agc_mode(uint8_t mode); /* 0=OFF, 1=LOW, 2=MEDIUM, 3=HIGH */
esp_err_t config_set_codec_mode(uint8_t mode); /* 0=ADPCM, 1=PCM */
esp_err_t config_set_wifi_channel(uint8_t ch); /* 1-13 */
esp_err_t config_set_transport_mode(uint8_t mode); /* 0=UDP, 1=TCP, 2=RawTX */
esp_err_t config_set_i2s_timing(uint8_t sd_delay, uint8_t ws_delay, uint8_t bck_delay); /* each 0..3 */

/* Factory reset - restores defaults from board_config.h */
esp_err_t config_factory_reset(void);

/* Helper: channel_format -> channel count (1 or 2) */
static inline uint8_t channel_format_to_count(uint8_t fmt)
{
    return (fmt == I2S_CHFMT_STEREO) ? 2 : 1;
}

#endif /* CONFIG_MGR_H */
