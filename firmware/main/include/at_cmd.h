#ifndef AT_CMD_H
#define AT_CMD_H

#include "esp_err.h"

/*
 * AT command parser on UART0.
 *
 * Commands (terminated with \r\n):
 *   AT                 - test connection
 *   AT+RST             - restart
 *   AT+GMR             - show version
 *   AT+HELP            - command list
 *   AT+WIFI?           - show WiFi settings
 *   AT+WIFI=ssid,pwd   - set WiFi (auto-save, immediate)
 *   AT+PORT?           - show service port
 *   AT+PORT=n          - set service port (auto-save, restart required)
 *   AT+TXPWR?          - show WiFi TX power
 *   AT+TXPWR=n         - set TX power 0-20 dBm (auto-save, immediate)
 *   AT+RATE?           - show sample rate
 *   AT+RATE=n          - set 8000/11025/16000/22050/32000/44100/48000
 *   AT+BITS?           - show I2S bits (16 or 24)
 *   AT+BITS=16|24      - set bits (auto-save, next stream)
 *   AT+FMT?            - show I2S comm format
 *   AT+FMT=0|1         - 0=Philips 1=LSB (auto-save, next stream)
 *   AT+CH?             - show channel format
 *   AT+CH=0|1|2        - 0=left 1=right 2=stereo (auto-save, next stream)
 *   AT+STATUS          - full device status
 *   AT+FACTORY         - factory reset (restart required)
 */

esp_err_t at_cmd_init(void);

#endif /* AT_CMD_H */
