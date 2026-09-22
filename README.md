# Esp32s3DACBT

Bluetooth audio bridge using an **APB8202 V1.3 Bluetooth audio module**, an **ESP32-S3**, and an existing **NRG USB Audio 7.1** USB sound-card/DAC.

The finished signal path is:

\`\`\`text
Phone / Bluetooth source
        |
        | Bluetooth A2DP
        v
APB8202 V1.3 Bluetooth module
        |
        | analog stereo: L-OUT / R-OUT / AGND
        v
ESP32-S3 ADC
        |
        | stereo PCM
        v
ESP32-S3 native USB HOST
        |
        | USB Audio Class
        v
NRG USB Audio 7.1
        |
        v
3.5 mm headphones / amplifier / speakers
\`\`\`

## Project status

The NRG USB Audio 7.1 has already been converted/tested as a USB-C audio device directly on a Samsung phone and works, including its control panel.

This repository is the next stage: put an ESP32-S3 between the Bluetooth receiver and the NRG so the APB8202 receives Bluetooth audio and the ESP32-S3 sends that audio to the NRG as USB Audio Class host data.

The firmware in this repository is the first Arduino-IDE prototype for that bridge. Hardware-specific USB-C/VBUS behavior still depends on the exact ESP32-S3 development board.

---

# Hardware

## 1. Bluetooth module

Exact module:

\`\`\`text
APB8202 V1.3
\`\`\`

Known core pinout:

| Pin | Name | Function |
|---:|---|---|
| 1 | XTAL_P | Crystal oscillator input |
| 2 | XTAL_O | Crystal oscillator output |
| 3 | VIN | Module power input |
| 4 | GND | Digital/system ground |
| 5 | TXD | UART transmit |
| 6 | RXD | UART receive |
| 7 | CTS | UART clear-to-send |
| 8 | TP6 | Factory test |
| 9 | TP5 | Factory test |
| 10 | TP4 | Factory test |
| 11 | TP3 | Factory test |
| 12 | TP2 | Factory test |
| 13 | TP1 / TEST_EN | Factory test / test enable |
| 14 | GND | Ground |

The module also exposes analog audio pads:

| Pad | Function |
|---|---|
| L-OUT | Left analog audio |
| R-OUT | Right analog audio |
| AGND | Audio ground |

For this project, the UART/test pins are **not required**.

The old Bluetooth-speaker main PCB and its amplifier section are **not part of the final build**. We are using the APB8202 module directly.

---

## 2. NRG USB Audio 7.1

The NRG board already contains the USB audio controller, DAC and 3.5 mm output.

Its USB pads were identified directly from the PCB silkscreen:

\`\`\`text
TOP

[ GND  ]   cable shield / chassis
[ DGND ]   USB electrical ground
[ +5V  ]   USB VBUS
[ D-   ]   USB data minus
[ D+   ]   USB data plus

BOTTOM
\`\`\`

The direct USB-C conversion has already been tested successfully with the Samsung phone.

For a raw ESP32-S3 USB host connection:

\`\`\`text
ESP32-S3 GPIO19  -> USB D-
ESP32-S3 GPIO20  -> USB D+
USB 5V VBUS      -> NRG +5V
USB GND          -> NRG DGND
USB shield       -> NRG GND/shield when appropriate
\`\`\`

If the ESP32-S3 development board already has a **native USB-OTG USB-C connector**, use that connector rather than manually wiring GPIO19/GPIO20.

### Important USB power note

The ESP32-S3 chip supports USB host mode, but not every development board supplies **5 V VBUS outward** from its USB-C connector.

The NRG requires USB VBUS power.

If the NRG does not power up when connected to the ESP32-S3 host port, the board may need:

- an OTG/VBUS enable jumper,
- a board-specific VBUS power switch,
- or an externally powered USB host connection.

Do **not** feed 5 V into the ESP32-S3 3.3 V rail.

A practical check is to measure between the NRG pads:

\`\`\`text
NRG +5V  -> approximately 5 V
NRG DGND -> ground
\`\`\`

while the NRG is connected to the ESP32-S3 host port.

---

# APB8202 -> ESP32-S3 audio wiring

The first prototype uses the ESP32-S3 internal ADC.

Chosen pins:

\`\`\`text
GPIO4 = LEFT ADC input
GPIO5 = RIGHT ADC input
\`\`\`

Both are ADC1-capable pins on ESP32-S3 and avoid the native USB pins GPIO19/GPIO20.

## Power wiring

\`\`\`text
APB8202 VIN   -> ESP32 3.3V
APB8202 GND   -> ESP32 GND
APB8202 AGND  -> ESP32 GND
\`\`\`

Unused for this build:

\`\`\`text
TXD   -> not connected
RXD   -> not connected
CTS   -> not connected
TP1-TP6 -> not connected
XTAL_P / XTAL_O -> not connected externally
\`\`\`

---

# 1.65 V VBIAS circuit

The ESP32 ADC cannot measure a signal that swings below 0 V.

Audio is AC, so both audio channels are shifted so that "zero audio" sits near **1.65 V**, halfway between 0 V and 3.3 V.

VBIAS is **not another power supply**. It is simply the junction between two equal 10 kΩ resistors.

\`\`\`text
ESP32 3.3V
    |
   10k
    |
    +------ VBIAS ~= 1.65V
    |
   10k
    |
ESP32 GND
\`\`\`

Add one 10 uF capacitor from VBIAS to ground:

\`\`\`text
VBIAS ---- (+) 10uF (-) ---- GND
\`\`\`

For an electrolytic 10 uF capacitor:

\`\`\`text
positive leg -> VBIAS
negative/striped leg -> GND
\`\`\`

A voltage rating of **6.3 V or higher** is enough. 10 V, 16 V, 25 V, etc. are all fine.

Optional extra filtering:

\`\`\`text
VBIAS ---- 100nF ---- GND
\`\`\`

The 100 nF capacitor is optional for the first prototype.

Common capacitor codes:

\`\`\`text
104 = 100 nF = 0.1 uF
105 = 1 uF
106 = 10 uF
\`\`\`

---

# Complete left/right ADC input circuit

## Left channel

\`\`\`text
APB8202 L-OUT
      |
     1uF
      |
     10k
      |
      +---------------- GPIO4
      |
     10k
      |
    VBIAS
\`\`\`

## Right channel

\`\`\`text
APB8202 R-OUT
      |
     1uF
      |
     10k
      |
      +---------------- GPIO5
      |
     10k
      |
    VBIAS
\`\`\`

Both channel-bias resistors connect back to the **same shared VBIAS point**:

\`\`\`text
                    VBIAS
                      |
              +-------+-------+
              |               |
             10k             10k
              |               |
            GPIO4           GPIO5
\`\`\`

The complete shared bias network is therefore:

\`\`\`text
                         3.3V
                           |
                          10k
                           |
                           +------ VBIAS
                           |          |
                          10k       (+)10uF(-)
                           |          |
                          GND        GND

VBIAS ---- 10k ---- GPIO4
VBIAS ---- 10k ---- GPIO5
\`\`\`

The 1 uF capacitors block DC from the APB8202 audio outputs.

Prefer a **1 uF ceramic or film capacitor** because it is non-polar.

If an electrolytic capacitor is used for the 1 uF coupling capacitor, measure the DC voltage on both sides first and put the positive lead toward the side with the higher DC voltage. Do not assume its polarity without measuring.

---

# Full point-to-point wiring summary

\`\`\`text
APB8202                         ESP32-S3
------------------------------------------------
VIN        ------------------> 3.3V
GND        ------------------> GND
AGND       ------------------> GND

L-OUT -> 1uF -> 10k ->+------> GPIO4
                      |
                     10k
                      |
                    VBIAS

R-OUT -> 1uF -> 10k ->+------> GPIO5
                      |
                     10k
                      |
                    VBIAS


VBIAS generator:

3.3V ---- 10k ----+---- 10k ---- GND
                  |
                  +---- (+)10uF(-) ---- GND
                  |
                  +---- optional 100nF ---- GND
\`\`\`

---

# USB host connection

The ESP32-S3 native USB peripheral uses:

\`\`\`text
GPIO19 = USB D-
GPIO20 = USB D+
\`\`\`

If using the board's native OTG USB-C connector:

\`\`\`text
ESP32-S3 native USB-C
        |
        | USB-C cable / correct OTG-host connection
        v
NRG USB-C
\`\`\`

Do not use a USB-to-UART bridge connector as the NRG host port. The NRG must be connected to the connector wired to the ESP32-S3 **native USB OTG peripheral**.

When using the native USB port as a host, USB CDC serial on that same native port cannot also own the USB peripheral. For debugging, use a separate UART/USB connector or external USB-to-UART adapter if your board provides one.

---

# Arduino IDE requirements

This sketch targets:

- ESP32-S3
- Arduino IDE
- Arduino-ESP32 **3.2.0 or newer**
- EspUsbHost **2.x**

For first bring-up, **Arduino-ESP32 3.3.10 is recommended**.

As of September 2026 there is an open Arduino-ESP32 issue reporting an ESP32-S3 USB-host enumeration regression in core 3.3.11. Core 3.3.12 is also based on ESP-IDF 5.5.5, so 3.3.10 is the conservative starting point until the host regression is confirmed fixed.

This project is separate from projects that intentionally require ESP32 core 2.0.17. **Do not use 2.0.17 for this sketch.**

## Install board package

Arduino IDE:

\`\`\`text
Tools / Board / Boards Manager
Search: esp32
Install/select: esp32 by Espressif Systems
Recommended first test version: 3.3.10
\`\`\`

## Install USB host library

Arduino IDE:

\`\`\`text
Sketch -> Include Library -> Manage Libraries
Search: EspUsbHost
Install: EspUsbHost
\`\`\`

The current library line supports USB Audio host streaming on ESP32-S3.

---

# Arduino IDE settings

Typical starting point:

\`\`\`text
Board: ESP32S3 Dev Module
USB CDC On Boot: Disabled
\`\`\`

If your board has separate UART and native-USB connectors, use the UART connector for upload/Serial Monitor and reserve the native USB connector for the NRG.

Exact menu names vary by board package and board definition.

---

# Firmware behavior

The sketch:

1. Samples APB8202 left/right analog audio using ESP32-S3 ADC1 DMA/continuous mode.
2. Samples at a nominal 48 kHz per channel.
3. Removes the approximately 1.65 V DC bias digitally.
4. Converts the 12-bit ADC data to signed 16-bit PCM.
5. Buffers stereo PCM in RAM.
6. Starts the ESP32-S3 as a USB host using EspUsbHost.
7. Detects the NRG as a USB Audio Class output device.
8. Prefers 48 kHz / stereo / 16-bit output.
9. Falls back to 44.1 kHz / stereo / 16-bit with simple sample-rate conversion.
10. Streams the captured Bluetooth audio to the NRG.
11. Prints USB/ADC buffer statistics to Serial.

The code is in:

\`\`\`text
Esp32s3DACBT.ino
\`\`\`

---

# Audio quality

This first version deliberately uses the ESP32-S3 internal ADC so no extra ADC chip is required.

Expected limitations:

- more noise than a dedicated audio ADC,
- limited ADC linearity,
- possible ground noise,
- some sample-clock drift,
- simple 44.1 kHz fallback resampling,
- quality depends heavily on wiring and grounding.

A later higher-quality version can replace the internal ADC with a stereo I2S audio ADC while leaving the USB-host/NRG side largely unchanged.

---

# First power-up procedure

1. Build the VBIAS network.
2. With no APB audio connected, power the ESP32-S3.
3. Measure **VBIAS -> GND**. It should be close to **1.65 V**.
4. Power off.
5. Connect APB8202 VIN/GND/AGND.
6. Connect L-OUT through its 1 uF + 10 kΩ network to GPIO4.
7. Connect R-OUT through its 1 uF + 10 kΩ network to GPIO5.
8. Flash the Arduino sketch.
9. Power the ESP32-S3.
10. Pair a phone to the APB8202 and play audio.
11. Connect the NRG to the ESP32-S3 native USB host port.
12. Confirm the NRG receives approximately 5 V VBUS.
13. Watch Serial output for the NRG USB descriptors and a supported 48 kHz or 44.1 kHz stereo stream.
14. Plug headphones into the NRG 3.5 mm socket.

---

# Troubleshooting

## NRG does not power on

The ESP32-S3 board may not source 5 V VBUS from its USB-C connector in host mode.

Check the board schematic/OTG jumper and measure NRG \`+5V\` to \`DGND\`.

## NRG powers but never enumerates

Check:

- correct native USB connector,
- USB host/OTG mode,
- cable supports data,
- VBUS is present,
- Arduino-ESP32 version.

For first bring-up use Arduino-ESP32 3.3.10.

## Serial says audio format unsupported

The sketch prints the USB audio streams advertised by the NRG.

Current firmware accepts:

\`\`\`text
48,000 Hz / 2 channel / 16 bit
44,100 Hz / 2 channel / 16 bit
\`\`\`

If the NRG exposes a different format, update the stream-selection code.

## Audio is distorted

Possible causes:

- L-OUT/R-OUT clipping the ADC input,
- poor ground connection,
- wrong coupling capacitor wiring,
- excessive PCM gain,
- ADC noise,
- VBIAS not near 1.65 V.

Reduce \`PCM_GAIN\` in the sketch if the ADC signal is clipping.

## Loud hum/noise

Keep:

- APB audio wiring short,
- AGND tied cleanly to ESP32 GND,
- VBIAS capacitor close to the ESP32 ADC wiring,
- USB/data wiring away from switching regulators where possible.

## NRG buttons

When the NRG was connected directly to the Samsung phone its controls worked.

With the ESP32-S3 acting as the host, those controls no longer automatically reach the phone. Forwarding NRG HID consumer-control events back through the Bluetooth module would be a separate feature.

---

# Reference links

- Arduino-ESP32 USB Host documentation: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/usb_host.html
- Arduino-ESP32 ADC documentation: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html
- EspUsbHost library: https://github.com/tanakamasayuki/EspUsbHost
- Arduino-ESP32 USB host regression report: https://github.com/espressif/arduino-esp32/issues/12783
