/*
 * Stream sender - sends encoded audio packets to the receiver.
 *
 * Two modes:
 * 1. UDP mode (default): uses a standard UDP socket (lwIP).
 * 2. Raw 802.11 TX mode: uses esp_wifi_80211_tx to broadcast raw
 *    WiFi data frames directly into the air on the current channel.
 *    No router connection needed. Receiver must be in Monitor Mode.
 */

#include "udp_stream.h"

#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"
#include "esp_wifi.h"
#include "esp_log.h"

#include "board_config.h"

#ifndef IPTOS_DSCP_EF
#define IPTOS_DSCP_EF 0xB8
#endif

static const char *TAG = "stream";

static int s_sock = -1;
static struct sockaddr_in s_dest;
static bool s_ready = false;
static bool s_raw_tx = false;

/* 802.11 MAC header for Data frames (24 bytes).
 * Layout (IEEE 802.11-2016, §9.2.4):
 *   bytes 0-1:   Frame Control
 *   bytes 2-3:   Duration/ID
 *   bytes 4-9:   Address 1 (Receiver / DA)
 *   bytes 10-15: Address 2 (Transmitter / SA)
 *   bytes 16-21: Address 3 (BSSID)
 *   bytes 22-23: Sequence Control (12-bit seq num + 4-bit frag num)
 *
 * For independent raw TX (no AP association):
 *   ToDS=0, FromDS=0, all addresses broadcast except SA (our MAC). */
#define WIFI_HDR_LEN 24
static uint8_t s_wifi_hdr[WIFI_HDR_LEN];

/* Sequence number for 802.11 frames (12-bit, wraps at 4096).
 * Incremented per transmitted MSDU to allow receiver dedup. */
static uint16_t s_wifi_seq = 0;

esp_err_t udp_stream_init(uint32_t host_ip, uint16_t host_port)
{
    if (s_ready)
    {
        udp_stream_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_raw_tx = false;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        ESP_LOGE(TAG, "socket: errno=%d", errno);
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = UDP_SEND_TIMEOUT_MS / 1000,
        .tv_usec = (UDP_SEND_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(s_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Set TOS (Type of Service) to Expedited Forwarding (Voice). */

    int tos = IPTOS_DSCP_EF;
    setsockopt(s_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons(host_port);
    s_dest.sin_addr.s_addr = host_ip; /* already network byte order */

    ESP_LOGI(TAG, "UDP -> %d.%d.%d.%d:%u",
             (int)(host_ip & 0xFF), (int)((host_ip >> 8) & 0xFF),
             (int)((host_ip >> 16) & 0xFF), (int)((host_ip >> 24) & 0xFF),
             (unsigned)host_port);

    s_ready = true;
    return ESP_OK;
}

esp_err_t udp_stream_init_raw(void)
{
    if (s_ready)
    {
        udp_stream_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_raw_tx = true;

    /* Build 802.11 MAC header for a Data frame sent as an independent
     * station (not associated with any AP).
     *
     * Frame Control (2 bytes, little-endian):
     *   byte 0 = 0x08: Protocol=0, Type=2 (Data), Subtype=0
     *   byte 1 = 0x00: ToDS=0, FromDS=0 (independent frame, no DS)
     *
     *   IMPORTANT: byte 1 bit 0 is ToDS. Setting 0x01 means ToDS=1
     *   ("frame going TO the distribution system" = STA->AP), which is
     *   only valid when associated with an AP. For raw broadcast TX with
     *   no AP, ToDS MUST be 0. Using ToDS=1 produces malformed frames
     *   that monitors may drop or flag as invalid.
     *
     * Addressing (ToDS=0, FromDS=0):
     *   addr1 (DA)    = broadcast (we want everyone to receive it)
     *   addr2 (SA)    = our MAC (so receivers know who sent it)
     *   addr3 (BSSID) = broadcast (we're not in any BSS)
     *
     * Sequence Control: initialized to 0, incremented per packet in
     * udp_stream_send() so receivers can dedup. */
    memset(s_wifi_hdr, 0, sizeof(s_wifi_hdr));
    s_wifi_hdr[0] = 0x08; /* FC byte 0: Data frame (type=2, subtype=0) */
    s_wifi_hdr[1] = 0x00; /* FC byte 1: ToDS=0, FromDS=0 (independent TX) */

    /* addr1 (bytes 4-9): Receiver Address = Broadcast */
    memset(&s_wifi_hdr[4], 0xFF, 6);

    /* addr2 (bytes 10-15): Transmitter Address = our MAC */
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
    {
        memcpy(&s_wifi_hdr[10], mac, 6);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to get MAC, using default");
        uint8_t def_mac[6] = {0x9C, 0x9C, 0x1F, 0x8D, 0xAA, 0xC5};
        memcpy(&s_wifi_hdr[10], def_mac, 6);
    }

    /* addr3 (bytes 16-21): BSSID = Broadcast.
     * For ToDS=0/FromDS=0, addr3 is the BSSID. Since we're not in any
     * BSS (no AP association), use broadcast - not our MAC. */
    memset(&s_wifi_hdr[16], 0xFF, 6);

    /* bytes 22-23: Sequence Control - starts at 0, incremented per packet. */
    s_wifi_seq = 0;

    ESP_LOGI(TAG, "Raw 802.11 TX mode active (broadcast, ToDS=0, FromDS=0)");

    s_ready = true;
    return ESP_OK;
}

esp_err_t udp_stream_deinit(void)
{
    if (s_sock >= 0)
    {
        close(s_sock);
        s_sock = -1;
    }
    s_raw_tx = false;
    s_ready = false;
    return ESP_OK;
}

bool udp_stream_is_ready(void) { return s_ready; }

esp_err_t udp_stream_send(const uint8_t *data, size_t len)
{
    if (!data || !len || !s_ready)
        return ESP_ERR_INVALID_ARG;

    if (s_raw_tx)
    {
        /* Raw 802.11 TX: prepend MAC header and send via esp_wifi_80211_tx.
         * Buffer: [wifi_hdr 24B][data len bytes]
         * Allocate on heap - task stack (1024B) cannot hold 1424B buffer. */
        if (len > 1400)
            len = 1400;

        size_t total_len = WIFI_HDR_LEN + len;
        uint8_t *buf = malloc(total_len);
        if (!buf)
            return ESP_ERR_NO_MEM;

        memcpy(buf, s_wifi_hdr, WIFI_HDR_LEN);

        /* Update Sequence Control (bytes 22-23) for this packet.
         * Layout (little-endian): 12-bit seq num + 4-bit frag num.
         *   byte 22 = (frag << 0) | (seq_lo << 4)  - frag=0, seq_lo = seq & 0xF
         *   byte 23 = seq_hi = (seq >> 4) & 0xFF
         * seq wraps at 4096 (12-bit). We never fragment, so frag stays 0. */
        uint16_t seq = s_wifi_seq++;
        buf[22] = (uint8_t)(seq & 0x0F) << 4;   /* frag=0 | seq_lo */
        buf[23] = (uint8_t)((seq >> 4) & 0xFF); /* seq_hi */

        memcpy(buf + WIFI_HDR_LEN, data, len);

        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, buf, total_len, false);

        free(buf);

        if (err != ESP_OK)
        {
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    else
    {
        /* Standard UDP mode */
        if (sendto(s_sock, data, len, 0,
                   (struct sockaddr *)&s_dest, sizeof(s_dest)) < 0)
            return ESP_FAIL;
        return ESP_OK;
    }
}
