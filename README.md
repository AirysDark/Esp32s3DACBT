# Esp32s3DACBT

Bluetooth audio bridge built around an **APB8202 V1.3 Bluetooth audio module**, an **ESP32-S3**, and the existing **NRG USB Audio 7.1** USB DAC/headphone adapter.

## Required ESP32 core

**This repository targets Arduino-ESP32 core 2.0.17.**

The GitHub Actions build is pinned to:

\`\`\`text
esp32 by Espressif Systems: 2.0.17
Board: ESP32S3 Dev Module
\`\`\`

The firmware does **not** use the current EspUsbHost library, because current EspUsbHost 2.x requires Arduino-ESP32 3.2.0 or newer. Instead, the sketch uses the ESP-IDF 4.4 USB Host API that is already bundled inside Arduino-ESP32 2.0.17.

The repository CI compiles the sketch against **2.0.17 on every push**.

---

## Signal path

\`\`\`text
Phone
  |
  | Bluetooth A2DP
  v
APB8202 V1.3
  |
  | L-OUT / R-OUT analog stereo
  v
ESP32-S3 ADC DMA
  |
  | PCM
  v
ESP32-S3 native USB HOST
  |
  | USB Audio Class 1
  v
NRG USB Audio 7.1
  |
  v
3.5 mm output
\`\`\`

The original Bluetooth-speaker main PCB and power-amplifier section are **not used** in the final build. Only the loose APB8202 module is required.

---

# APB8202 V1.3

Known 14-pin core pinout:

| Pin | Name | Function |
|---:|---|---|
| 1 | XTAL_P | Crystal oscillator input |
| 2 | XTAL_O | Crystal oscillator output |
| 3 | VIN | Module power |
| 4 | GND | System ground |
| 5 | TXD | UART TX |
| 6 | RXD | UART RX |
| 7 | CTS | UART CTS |
| 8 | TP6 | Factory test |
| 9 | TP5 | Factory test |
| 10 | TP4 | Factory test |
| 11 | TP3 | Factory test |
| 12 | TP2 | Factory test |
| 13 | TP1 / TEST_EN | Factory test / test enable |
| 14 | GND | Ground |

Audio pads:

\`\`\`text
L-OUT = left analog audio
R-OUT = right analog audio
AGND  = analog audio ground
\`\`\`

For this build, TXD/RXD/CTS/test pins are not needed.

---

# ESP32-S3 audio input wiring

The prototype uses:

\`\`\`text
GPIO4 = left ADC input  = ADC1 channel 3
GPIO5 = right ADC input = ADC1 channel 4
\`\`\`

## APB power

\`\`\`text
APB8202 VIN  -> ESP32 3.3V
APB8202 GND  -> ESP32 GND
APB8202 AGND -> ESP32 GND
\`\`\`

Do not connect the APB UART/test pins for this version.

---

# Shared 1.65 V VBIAS

The ESP32 ADC cannot measure the negative half of a normal AC audio waveform.

We therefore bias both ADC inputs around half of 3.3 V.

VBIAS is simply the middle junction of two 10 kΩ resistors:

\`\`\`text
ESP32 3.3V
    |
   10k
    |
    +---------- VBIAS ~1.65V
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
negative / striped leg -> GND
\`\`\`

Use a capacitor rated **6.3 V or higher**. 10 V, 16 V and 25 V are all fine.

Optional extra filtering:

\`\`\`text
VBIAS ---- 100nF ---- GND
\`\`\`

The 100 nF capacitor is optional for first testing.

Common capacitor codes:

\`\`\`text
104 = 100 nF = 0.1 uF
105 = 1 uF
106 = 10 uF
\`\`\`

---

# Left and right audio circuits

## Left

\`\`\`text
APB L-OUT
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

## Right

\`\`\`text
APB R-OUT
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

Both 10 kΩ bias resistors connect to the **same VBIAS point**:

\`\`\`text
                   VBIAS
                     |
             +-------+-------+
             |               |
            10k             10k
             |               |
           GPIO4           GPIO5
\`\`\`

Prefer non-polar 1 uF ceramic/film capacitors for the two audio coupling capacitors.

---

# Complete analog wiring

\`\`\`text
                         ESP32 3.3V
                              |
                             10k
                              |
                              +------ VBIAS
                              |          |
                             10k       (+)10uF(-)
                              |          |
                             GND        GND

APB L-OUT ---- 1uF ---- 10k ----+---- GPIO4
                                 |
                                10k
                                 |
                               VBIAS

APB R-OUT ---- 1uF ---- 10k ----+---- GPIO5
                                 |
                                10k
                                 |
                               VBIAS

APB VIN  ---------------------------- ESP32 3.3V
APB GND  ---------------------------- ESP32 GND
APB AGND ---------------------------- ESP32 GND
\`\`\`

Before connecting the APB audio signals, power the ESP32 and measure:

\`\`\`text
VBIAS -> GND ~= 1.65 V
\`\`\`

---

# NRG USB Audio 7.1

The NRG board was already tested directly on a Samsung USB-C phone and works, including its controls.

Its labelled USB pads are:

\`\`\`text
TOP

[ GND  ]  shield / chassis
[ DGND ]  USB electrical ground
[ +5V  ]  USB VBUS
[ D-   ]  USB data minus
[ D+   ]  USB data plus

BOTTOM
\`\`\`

ESP32-S3 native USB pins:

\`\`\`text
GPIO19 = USB D-
GPIO20 = USB D+
\`\`\`

If using a raw USB connection:

\`\`\`text
ESP32-S3 GPIO19 -> NRG D-
ESP32-S3 GPIO20 -> NRG D+
USB 5V VBUS     -> NRG +5V
USB GND         -> NRG DGND
shield          -> NRG GND where appropriate
\`\`\`

If your ESP32-S3 development board has a native USB-OTG USB-C connector, use that connector and a proper USB data/OTG connection.

## VBUS warning

The ESP32-S3 chip supports USB host mode, but **the chip does not magically create 5 V VBUS**.

Some development boards do not feed 5 V outward on the native USB connector when operating as host.

The NRG needs approximately 5 V on VBUS.

If the NRG does not power up, check:

\`\`\`text
NRG +5V to NRG DGND
\`\`\`

and verify approximately 5 V is present.

Never connect 5 V to the ESP32-S3 **3V3** pin.

---

# Why core 2.0.17 changes the firmware

Arduino-ESP32 2.0.17 is based on the ESP-IDF 4.4 generation.

Two important consequences:

1. The modern EspUsbHost 2.x library cannot be used because it requires Arduino-ESP32 3.2.0+.
2. The older ADC DMA API has a lower documented aggregate sample-rate ceiling.

Therefore this project uses:

\`\`\`text
adc_digi_initialize()
adc_digi_controller_configure()
adc_digi_read_bytes()
\`\`\`

and:

\`\`\`text
usb_host_install()
usb_host_client_register()
usb_host_interface_claim()
usb_host_transfer_alloc()
usb_host_transfer_submit()
\`\`\`

directly from the ESP-IDF APIs included with core 2.0.17.

---

# ADC rate on core 2.0.17

The IDF 4.4 ADC digital driver documents a maximum aggregate rate of about 83.3 k conversions/s.

We have two channels, so the code uses:

\`\`\`text
80,000 ADC conversions/second total
= 40,000 samples/second LEFT
+ 40,000 samples/second RIGHT
\`\`\`

The USB side then performs simple sample-rate conversion from the 40 kHz captured PCM to whichever NRG stream is selected.

The current USB output preference is:

\`\`\`text
48,000 Hz stereo 16-bit
fallback:
44,100 Hz stereo 16-bit
\`\`\`

---

# USB Audio support in this first core-2.0.17 build

The code implements a small USB Audio Class 1 host directly in the Arduino sketch.

It searches the NRG USB configuration descriptor for:

\`\`\`text
USB Audio Class 1
AudioStreaming interface
stereo
16-bit PCM
48 kHz or 44.1 kHz
isochronous OUT endpoint
\`\`\`

It then:

1. claims the AudioStreaming alternate interface,
2. attempts the UAC1 endpoint SET_CUR sample-rate request,
3. allocates multiple isochronous USB OUT transfers,
4. pulls stereo PCM from the ADC ring buffer,
5. resamples 40 kHz input to the USB output rate,
6. continuously feeds the NRG DAC.

This is deliberately targeted at the NRG unit used for this project rather than trying to be a universal USB-audio library.

---

# Arduino IDE setup

Install:

\`\`\`text
Arduino IDE
Boards Manager
esp32 by Espressif Systems
Version: 2.0.17
\`\`\`

Select:

\`\`\`text
Board: ESP32S3 Dev Module
\`\`\`

No additional USB host library is required.

In particular:

\`\`\`text
DO NOT install/use EspUsbHost for this core-2.0.17 version.
\`\`\`

The USB Host code comes from the ESP-IDF libraries bundled inside the ESP32 board package.

---

# Development / flashing

The native USB peripheral is needed for the NRG host connection.

During development, it is easiest to use a board with:

\`\`\`text
one USB/UART connector for programming + Serial Monitor
and
one native USB-OTG connector for the NRG
\`\`\`

If your board has only one USB connector, you may need an external USB-to-UART programmer while the native USB peripheral is being used as the NRG host.

---

# Serial output

The sketch prints information such as:

\`\`\`text
[ADC] running: GPIO4/GPIO5, 40000 Hz/channel
[USB] host library installed
[USB] client registered; waiting for NRG
[USB] device VID=....
[USB] UAC1 OUT: iface=... alt=... ep=... mps=... rate=48000
[USB] sample rate SET_CUR accepted
[USB] audio streaming started
\`\`\`

Once per second it also prints buffer statistics:

\`\`\`text
[STAT] ring=... adc_drop=... usb_starve=... usb=streaming rate=48000
\`\`\`

---

# Troubleshooting

## NRG does not power on

The board is probably not sourcing 5 V VBUS.

Measure NRG \`+5V\` to \`DGND\`.

## NRG powers but there is no USB device message

Check:

- GPIO19 is D-
- GPIO20 is D+
- cable carries USB data
- connector is the native USB-OTG port
- common ground
- 5 V VBUS
- core is actually 2.0.17

## "no supported UAC1 stereo 16-bit 48k/44.1k output stream"

The NRG is advertising a different USB Audio descriptor than expected.

Capture the Serial output / descriptor information and the parser can be extended for the exact stream.

## Audio crackles / drops

Watch:

\`\`\`text
adc_drop
usb_starve
\`\`\`

A rising \`adc_drop\` means the USB side is not consuming captured PCM quickly enough.

A rising \`usb_starve\` means the USB side is consuming PCM faster than it is arriving or timing is unstable.

The first version uses a simple rate converter and the ESP32-S3 internal ADC, so it is a functional prototype rather than the final hi-fi version.

## Better final audio quality

A later revision can replace the internal ADC with a stereo I2S ADC.

The overall architecture remains:

\`\`\`text
APB8202 -> digital capture -> ESP32-S3 -> USB host -> NRG
\`\`\`

---

# Repository build check

GitHub Actions is pinned to **Arduino-ESP32 2.0.17**.

A green workflow means the checked-in sketch compiled using the exact core version required by this project.
