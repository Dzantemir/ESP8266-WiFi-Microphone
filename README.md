# ESP8266 WiFi Microphone

Real-time audio streaming from an ESP8266 to a Windows PC over WiFi with professional-grade audio quality.

```
INMP441 Microphone ──I2S──> ESP8266 ──WiFi──> PC (EASSP Server) ──WaveOut──> Speakers
                          24-bit ADC           UDP / Raw 802.11          WAV Recording
                          TPDF Dither          ADPCM / PCM               Multi-device
                          AGC + Gain           8-48 kHz                  On-the-fly format switch
```

## Overview

This project turns an ESP8266 into a high-quality wireless microphone. It captures audio from an I2S MEMS microphone (INMP441), processes it with professional audio techniques (TPDF dithering, AGC), encodes it with IMA ADPCM or raw PCM, and streams it to a Windows PC in real time.

The Windows receiver (EASSP Server) discovers ESP8266 devices on the network, decodes the audio, plays it through the sound card, and can record to WAV files.

### Key Features

- **Two WiFi modes**: Standard UDP (via router) or Raw 802.11 TX (broadcast, no router needed)
- **Two codecs**: IMA ADPCM (DVI4/RFC 3551, ~32 kbps) or raw PCM (16/24-bit, lossless)
- **Professional audio pipeline**: 24-bit I2S capture, TPDF dithering for 24->16-bit reduction, software AGC with 4 presets
- **On-the-fly format switching**: Change sample rate, channels, bit depth, and codec via AT commands without rebooting - the receiver adapts automatically
- **Multi-device support**: Stream from up to 16 ESP8266 devices simultaneously
- **WAV recording**: Built-in recorder with 1 GB auto-split, correct headers for all formats
- **Configurable**: 8-48 kHz sample rate, mono/stereo, 16/24-bit, all settings persist in NVS
- **AT command interface**: Full configuration over UART (115200 baud)

## Project Structure

```
esp8266-wifi-microphone/
├── README.md                  # This file
├── LICENSE
├── firmware/                  # ESP8266 firmware (ESP8266 RTOS SDK)
│   ├── CMakeLists.txt
│   ├── sdkconfig
│   ├── i2s_patched.c          # Patched I2S driver (copy to SDK)
│   └── main/
│       ├── CMakeLists.txt
│       ├── Kconfig.projbuild
│       ├── main.c             # Main application + pipeline
│       ├── stream_mode.c      # UDP/Raw TX mode abstraction
│       ├── wifi_sta.c         # WiFi STA + Raw TX init
│       ├── udp_stream.c       # UDP socket / Raw 802.11 TX
│       ├── at_cmd.c           # AT command interface
│       ├── config_mgr.c       # NVS configuration manager
│       ├── svc_port.c         # EASSP service port (discovery/control)
│       ├── i2s_capture.c      # I2S capture driver wrapper
│       ├── adpcm_encoder.c    # IMA ADPCM encoder (IRAM-optimized)
│       ├── tpdf_dither.c      # TPDF dithering (24->16 bit)
│       ├── battery.c          # Battery monitoring (optional)
│       └── include/           # Header files
├── server/                    # Windows receiver (PowerBASIC)
│   ├── eassp_server.bas       # Main application
│   ├── config.inc             # Constants
│   └── types.inc              # Type definitions
└── docs/
    ├── wiring.md              # Hardware wiring guide
    └── protocol.md            # Communication protocol specification
```

## Hardware Requirements

### ESP8266 Side
- ESP8266 module (ESP-12F, NodeMCU, Wemos D1, etc.)
- INMP441 I2S MEMS microphone
- USB-to-UART adapter (for flashing and AT commands)

### PC Side
- Windows 7/10/11
- WiFi adapter (for receiving streams)
- Sound card (16-bit minimum, 24-bit for full quality)

### Wiring (INMP441 -> ESP8266)

```
INMP441      ESP8266
────────     ───────
SCK    ->    GPIO13  (I2SI_BCK)
WS     ->    GPIO14  (I2SI_WS)
SD     ->    GPIO12  (I2SI_DATA) + 100k pulldown to GND
L/R    ->    GND     (left channel)
VDD    ->    3.3V    + 0.1uF decoupling cap
GND    ->    GND
```

## Building the Firmware

### Recommended Build Environment

