# Communication Protocol Specification

## EASSP - ESP WiFi Microphone Service Protocol

### Overview

EASSP is a binary UDP protocol for device discovery, streaming control, and status reporting between ESP8266 audio streamer devices and the EASSP Server (Windows receiver).

### Transports

Audio data can be sent via three transports, selectable at runtime via `AT+XPORT`:

| Transport | Discovery | Audio Path | Framing |
|-----------|-----------|------------|---------|
| UDP (default) | UDP 3950 | UDP datagrams (port 5000+) | Each datagram = 1 frame (boundaries preserved by UDP) |
| TCP | UDP 3950 | TCP connection (ESP = listener, server connects) | Length-prefix: `[u16 len BE][frame]` |
| Raw 802.11 TX | None (auto-start) | Raw WiFi frames (broadcast) | Each frame = 1 802.11 data frame |

Discovery (DISCOVER/CONFIGURE/STOP/INFO) always uses UDP port 3950, even in TCP mode.

### Ports

| Port  | Direction         | Purpose                        |
|-------|-------------------|--------------------------------|
| 3950  | Server -> Device  | DISCOVER, CONFIGURE, STOP      |
| 3950  | Device -> Server  | INFO (responses, announcements)|
| 5000+ | Device -> Server  | Audio data stream (UDP) or TCP listener |

### Packet Format

All EASSP packets share an 8-byte header:

```
Offset  Field           Type     Description
0       magic[2]        u8[2]    0xEA, 0x55
2       version         u8       Protocol version (1)
3       cmd             u8       Command byte (see below)
4       seq             u16      Sequence number
6       payload_len     u16      Payload length after header
```

### Commands

#### DISCOVER (0x01) - Server -> Device
Sent by the server every 3 seconds as a heartbeat while streaming, and for discovery when idle.

Payload: none (payload_len = 0)

#### CONFIGURE (0x02) - Server -> Device
Tells the device to start streaming audio to the server.

Payload (2 bytes):
```
Offset  Field           Type     Description
0       stream_port     u16      Port to send audio data to (network byte order)
```

#### STOP (0x03) - Server -> Device
Tells the device to stop streaming immediately (no watchdog timeout wait).

Payload: none (payload_len = 0)

#### INFO (0x81) - Device -> Server
Status response sent in reply to DISCOVER or CONFIGURE, and periodically while streaming.

Payload (58 bytes, v2.2):
```
Offset  Field             Type     Description
0       status            u8       0=IDLE, 1=STREAMING, 2=ERROR
1       codec_id          u8       5=ADPCM, 6=PCM
2       error             u8       Error code (0=none)
3       channels          u8       1 or 2
4       sample_rate       u32      Hz (e.g., 48000)
8       frame_ms          u8       Frame duration in ms
9       mac[6]            u8[6]    Device MAC address
15      packets_sent      u32      Total packets sent since stream start
19      free_heap         u32      Free heap in bytes
23      wifi_rssi         i8       WiFi RSSI in dBm
24      firmware[8]       char[8]  Firmware version string
32      bits_per_sample   u8       16 or 24
33      transport_mode    u8       v2.1: 0=UDP, 1=TCP, 2=Raw 802.11 TX
34      hostname[24]      char[24] v2.2: DHCP hostname (NUL-terminated, max 23 chars)
```

### Error Codes

| Code | Name     | Description                        |
|------|----------|------------------------------------|
| 0    | NONE     | No error                           |
| 1    | MEMORY   | Memory allocation failure          |
| 2    | I2S      | I2S capture error                  |
| 3    | CODEC    | ADPCM encoder error                |
| 4    | NETWORK  | UDP/WiFi send failure              |
| 5    | WATCHDOG | Server watchdog timeout (auto-stop)|
| 6    | CONFIG   | Configuration error                |

## Audio Packet Format

Audio data is sent as packets with a 16-byte header followed by the audio payload.
In UDP mode, each packet is a single datagram. In TCP mode, each packet is
prefixed with a 2-byte length (see TCP Framing below). In Raw 802.11 TX mode,
each packet is a single 802.11 data frame.

