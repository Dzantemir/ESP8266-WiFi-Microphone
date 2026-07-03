# Hardware Wiring Guide

## INMP441 I2S MEMS Microphone

The INMP441 is a high-quality 24-bit I2S MEMS microphone. It connects directly to the ESP8266's hardware I2S peripheral - no external ADC needed.

### Pin Connections

```
INMP441          ESP8266 Pin     GPIO
──────────       ───────────     ────
SCK  (Clock)  -> I2SI_BCK      -> GPIO13
WS   (Select) -> I2SI_WS       -> GPIO14
SD   (Data)   -> I2SI_DATA     -> GPIO12
L/R  (Channel)-> GND           (left channel)
VDD  (Power)  -> 3.3V          (+ 0.1uF decoupling cap to GND)
GND  (Ground) -> GND
```

### Notes

- **GPIO12 (MTDI)**: This pin is also used as a boot strapping pin. Make sure it has a 100k pulldown resistor to GND to ensure correct boot mode. The INMP441 data line is high-impedance when not actively sending data, so the pulldown won't affect audio.
- **3.3V power**: The INMP441 requires clean 3.3V power. Add a 0.1uF ceramic capacitor between VDD and GND, placed as close to the microphone as possible.
- **L/R pin**: Connect to GND for left channel, to VDD for right channel. The ESP8266 I2S driver is configured to capture only the selected channel.
- **Wire length**: Keep I2S wires short (< 10cm) to minimize noise. Longer wires may require shielded cable.
- **I2S RX timing delays**: If you experience audio glitches with longer wires
  or unusual PCB layout, the ESP8266 allows programming input delays on the
  SD/WS/BCK signals (0–3 APB clock cycles, 12.5 ns each at 80 MHz). Configure
  via `AT+TIMING=sd,ws,bck` or in menuconfig under `I2S Timing`. Default is 0
  (no delay) — suitable for INMP441 on short wires.

### Boot Strapping Pins

The ESP8266 uses several pins for boot mode selection. Make sure your microphone wiring doesn't interfere:

| GPIO | Boot Function     | Safe to use? |
|------|-------------------|--------------|
| 0    | Boot mode         | Avoid (used for flashing) |
| 2    | Boot mode         | OK (has pullup on most boards) |
| 12   | Flash voltage     | OK with pulldown (INMP441 SD) |
| 13   | -                 | OK (INMP441 SCK) |
| 14   | -                 | OK (INMP441 WS) |
| 15   | Boot mode         | Avoid (must be LOW at boot) |

### Optional: Battery Monitoring

If you want to monitor battery voltage, connect a voltage divider to the TOUT (ADC) pin:

```
V_batt ----[ R1: 100k ]----+---- TOUT (ADC)
                            |
                         [ R2: 33k ]
                            |
                           GND
```

Formula: `V_batt_mV = (ADC_raw * 5711) / 1024`

Enable in menuconfig: `STREAMER_BATTERY_ENABLED=y`

---

## Advanced: Studio-Quality Audio Modifications

The basic wiring (above) works, but the ESP8266 is a noisy digital environment:
WiFi TX/RX draws up to 170 mA in bursts, SPI flash runs at 40–80 MHz, and the
CPU switches at 80–160 MHz. All of this noise couples into the INMP441's 24-bit
ADC through the power and ground lines, and into the I2S signal lines as ringing.

The modifications below dramatically reduce digital noise and ringing, bringing
the audio quality close to studio-grade.

### 1. Ferrite Bead on VDD (power noise filter)

Replace the direct 3.3V wire to INMP441 VDD with a **ferrite bead** in series.
The ferrite bead presents near-zero resistance at audio frequencies but acts as
a **resistive absorber** (50–1000 Ω) at high frequencies (100 MHz+), converting
WiFi and CPU noise into heat rather than reflecting it back.

**Do NOT use an inductor/choke here** — inductors reflect noise (causing
standing waves and LC ringing) and generate back-EMF voltage spikes on
rapid current changes. Ferrite beads absorb noise in their resistive region.

Recommended: any SMD ferrite bead rated 600–1000 Ω @ 100 MHz
(e.g., BLM18AG601SN1, or any 0805/0603 bead).

```
ESP8266 3.3V ──[ Ferrite Bead 600Ω@100MHz ]──┬── INMP441 VDD
                                              │
                                         0.1µF ceramic
                                              │
                                         10µF (optional,
                                         electrolytic/tantal)
                                              │
                                             GND
```

