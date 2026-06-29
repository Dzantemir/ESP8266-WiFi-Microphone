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
