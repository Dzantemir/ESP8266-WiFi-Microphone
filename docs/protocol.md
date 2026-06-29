# Communication Protocol Specification

## EASSP - ESP WiFi Microphone Service Protocol

### Overview

EASSP is a binary UDP protocol for device discovery, streaming control, and status reporting between ESP8266 audio streamer devices and the EASSP Server (Windows receiver).

### Ports

| Port  | Direction         | Purpose                        |
|-------|-------------------|--------------------------------|
| 3950  | Server -> Device  | DISCOVER, CONFIGURE, STOP      |
| 3950  | Device -> Server  | INFO (responses, announcements)|
| 5000+ | Device -> Server  | Audio data stream              |

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

Payload (33 bytes):
```
Offset  Field           Type     Description
0       status          u8       0=IDLE, 1=STREAMING, 2=ERROR
1       codec_id        u8       5=ADPCM, 6=PCM
2       error           u8       Error code (0=none)
3       channels        u8       1 or 2
4       sample_rate     u32      Hz (e.g., 48000)
8       frame_ms        u8       Frame duration in ms
9       mac[6]          u8[6]    Device MAC address
15      packets_sent    u32      Total packets sent since stream start
19      free_heap       u32      Free heap in bytes
23      wifi_rssi       i8       WiFi RSSI in dBm
24      firmware[8]     char[8]  Firmware version string
32      bits_per_sample u8       16 or 24
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

Audio data is sent as UDP packets with a 16-byte header followed by the audio payload.

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

### Frame Duration

Frame duration (`frame_ms`) is computed adaptively by the ESP8266 to satisfy:
- Even sample count (ADPCM requirement: 2 samples per byte)
- DMA buffer minimum (32 samples per quarter-frame)
- UDP MTU limit (packet size <= 1400 bytes)

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
