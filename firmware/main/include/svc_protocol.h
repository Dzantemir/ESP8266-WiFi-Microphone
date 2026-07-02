#ifndef SVC_PROTOCOL_H
#define SVC_PROTOCOL_H

#include <stdint.h>
#include "board_config.h"

/*
 * ESP8266 WiFi Microphone Service Protocol (EASSP)
 *
 * Binary protocol for device discovery and streaming control
 * over UDP service port (default 3950).
 *
 * Packet format:
 *   [Header (8 bytes)] [Payload (variable)]
 *
 * Header:
 *   Offset  Field         Type     Description
 *   0       magic         uint8[2] 0xEA, 0x55
 *   2       version       uint8    Protocol version (1)
 *   3       cmd           uint8    Command byte
 *   4       seq           uint16   Sequence number
 *   6       payload_len   uint16   Length of payload after header
 *
 * AUDIO PARAMETERS:
 *   Sample rate: NVS-configurable via AT+RATE (default 16 kHz)
 *   Frame duration: computed at runtime from I2S params (typically 20 ms)
 *   Channels: NVS-configurable via AT+CH (default mono)
 *   Codec: DVI4 IMA ADPCM (RFC 3551)
 *   Bitrate: sample_rate * 4 * channels bps
 */

/* Magic bytes */
#define EASSP_MAGIC0     0xEA
#define EASSP_MAGIC1     0x55
#define EASSP_VER        1

/* Header size */
#define SVC_HEADER_SIZE  8

/* Commands (server -> device) */
#define SVC_CMD_DISCOVER    0x01
#define SVC_CMD_CONFIGURE   0x02
#define SVC_CMD_STOP        0x03   /* Explicit stop - no payload, immediate stop */

/* Commands (device -> server) */
#define SVC_CMD_INFO        0x81

/* Status codes (in INFO payload) */
#define SVC_STATUS_IDLE     0
#define SVC_STATUS_STREAMING 1
#define SVC_STATUS_ERROR    2

/* Error codes (in INFO payload when status == ERROR) */
#define SVC_ERR_NONE        0
#define SVC_ERR_MEMORY      1
#define SVC_ERR_I2S         2
#define SVC_ERR_CODEC       3
#define SVC_ERR_NETWORK     4
#define SVC_ERR_WATCHDOG    5
#define SVC_ERR_CONFIG      6

/* Payload sizes */
#define INFO_PAYLOAD_SZ     34   /* v2.1: +1 byte transport_mode (was 33) */
#define CFG_PAYLOAD_SZ      2

/* Header structure (8 bytes, packed) */
typedef struct __attribute__((packed)) svc_header {
    uint8_t  magic[2];      /* 0xEA, 0x55 */
    uint8_t  version;       /* 1 */
    uint8_t  cmd;           /* SVC_CMD_* */
    uint16_t seq;           /* sequence number */
    uint16_t payload_len;   /* length of payload */
} svc_header_t;

_Static_assert(sizeof(svc_header_t) == SVC_HEADER_SIZE, "svc_header_t must be 8 bytes");

/* INFO payload (34 bytes, packed) - sent in response to DISCOVER/CONFIGURE.
 * v2.1: added transport_mode (1 byte) at the end. Old receivers (expecting
 * 33 bytes) ignore the extra byte — backward compatible over UDP datagram. */
typedef struct __attribute__((packed)) svc_info_payload {
    uint8_t  status;        /* SVC_STATUS_* */
    uint8_t  codec_id;      /* CODEC_ID_ADPCM = 5, CODEC_ID_PCM = 6 */
    uint8_t  error;         /* SVC_ERR_* (0 = none) */
    uint8_t  channels;      /* 1 or 2 */
    uint32_t sample_rate;   /* Hz (e.g., 16000) */
    uint8_t  frame_ms;      /* frame duration */
    uint8_t  mac[6];        /* device MAC address */
    uint32_t packets_sent;  /* since stream start */
    uint32_t free_heap;     /* current free heap */
    int8_t   wifi_rssi;     /* dBm */
    char     firmware[8];   /* FIRMWARE_VERSION */
    uint8_t  bits_per_sample; /* 16 or 24 (from NVS config) */
    uint8_t  transport_mode;  /* v2.1: 0=UDP, 1=TCP, 2=Raw 802.11 TX */
} svc_info_payload_t;

_Static_assert(sizeof(svc_info_payload_t) == INFO_PAYLOAD_SZ, "svc_info_payload_t must be 34 bytes");

/* CONFIGURE payload (2 bytes, packed) - server -> device
 *
 * PROTOCOL PRINCIPLE: The receiver (server) NEVER dictates audio parameters
 * to the device. CONFIGURE only tells the device WHERE to send audio
 * (stream_port). The device is the audio authority - it streams exactly
 * what is in its NVS config (set by AT+CH). The server learns the actual
 * format from INFO packets and adapts its playback (WaveOut) accordingly.
 * If the format is unsupported, the stream is rejected.
 *
 * The legacy `channels` field was removed - it was always ignored by the
 * device anyway, but sending it violated the "receiver doesn't configure
 * the device" principle. */
typedef struct __attribute__((packed)) svc_configure_payload {
    uint16_t stream_port;   /* network byte order (htons) */
} svc_configure_payload_t;

_Static_assert(sizeof(svc_configure_payload_t) == CFG_PAYLOAD_SZ, "svc_configure_payload_t must be 2 bytes");

static inline void svc_header_init(svc_header_t *hdr, uint8_t cmd, uint16_t seq, uint16_t payload_len)
{
    hdr->magic[0]    = EASSP_MAGIC0;
    hdr->magic[1]    = EASSP_MAGIC1;
    hdr->version     = EASSP_VER;
    hdr->cmd         = cmd;
    hdr->seq         = seq;
    hdr->payload_len = payload_len;
}

#endif /* SVC_PROTOCOL_H */
