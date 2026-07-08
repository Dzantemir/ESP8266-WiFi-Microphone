/*
 * Raw 802.11 TX transport — broadcasts raw WiFi data frames directly into
 * the air on the current channel (no router/AP association needed).
 * Receiver must be in Monitor Mode on the same channel.
 *
 * Independent module — does not share state with udp_stream.c or tcp_stream.c.
 */

/* ---- System / SDK includes ---- */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_log.h"

/* ---- Project includes ---- */
#include "rawtx_stream.h"

static const char *TAG = "rawtx";

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

static bool s_ready = false;

/* Static frame buffer — single-threaded (only stream_task_fn calls
 * rawtx_stream_send via transport_send), so no mutex needed. Same approach
 * as tcp_stream.c. Avoids per-packet malloc/free which fragments the heap
 * at 66+ allocs/sec (15ms frames) — each alloc churns the ESP8266's small
 * heap and risks fragmentation over long streaming sessions.
 * Max frame = 24 (wifi hdr) + 1400 (payload) = 1424 bytes. */
static uint8_t s_frame_buf[WIFI_HDR_LEN + 1400];

esp_err_t rawtx_stream_init(void)
{
    if (s_ready)
    {
        rawtx_stream_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

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
     * rawtx_stream_send() so receivers can dedup. */
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

esp_err_t rawtx_stream_deinit(void)
{
    s_ready = false;
    return ESP_OK;
}

bool rawtx_stream_is_ready(void)
{
    return s_ready;
}

esp_err_t rawtx_stream_send(const uint8_t *data, size_t len)
{
    if (!data || !len || !s_ready)
        return ESP_ERR_INVALID_ARG;

    /* Raw 802.11 TX: prepend MAC header and send via esp_wifi_80211_tx.
     * Buffer: [wifi_hdr 24B][data len bytes]
     * Uses static s_frame_buf (no per-packet malloc - see comment above). */
    /* FIX (L11): reject oversized payloads instead of silently truncating.
     * Silent truncation drops the tail of the audio packet - the receiver
     * gets a short ADPCM frame and may decode garbage for the missing
     * nibbles. main.c's MTU guard should prevent this, but a bug there
     * would be masked here. */
    if (len > 1400)
    {
        ESP_LOGW("rawtx", "payload %u > 1400, rejecting", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total_len = WIFI_HDR_LEN + len;

    memcpy(s_frame_buf, s_wifi_hdr, WIFI_HDR_LEN);

    /* Update Sequence Control (bytes 22-23) for this packet.
     * Layout (little-endian): 12-bit seq num + 4-bit frag num.
     *   byte 22 = (frag << 0) | (seq_lo << 4)  - frag=0, seq_lo = seq & 0xF
     *   byte 23 = seq_hi = (seq >> 4) & 0xFF
     * seq wraps at 4096 (12-bit). We never fragment, so frag stays 0. */
    uint16_t seq = s_wifi_seq++;
    s_frame_buf[22] = (uint8_t)(seq & 0x0F) << 4;   /* frag=0 | seq_lo */
    s_frame_buf[23] = (uint8_t)((seq >> 4) & 0xFF); /* seq_hi */

    memcpy(s_frame_buf + WIFI_HDR_LEN, data, len);

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, s_frame_buf, total_len, false);

    if (err != ESP_OK)
    {
        /* FIX (L12): return the actual error so the caller can log it
         * (e.g. ESP_ERR_WIFI_IF meaning "WiFi not started"). */
        return err;
    }
    return ESP_OK;
}
