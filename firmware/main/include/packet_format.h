#ifndef PACKET_FORMAT_H
#define PACKET_FORMAT_H

#include <stdint.h>

/*
 * UDP packet header (16 bytes, packed).
 *
 *  Offset  Field             Type       Description
 *  0       seq_num           uint16     Sequence number (wraps at 65535)
 *  2       timestamp_ms      uint32     Frame timestamp in milliseconds
 *  6       codec             uint8      Codec ID (5 = DVI4 IMA ADPCM, 6 = PCM)
 *  7       sample_rate_enum  uint8      Sample rate enum (0..6 -> 8k..48k)
 *  8       channels          uint8      Number of channels (1 = mono, 2 = stereo)
 *  9       frame_ms          uint8      Frame duration in ms
 *  10      bitrate           uint32     Bitrate in bps
 *  14      bits              uint16     Bits per sample (16 or 24)
 *
 * The `bits` field carries the I2S bit depth (16 or 24). For ADPCM it is
 * always 16 (the ESP dithers 24->16 before encoding). For PCM it reflects
 * the actual I2S config (16 or 24). The receiver uses this field to detect
 * on-the-fly format changes and reopen WaveOut with the correct format.
 *
 * After this header, the payload is a DVI4 block (RFC 3551) for ADPCM, or
 * raw signed PCM samples for PCM mode.
 *
 * Multi-byte fields are sent in CPU native byte order (little-endian on
 * Xtensa LX106). Both ESP and the PowerBASIC server are little-endian,
 * so no htons/htonl is needed for this private protocol.
 */

typedef struct __attribute__((packed)) pkt_header {
    uint16_t seq_num;
    uint32_t timestamp_ms;
    uint8_t  codec;
    uint8_t  sample_rate_enum;
    uint8_t  channels;
    uint8_t  frame_ms;
    uint32_t bitrate;
    uint16_t bits;
} pkt_header_t;

_Static_assert(sizeof(pkt_header_t) == 16, "pkt_header_t must be exactly 16 bytes");

static inline void pkt_header_init(pkt_header_t *hdr,
                                   uint16_t seq_num,
                                   uint32_t timestamp_ms,
                                   uint8_t  codec,
                                   uint8_t  sample_rate_enum,
                                   uint8_t  channels,
                                   uint8_t  frame_ms,
                                   uint32_t bitrate,
                                   uint16_t bits)
{
    hdr->seq_num          = seq_num;
    hdr->timestamp_ms     = timestamp_ms;
    hdr->codec            = codec;
    hdr->sample_rate_enum = sample_rate_enum;
    hdr->channels         = channels;
    hdr->frame_ms         = frame_ms;
    hdr->bitrate          = bitrate;
    hdr->bits             = bits;
}

#endif /* PACKET_FORMAT_H */
