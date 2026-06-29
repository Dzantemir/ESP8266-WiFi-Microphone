<div align="center">

# 🎤 ESP8266 WiFi Microphone

### High-Fidelity Wireless Audio Streaming from ESP8266 to PC

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ESP8266 RTOS SDK](https://img.shields.io/badge/ESP8266%20RTOS%20SDK-v3.4-blue.svg)](https://github.com/espressif/ESP8266_RTOS_SDK)
[![PowerBASIC](https://img.shields.io/badge/PowerBASIC-10-red.svg)](https://www.powerbasic.com/)
[![Platform](https://img.shields.io/badge/Platform-ESP8266%20%7C%20Windows-lightgrey.svg)]()

**Professional 24-bit I2S capture • TPDF dithering • IMA ADPCM / PCM • Real-time playback • WAV recording**

*Developed in collaboration with AI (Z.ai Code)*

</div>

---

## ✨ Features

<table>
<tr>
<td width="50%" valign="top">

### 🎙️ Professional Audio Pipeline
- **24-bit I2S capture** with INMP441 MEMS microphone
- **TPDF dithering** (Wannamaker/Vanderkooy/Lipshitz) for 24→16-bit reduction
- **Software AGC** with 4 presets (OFF/LOW/MEDIUM/HIGH), noise gate, gain smoothing
- **Fixed digital gain** (0–64×, +0 to +36 dB)
- **IRAM-optimized** ADPCM encoder (zero flash cache stalls)

</td>
<td width="50%" valign="top">

### 📡 Dual WiFi Modes
- **UDP Mode**: Standard WiFi via router, 5+ Mbps throughput
- **Raw 802.11 TX Mode**: Broadcast directly to Monitor Mode receiver
  - No router needed
  - Fixed 11 Mbps TX rate (802.11b)
  - ~1–2% packet drop at 48kHz/2ch/24bit
  - Sequence-numbered frames with auto-increment

</td>
</tr>
<tr>
<td width="50%" valign="top">

### 🎵 Dual Codec Support
- **IMA ADPCM** (DVI4/RFC 3551): 4 bits/sample, ~32 kbps at 16kHz
  - RFC 3551 nibble packing (high nibble first)
  - Per-channel DVI4 header with predictor + step index
- **Raw PCM**: 16-bit or 24-bit signed, little-endian
  - 24-bit PCM passes through bit-perfect
  - Stereo interleaved

</td>
<td width="50%" valign="top">

### 🔄 On-the-Fly Format Switching
- Change sample rate, channels, bit depth, codec **without rebooting**
- `AT+HOTRESTART` restarts the stream pipeline in ~200ms
- Receiver **auto-detects** format change from packet header
- Reopens WaveOut with new format seamlessly
- Resets ADPCM decoder state on codec change

</td>
</tr>
<tr>
<td width="50%" valign="top">

### 🎛️ Full AT Command Interface
- Configure everything over UART (115200 baud)
- All settings persist in NVS flash
- `AT+HOTRESTART` applies audio changes instantly
- No reboot needed for audio parameter changes

</td>
<td width="50%" valign="top">

### 💻 Windows Receiver (EASSP Server)
- **Multi-device**: Stream from up to 16 ESP8266s simultaneously
- **Auto-discovery**: UDP broadcast on port 3950
- **WAV recording**: 1 GB auto-split, correct headers for all formats
- **24-bit playback**: Native WaveOut, auto-fallback to 16-bit
- **Real-time stats**: RSSI, heap, packet loss, duration

</td>
</tr>
</table>

---

## 📸 Social Preview

![ESP8266 WiFi Microphone](social_preview.png)

---

## 🏗️ Architecture

```
                    ESP8266 Firmware
 ┌─────────────────────────────────────────────────────────┐
 │                                                         │
 │  INMP441 ──I2S──> TPDF Dither ──> ADPCM/PCM ──> WiFi  │
 │   24-bit           24→16 bit       Encode        TX    │
 │   capture          dither          DVI4/PCM            │
 │                                                         │
 │  ┌──────────┐  ┌───────────┐  ┌──────────┐  ┌───────┐ │
 │  │ I2S Task │->│ Enc Task  │->│ TX Task  │->│ WiFi  │ │
 │  │ prio: 5  │  │ prio: 3   │  │ prio: 2  │  │ Radio │ │
 │  └──────────┘  └───────────┘  └──────────┘  └───────┘ │
 │       │              │              │                   │
 │  Gain/AGC       ADPCM nibbles   UDP socket /           │
 │  TPDF dither    or PCM copy     esp_wifi_80211_tx      │
 │                                                         │
 └─────────────────────────────────────────────────────────┘
                          │
                          │ UDP / Raw 802.11
                          ▼
                    Windows Receiver
 ┌─────────────────────────────────────────────────────────┐
 │                                                         │
 │  UDP RX ──> Header Parse ──> ADPCM Decode ──> WaveOut  │
 │                            or PCM copy        Playback │
 │                                 │                       │
 │                          Format Change?                 │
 │                           ├── Yes -> Reopen WaveOut     │
 │                           └── No  -> Continue           │
 │                                                         │
 │  ┌─────────────┐  ┌────────────┐  ┌─────────────────┐  │
 │  │ AudioThread │  │ Heartbeat  │  │ Discovery Proc  │  │
 │  │ (per device)│  │ Thread     │  │ (UDP port 3950) │  │
 │  └─────────────┘  └────────────┘  └─────────────────┘  │
 │                                                         │
 │  + WAV Recording (1 GB auto-split)                     │
 │  + ListView with live device stats                     │
 │  + Multi-device simultaneous streaming                 │
 │                                                         │
 └─────────────────────────────────────────────────────────┘
```

---

## 🔧 Hardware

### Bill of Materials

| Component | Purpose | Price |
|-----------|---------|-------|
| ESP8266 (ESP-12F / NodeMCU / Wemos D1) | Microcontroller + WiFi | ~$3 |
| INMP441 I2S MEMS microphone | Audio capture (24-bit) | ~$2 |
| USB-to-UART adapter | Flashing + AT commands | ~$2 |

### Wiring Diagram

```
 ┌──────────────┐              ┌──────────────┐
 │   INMP441    │              │    ESP8266   │
 │              │              │              │
 │  VDD ────────┼──────────────┼─ 3.3V        │
 │  GND ────────┼──────────────┼─ GND         │
 │  SCK ────────┼──────────────┼─ GPIO13      │
 │  WS  ────────┼──────────────┼─ GPIO14      │
 │  SD  ────────┼──────────────┼─ GPIO12      │
 │  L/R ────────┼──────────────┼─ GND (left)  │
 │              │              │              │
 └──────────────┘              └──────────────┘
```

> ⚠️ **Note**: Add a 100kΩ pulldown resistor on GPIO12 (INMP441 SD line) to ensure correct boot mode.

> 💡 **Tip**: Place a 0.1µF ceramic capacitor between INMP441 VDD and GND, as close to the microphone as possible.

---

## 🚀 Quick Start

### 1. Build & Flash Firmware

We recommend using [ESP8266-IDF](https://github.com/Dzantemir/ESP8266-IDF) Docker build environment:

```bash
# Clone build environment
git clone https://github.com/Dzantemir/ESP8266-IDF.git
cd ESP8266-IDF

# Clone this project
git clone https://github.com/yourname/esp8266-wifi-microphone.git projects/esp8266-wifi-microphone

# Build
docker-compose run --rm esp8266 bash -c "
  cd /projects/esp8266-wifi-microphone/firmware &&
  export IDF_PATH=/opt/esp8266-rtos-sdk &&
  cp i2s.c \$IDF_PATH/components/esp8266/driver/i2s.c &&
  idf.py build
"

# Flash
docker-compose run --rm esp8266 --device /dev/ttyUSB0 bash -c "
  cd /projects/esp8266-wifi-microphone/firmware &&
  export IDF_PATH=/opt/esp8266-rtos-sdk &&
  idf.py -p /dev/ttyUSB0 flash
"
```

<details>
<summary>📖 Manual build (without Docker)</summary>

```bash
# Install ESP8266 RTOS SDK v3.4
git clone --recursive https://github.com/espressif/ESP8266_RTOS_SDK.git
cd ESP8266_RTOS_SDK && git checkout release/v3.4

# Install toolchain: xtensa-lx106-elf (GCC 8.4.0)

# Copy patched I2S driver (REPLACE the SDK file)
cp firmware/i2s.c $IDF_PATH/components/esp8266/driver/i2s.c

# Build
cd firmware
export IDF_PATH=/path/to/ESP8266_RTOS_SDK
idf.py build
idf.py flash monitor
```

</details>

### 2. Configure WiFi

Connect to ESP8266 UART (115200 baud) and send:

```
AT+WIFI=YourWiFiSSID,YourWiFiPassword
AT+RATE=48000
AT+BITS=24
AT+AGC=2
AT+RST
```

### 3. Start the Receiver

Run `eassp_server.exe` on your Windows PC. The ESP8266 appears automatically:

1. ✅ Check the checkbox next to the device
2. ▶️ Click **Start Stream**
3. 🔊 Audio plays through your speakers!

### 4. Record (Optional)

Click **DUMP** to record to WAV. Files auto-split at 1 GB:
- `dump_153000_1.wav` — first gigabyte
- `dump_153000_2.wav` — second gigabyte
- ...

---

## ⌨️ AT Command Reference

| Command | Description | Example |
|---------|-------------|---------|
| `AT` | Check connection | `AT` |
| `AT+RST` | Reboot device | `AT+RST` |
| `AT+STATUS` | Full device status | `AT+STATUS` |
| `AT+WIFI=ssid,pass` | Set WiFi credentials | `AT+WIFI=MyHome,secret123` |
| `AT+RATE=n` | Sample rate (Hz) | `AT+RATE=48000` |
| `AT+BITS=n` | Bit depth (16 or 24) | `AT+BITS=24` |
| `AT+CH=n` | Channel: 0=L, 1=R, 2=stereo | `AT+CH=0` |
| `AT+CODEC=n` | 0=ADPCM, 1=PCM | `AT+CODEC=0` |
| `AT+AGC=n` | 0=OFF, 1=LOW, 2=MED, 3=HIGH | `AT+AGC=2` |
| `AT+GAIN=n` | Fixed gain 0-64 (0=bypass) | `AT+GAIN=32` |
| `AT+FMT=n` | 0=Philips I2S, 1=LSB | `AT+FMT=0` |
| `AT+RAWTX=n` | 0=UDP, 1=Raw 802.11 TX | `AT+RAWTX=1` |
| `AT+WCH=n` | WiFi channel 1-13 (Raw TX) | `AT+WCH=6` |
| `AT+HOTRESTART` | Restart stream (apply changes) | `AT+HOTRESTART` |
| `AT+FACTORY` | Factory reset | `AT+FACTORY` |
| `AT+HELP` | Show all commands | `AT+HELP` |

---

## 🎵 Audio Quality

This project prioritizes audio quality at every stage:

<details>
<summary>🔬 Detailed quality analysis</summary>

### Capture Stage
| Parameter | Value |
|-----------|-------|
| Microphone | INMP441 (24-bit I2S MEMS) |
| SNR | 61 dB SPL |
| Sensitivity | -26 dBFS @ 94 dB SPL |
| Bit depth | 24-bit (configurable to 16-bit) |
| Sample rates | 8, 11.025, 16, 22.05, 32, 44.1, 48 kHz |

### Processing Stage
| Technique | Purpose |
|-----------|---------|
| TPDF Dithering | Linearizes quantizer, decorrelates error (24→16 bit) |
| AGC | Speech-optimized, asymmetric attack/release, noise gate |
| Gain Smoothing | Prevents zipper noise on gain changes |
| IRAM Encoder | Zero flash cache stalls during WiFi SPI operations |

### ADPCM Details
- IMA ADPCM (DVI4 / RFC 3551)
- 4 bits per sample (8:1 compression vs 16-bit PCM)
- Per-channel DVI4 header (predictor + step index)
- Step table: 89 entries, index table: 16 entries
- Encoder hot path in IRAM for deterministic timing

### PCM Details
- 16-bit: 2 bytes/sample, direct passthrough
- 24-bit: 3 bytes/sample (low 3 bytes of int32), bit-perfect
- Stereo: interleaved L/R/L/R
- No compression artifacts

</details>

---

## 📡 Protocol

<details>
<summary>📋 EASSP Protocol Specification</summary>

### Service Port (UDP 3950)

| Command | Code | Direction | Description |
|---------|------|-----------|-------------|
| DISCOVER | 0x01 | Server→Device | Heartbeat / discovery |
| CONFIGURE | 0x02 | Server→Device | Start streaming to port |
| STOP | 0x03 | Server→Device | Stop streaming immediately |
| INFO | 0x81 | Device→Server | Status response |

### Audio Packet Header (16 bytes)

```
Offset  Field           Type     Description
0       seq_num         u16      Sequence number (wraps at 65535)
2       timestamp_ms    u32      Frame timestamp in milliseconds
6       codec           u8       5=ADPCM, 6=PCM
7       sample_rate     u8       Enum: 0=8k..6=48k
8       channels        u8       1=mono, 2=stereo
9       frame_ms        u8       Frame duration in ms
10      bitrate         u32      Audio bitrate in bps
14      bits            u16      Bits per sample (16 or 24)
```

### On-the-Fly Format Switching

When ESP8266 changes format via `AT+HOTRESTART`:
1. Receiver detects changed fields in packet header
2. Closes WaveOut → Opens new WaveOut with new format
3. Resets ADPCM decoder state
4. Continues playing — no user intervention needed

</details>

---

## 📁 Project Structure

```
esp8266-wifi-microphone/
├── README.md                      # You are here
├── LICENSE                        # MIT
├── social_preview.png             # GitHub social preview image
│
├── firmware/                      # ESP8266 firmware (ESP8266 RTOS SDK v3.4)
│   ├── CMakeLists.txt
│   ├── sdkconfig                  # Build configuration
│   ├── i2s.c                      # Patched I2S driver (REPLACE in SDK)
│   └── main/
│       ├── CMakeLists.txt
│       ├── Kconfig.projbuild      # menuconfig options
│       ├── main.c                 # Main app + pipeline tasks
│       ├── stream_mode.c          # UDP/Raw TX mode abstraction (vtable)
│       ├── wifi_sta.c             # WiFi STA + Raw TX + fixed rate
│       ├── udp_stream.c           # UDP socket / Raw 802.11 TX
│       ├── at_cmd.c               # AT command interface
│       ├── config_mgr.c           # NVS configuration manager
│       ├── svc_port.c             # EASSP service port
│       ├── i2s_capture.c          # I2S capture + AGC + gain
│       ├── adpcm_encoder.c        # IMA ADPCM encoder (IRAM)
│       ├── tpdf_dither.c          # TPDF dithering
│       ├── battery.c              # Battery monitoring (optional)
│       └── include/               # 14 header files
│
├── server/                        # Windows receiver (PowerBASIC 10)
│   ├── eassp_server.bas           # Main application
│   ├── config.inc                 # Constants
│   └── types.inc                  # Type definitions
│
└── docs/
    ├── wiring.md                  # Hardware wiring guide
    └── protocol.md                # Protocol specification
```

---

## 🙏 Acknowledgments

- **Espressif** — ESP8266 RTOS SDK
- **PowerBASIC** — Windows application development
- **Z.ai Code** — AI-assisted development
- **INMP441** — High-quality I2S MEMS microphone

---

## 📄 License

MIT License — see [LICENSE](LICENSE) for details.

---

<div align="center">

**⭐ Star this project if you find it useful!**

Made with ❤️ and AI

</div>