- **0.1µF** ceramic: filters high-frequency noise (WiFi, CPU harmonics)
- **10µF** (optional): filters low-frequency ripple (WiFi TX current bursts)
- Place both capacitors as close to the INMP441 VDD pin as possible

### 2. Series Resistors on I2S Lines (ringing suppression)

I2S signals (especially SCK at ~2.8 MHz for 44.1 kHz) have fast edges that
cause **ringing** (reflections) on wires longer than a few centimeters.
Series resistors dampen these reflections by matching the source impedance
to the transmission line impedance (~50–100 Ω).

Add a **33–100 Ω resistor** in series on each I2S signal line:

| Line | GPIO | Resistor | Placement |
|------|------|:--------:|-----------|
| SCK (BCK) | GPIO13 | **33–47 Ω** | Near ESP8266 (signal source) |
| WS (LRCLK) | GPIO14 | **47–100 Ω** | Near ESP8266 (signal source) |
| SD (DATA) | GPIO12 | **47–100 Ω** | Near INMP441 (signal source) |

> **Rule:** Place the resistor closest to the **signal source**.
> SCK/WS are driven by the ESP8266 → resistor near ESP.
> SD is driven by the INMP441 → resistor near the microphone.

### 3. Direct GND Connection (no inductor/choke on ground)

Connect INMP441 GND **directly** to ESP8266 GND with a short wire.
**Do NOT put an inductor or choke on the ground line.**

Why not:
- The INMP441 uses GND as the ADC voltage reference. Any impedance on GND
  causes the reference to shift with current draw → signal distortion.
- Inductors on GND create back-EMF voltage spikes during WiFi TX bursts
  (V = L × dI/dt). A 200µH choke with 170 mA WiFi current steps can
  generate 34V spikes — enough to damage the microphone.
- Return currents from I2S signals flow through GND. An inductor blocks
  these return paths, forcing currents through parasitic paths → EMI.

If ground noise is still an issue after adding the ferrite bead on VDD,
a **ferrite bead** (not an inductor) can be placed on GND as well — it
absorbs noise without creating back-EMF.

### Complete Modified Wiring Diagram

```
                    VDD (3.3V)
                       │
               ┌───────┴───────┐
               │  Ferrite Bead  │
               │  (600Ω@100MHz) │
               └───────┬───────┘
                       │
              ┌────────┴────────┐
              │                 │
          0.1µF              10µF (opt)
          ceramic           electrolytic
              │                 │
              └────────┬────────┘
                       │
                  INMP441 VDD
                      
 ┌─────────┐    47Ω     ┌──────────┐
 │ GPIO13  ├──[ R ]─────┤ SCK      │
 │  (SCK)  │            │          │
 │ GPIO14  ├──[ R ]─────┤ WS       │     INMP441
 │  (WS)   │    47Ω     │          │
 │ GPIO12  ├────[ R ]───┤ SD       │
 │  (SD)   │     47Ω    │          │
 │         │            │ L/R──GND │
 └────┬────┘            └────┬─────┘
      │                      │
      │    100kΩ pulldown    │
      ├──[ 100k ]──GND       │
      │                      │
      └──────────┬───────────┘
                 │
            GND (direct, short wire)
```

### Summary of Components

| Component | Value | Purpose |
|-----------|-------|---------|
| Ferrite bead | 600–1000 Ω @ 100 MHz | Blocks WiFi/CPU noise on VDD |
| Ceramic cap | 0.1 µF | High-frequency VDD decoupling (near mic) |
| Electrolytic (optional) | 10 µF | Low-frequency VDD ripple filtering |
| Series resistor SCK | 33–47 Ω | Dampens ringing on clock line |
| Series resistor WS | 47–100 Ω | Dampens ringing on word-select line |
| Series resistor SD | 47–100 Ω | Dampens ringing on data line |
| Pulldown resistor | 100 kΩ | GPIO12 boot mode safety |

### Expected Improvement

- **Lower noise floor**: Digital hash from WiFi/CPU is filtered from VDD
- **Cleaner transients**: No ringing on I2S edges → fewer bit errors
- **Stable ADC reference**: Direct GND connection keeps 24-bit ADC reference stable
- **Better SNR**: Combination of all modifications can improve effective SNR
  by 6–12 dB compared to basic wiring