### Header (16 bytes)

```
Offset  Field           Type     Description
0       seq_num         u16      Sequence number (wraps at 65535)
2       timestamp_ms    u32      Frame timestamp in milliseconds
6       codec           u8       5=ADPCM (DVI4), 6=PCM
7       sample_rate     u8       Enum: 0=8k, 1=11k, 2=16k, 3=22k, 4=32k, 5=44k, 6=48k
8       channels        u8       1=mono, 2=stereo
9       frame_ms        u8       Frame duration in milliseconds
10      bitrate         u32      Audio bitrate in bps
14      bits            u16      Bits per sample (16 or 24)
```

### ADPCM Payload (codec=5)

DVI4 IMA ADPCM (RFC 3551) format:

**Mono:**
```
[DVI4 header: predictor(2) + step_index(1) + reserved(1)] [ADPCM nibbles...]
```

**Stereo:**
```
[Left DVI4 header: 4 bytes]  [Left ADPCM nibbles...]
[Right DVI4 header: 4 bytes] [Right ADPCM nibbles...]
```

Nibble packing: first sample in HIGH nibble, second sample in LOW nibble (RFC 3551 DVI4 convention).

### PCM Payload (codec=6)

Raw signed PCM samples, little-endian:

**16-bit:** 2 bytes per sample per channel (interleaved for stereo)
**24-bit:** 3 bytes per sample per channel (low 3 bytes of int32, sign-extended)

### TCP Framing

When `transport_mode=1` (TCP), audio frames are sent over a TCP connection.
Since TCP is a stream protocol (no message boundaries), each frame is
prefixed with a 2-byte big-endian length:

```
[u16 length (big-endian)] [16-byte header] [payload]
        2 bytes                16 bytes       N bytes

length = 16 + payload_len (max 1416, fits in u16)
```

The ESP8266 opens a listening socket on `stream_port` (from CONFIGURE).
The server connects to `ESP_IP:stream_port` and reads:
1. 2 bytes → decode as big-endian u16 `length`
2. `length` bytes → parse as audio packet (same format as UDP)

The server reconnects automatically on disconnect.

**Backpressure:** The ESP uses blocking `send()` with `SO_SNDTIMEO` (configurable
via menuconfig, default 2000 ms). If the server stops reading, `send()` blocks
→ the ADPCM queue fills → I2S drops frames (natural backpressure, same as UDP).
On timeout, the connection is closed and the accept task picks up a new connect.

### Frame Duration

Frame duration (`frame_ms`) is computed adaptively by the ESP8266 to satisfy:
- Even sample count (ADPCM requirement: 2 samples per byte)
- DMA buffer minimum (32 samples per quarter-frame)
- UDP MTU limit (packet size <= 1400 bytes)
- DMA alignment: `samples_per_frame` is rounded down to a multiple of 8
  (16-bit) or 4 (24-bit) to ensure `blocksize = dma_buf_len * sample_size`
  is a multiple of 4 (ESP8266 SLC word size). Without this, one sample is
  lost per DMA buffer boundary → periodic clicks.

Preferred durations (largest that fits): 60, 50, 40, 30, 25, 20, 15, 10, 5 ms

### On-the-Fly Format Switching

The receiver detects format changes by comparing packet header fields (codec, sample_rate, channels, bits) with the currently open WaveOut format. On mismatch:
1. WaveOut is reset, unprepared, and closed
2. New WAVEFORMATEX is built from the packet header
3. New WaveOut is opened (with 24->16 fallback if sound card rejects 24-bit)
4. ADPCM decoder state is reset (predictor=0, step_index=0)
5. Sequence number tracking is reset
6. g_Devs is updated for UI display
7. Audio continues without user intervention

### Sequence Number Handling

- 16-bit sequence numbers wrap at 65535 -> 0
- Loss detection uses modular arithmetic: `diff = (seq - lastSeq - 1) & 0xFFFF`
  - `diff = 0`: in order, no loss
  - `diff < 32768`: lost packets = diff
  - `diff >= 32768`: out-of-order or duplicate (discarded)