We recommend using [ESP8266-IDF](https://github.com/Dzantemir/ESP8266-IDF) - a Docker-based build environment that includes the ESP8266 RTOS SDK v3.4 and toolchain pre-configured.

```bash
# Clone the build environment
git clone https://github.com/Dzantemir/ESP8266-IDF.git
cd ESP8266-IDF

# Clone this project
git clone https://github.com/yourname/esp8266-wifi-microphone.git projects/esp8266-wifi-microphone

# Build using Docker
docker-compose run --rm esp8266 bash -c "
  cd /projects/esp8266-wifi-microphone/firmware &&
  export IDF_PATH=/opt/esp8266-rtos-sdk &&
  cp i2s_patched.c \$IDF_PATH/components/esp8266/driver/i2s.c &&
  idf.py build
"

# Flash (replace /dev/ttyUSB0 with your port)
docker-compose run --rm esp8266 --device /dev/ttyUSB0 bash -c "
  cd /projects/esp8266-wifi-microphone/firmware &&
  export IDF_PATH=/opt/esp8266-rtos-sdk &&
  idf.py -p /dev/ttyUSB0 flash
"
```

### Manual Build (without Docker)

If you prefer a manual setup:

1. Install ESP8266 RTOS SDK v3.4:
```bash
git clone --recursive https://github.com/espressif/ESP8266_RTOS_SDK.git
cd ESP8266_RTOS_SDK
git checkout release/v3.4
```

2. Install toolchain (xtensa-lx106-elf GCC 8.4.0)

3. Copy patched I2S driver:
```bash
cp firmware/i2s_patched.c $IDF_PATH/components/esp8266/driver/i2s.c
```

4. Build:
```bash
cd firmware
export IDF_PATH=/path/to/ESP8266_RTOS_SDK
idf.py build
idf.py flash
idf.py monitor
```

### Patched I2S Driver

The patched `i2s.c` enables:
- **BBPLL audio clock output** (required for 24-bit I2S - TRM section 10.2.1.2)
- **/48 divider** for 24-bit frame clock
- **Memory barriers** (`memw` instruction) for correct peripheral register access ordering at -O2 optimization
- **Conditional BBPLL enable** (only when WiFi is not initialized, to prevent conflicts)

Without this patch, 24-bit I2S capture will not work.

## Building the Receiver

### Prerequisites
- PowerBASIC 10 for Windows (PBWIN 10)

### Build
1. Open `server/eassp_server.bas` in PowerBASIC IDE
2. Compile to `eassp_server.exe`
3. Run the executable

## Quick Start

### 1. Flash the ESP8266
```bash
cd firmware
idf.py build
idf.py flash monitor
```

### 2. Configure WiFi (UDP mode)
Connect to the ESP8266's UART (115200 baud) and send:
```
AT+WIFI=YourWiFiSSID,YourWiFiPassword
AT+RATE=48000
AT+BITS=24
AT+AGC=2
AT+RST
```

### 3. Start the Receiver
Run `eassp_server.exe` on your Windows PC. The ESP8266 will appear in the device list automatically.

### 4. Start Streaming
1. Check the checkbox next to the device
2. Click "Start Stream"
3. Audio will play through your speakers

### 5. Record (Optional)
Click "DUMP" to start recording to WAV. Files are saved as `dump_HHMMSS_1.wav`, `dump_HHMMSS_2.wav` (auto-split at 1 GB).

## AT Command Reference

| Command | Description |
|---------|-------------|
| `AT` | Check connection |
| `AT+RST` | Reboot device |
| `AT+STATUS` | Full device status |
| `AT+WIFI=ssid,pass` | Set WiFi credentials |
| `AT+RATE=48000` | Sample rate (8000/11025/16000/22050/32000/44100/48000) |
| `AT+BITS=24` | Bit depth (16 or 24) |
| `AT+CH=0` | Channel: 0=left, 1=right, 2=stereo |
| `AT+CODEC=0` | Codec: 0=ADPCM, 1=PCM |
| `AT+AGC=2` | AGC: 0=OFF, 1=LOW, 2=MEDIUM, 3=HIGH |
| `AT+GAIN=32` | Fixed gain 0-64 (0=bypass, 32=+30dB) |
| `AT+RAWTX=1` | WiFi mode: 0=UDP, 1=Raw 802.11 TX |
| `AT+WCH=6` | WiFi channel 1-13 (Raw TX only) |
| `AT+HOTRESTART` | Restart stream to apply audio changes |
| `AT+FACTORY` | Factory reset |
| `AT+HELP` | Show all commands |

## Audio Quality

This project prioritizes audio quality at every stage:

### Capture
- **INMP441**: 24-bit I2S MEMS microphone with high SNR
- **24-bit I2S**: Full 24-bit capture (not downsampled at hardware level)

### Processing
- **TPDF Dithering**: Triangular PDF dither (Wannamaker/Vanderkooy/Lipshitz 1991) for 24->16-bit reduction. Linearizes the quantizer and decorrelates quantization error from the signal - the same technique used in professional audio equipment.
- **AGC (Automatic Gain Control)**: Speech-optimized with asymmetric attack/release, noise gate, and gain smoothing to prevent zipper noise. 4 presets (OFF/LOW/MEDIUM/HIGH).
- **Fixed Gain**: Configurable 0-64x (+0 to +36 dB) to compensate for INMP441's low sensitivity.

### Encoding
- **ADPCM**: IMA ADPCM (DVI4/RFC 3551) - 4 bits/sample, optimized for speech. IRAM-placed encoder hot path for zero flash cache stalls.
- **PCM**: Raw 16-bit or 24-bit signed PCM - lossless, zero compression artifacts. 24-bit PCM passes through bit-perfect if the sound card supports it.

### Playback
- **WaveOut API**: 16 audio buffers (NUM_WAVE_BUFS) for smooth, gap-free playback
- **24-bit support**: Native 24-bit WaveOut when sound card supports it, automatic 16-bit fallback with on-the-fly conversion
- **Stereo interleave**: Proper L/R deinterleave and reinterleave for stereo ADPCM

## Communication Protocol

### EASSP (ESP WiFi Microphone Service Protocol)

**Discovery** (UDP port 3950):
- Server sends `DISCOVER` (0x01) every 3 seconds
- Device responds with `INFO` (0x81) containing status, codec, sample rate, etc.

**Streaming Control**:
- `CONFIGURE` (0x02): Start streaming to specified port
- `STOP` (0x03): Stop streaming immediately (no watchdog wait)
- `INFO` (0x81): Periodic status updates (every 1 second)

**Audio Data** (UDP, port 5000+):

16-byte header + payload:
```
Offset  Field             Description
0       seq_num (u16)     Sequence number (wraps at 65535)
2       timestamp_ms (u32) Frame timestamp in milliseconds
6       codec (u8)        5=ADPCM, 6=PCM
7       sample_rate (u8)  Enum: 0=8k, 1=11k, 2=16k, 3=22k, 4=32k, 5=44k, 6=48k
8       channels (u8)     1=mono, 2=stereo
9       frame_ms (u8)     Frame duration in milliseconds
10      bitrate (u32)     Audio bitrate in bps
14      bits (u16)        Bits per sample (16 or 24)
```

### On-the-Fly Format Switching

When the ESP8266 changes audio format (via `AT+HOTRESTART`), the receiver:
1. Detects the format change from the packet header (codec, rate, channels, bits)
2. Closes the current WaveOut device
3. Opens a new WaveOut device with the new format
4. Resets the ADPCM decoder state
5. Continues playing without user intervention

### Raw 802.11 TX Mode

In Raw TX mode, the ESP8266 broadcasts raw 802.11 data frames directly into the air on a fixed WiFi channel. No router connection is needed. The receiver must be in Monitor Mode.

- TX rate: 11 Mbps (802.11b DSSS/CCK)
- Promiscuous mode enabled for radio activation
- Frame: Data frame, ToDS=0, FromDS=0, broadcast DA, sequence number per frame
- Drop rate: ~1-2% at 48kHz/2ch/24bit PCM (384 kbps)

## Architecture

### ESP8266 Firmware

```
INMP441 (I2S) -> TPDF Dither -> ADPCM/PCM Encoder -> UDP/Raw TX -> WiFi
                     |                |                  |
                24->16 bit       DVI4/RFC 3551      802.11 frames
```

Three FreeRTOS tasks:
1. **I2S Task** (priority 5): Captures audio from I2S DMA, applies gain/AGC, TPDF dither
2. **Encoder Task** (priority 3): ADPCM encode or PCM pack
3. **TX Task** (priority 2): Sends packets via UDP socket or esp_wifi_80211_tx

Stream control via FreeRTOS EventGroup:
- `STREAM_EVT_START_REQ` - set by svc_port on CONFIGURE or AT+HOTRESTART
- `STREAM_EVT_STOP_REQ` - set by svc_port on watchdog/CMD_STOP or AT+RST
- `STREAM_EVT_ACTIVE` - set by start_streaming, cleared by stop_streaming

### Windows Receiver

Threads:
- **GUI Thread**: Main dialog, ListView updates, button handling
- **Heartbeat Thread**: Sends DISCOVER heartbeats every 3 seconds
- **Audio Thread** (per device): Receives UDP audio, decodes, plays via WaveOut
- **Discovery Handler**: Processes incoming UDP on port 3950

Audio pipeline:
```
UDP Packet -> Header Parse -> Format Change Detection -> ADPCM Decode / PCM Copy -> WaveOut
                                |                          |
                           (if changed)              (16-bit or 24-bit)
                           Reopen WaveOut
```

## License

MIT License - see LICENSE file for details.
