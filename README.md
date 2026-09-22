# Esp32s3DACBT

Bluetooth audio bridge built around an **APB8202 V1.3 Bluetooth audio module**, an **ESP32-S3**, and the existing **NRG USB Audio 7.1** USB DAC/headphone adapter.

## Required ESP32 core

**This repository targets Arduino-ESP32 core 2.0.17.**

## ESP32-S3 module used

The hardware used for this project is the **ESP32-S3-WROOM-1-N16R8** variant.

The `N16R8` memory configuration means:

| Memory | Installed |
|---|---:|
| Flash | **16 MB** |
| External PSRAM / SPIRAM | **8 MB** |
| Flash bus | **Quad SPI (QSPI)** |
| PSRAM bus | **Octal SPI (OPI)** |
| Internal SRAM | **512 KB** |
| CPU | Dual-core Xtensa LX7, up to **240 MHz** |

### Arduino IDE settings used

Use these settings with **esp32 by Espressif Systems 2.0.17**:

```text
Board:             ESP32S3 Dev Module
CPU Frequency:     240MHz
Flash Mode:        QIO
Flash Size:        16MB
Partition Scheme:  16M Flash (3MB APP / 9.9MB FATFS)
PSRAM:             OPI PSRAM
Debug Level:       None
```

The matching Arduino CLI/FQBN options used by GitHub Actions are:

```text
esp32:esp32:esp32s3:
  CPUFreq=240,
  FlashMode=qio,
  FlashSize=16M,
  PartitionScheme=app3M_fat9M_16MB,
  PSRAM=opi,\n  DebugLevel=none\n```

The workflow therefore compiles for the **actual N16R8 memory configuration**, rather than the generic ESP32-S3 defaults.

### 16 MB flash layout

Core 2.0.17 does contain a `default_16MB.csv` file internally, but that partition is not exposed by the ESP32S3 Dev Module board menu in core 2.0.17. The project therefore uses the exposed `app3M_fat9M_16MB` option so the full 16 MB flash is addressed while retaining OTA support.

```text
NVS       : 20 KB\nOTA data  : 8 KB\nAPP0      : 3 MB\nAPP1      : 3 MB\nFATFS     : 9.875 MB\nCore dump : 64 KB
```

This layout provides two 3 MB firmware slots for OTA/update testing and uses almost 10 MB of the remaining flash as FATFS storage, so the 16 MB module is not treated like the generic 4 MB default.

The 8 MB OPI PSRAM is enabled by the board configuration. DMA-critical USB/ADC buffers should remain in internal DMA-capable RAM; large non-DMA audio/history buffers can be moved to PSRAM later if needed.

The GitHub Actions build is pinned to:

```text
esp32 by Espressif Systems: 2.0.17
Board: ESP32S3 Dev Module
Flash: 16MB QIO
Partition: app3M_fat9M_16MB
PSRAM: 8MB OPI
CPU: 240MHz
```

The firmware does **not** use the current EspUsbHost library, because current EspUsbHost 2.x requires Arduino-ESP32 3.2.0 or newer. Instead, the sketch uses the ESP-IDF 4.4 USB Host API that is already bundled inside Arduino-ESP32 2.0.17.

The repository CI compiles the sketch against **2.0.17 on every push** using the N16R8 board settings above.

## Source layout

The firmware is split into small modules so each subsystem can be debugged without working through one very large `.ino` file.

| File | Purpose |
|---|---|
| `Esp32s3DACBT.ino` | Arduino `setup()` / `loop()` only; starts the subsystems and prints health statistics |
| `ProjectConfig.h` | GPIO assignments, ADC rates, PCM buffer size, USB rates and other project-wide constants |
| `AudioBuffer.h` / `AudioBuffer.cpp` | Stereo PCM ring buffer plus ADC-drop and USB-starvation counters |
| `AdcAudio.h` / `AdcAudio.cpp` | ESP32-S3 ADC DMA setup, APB8202 left/right sampling and conversion to signed PCM |
| `APB8202Monitor.h` / `APB8202Monitor.cpp` | Passive RX-only APB8202 UART/HCI capture, raw HEX/ASCII burst logging and baud switching |
| `UsbAudioHost.h` / `UsbAudioHost.cpp` | ESP-IDF 4.4 USB Host, UAC1 descriptor parsing, sample-rate setup, resampling and isochronous transfers to the NRG |

Debugging map:

```text
Bluetooth/analog problem -> AdcAudio.cpp
APB raw UART discovery   -> APB8202Monitor.cpp
PCM buffering/overrun    -> AudioBuffer.cpp
NRG USB/UAC problem      -> UsbAudioHost.cpp
Pins/rates/buffer sizes  -> ProjectConfig.h
Startup/status reporting -> Esp32s3DACBT.ino
```

All `.cpp` and `.h` files stay in the same Arduino sketch folder as `Esp32s3DACBT.ino`, so Arduino IDE automatically compiles them with the sketch.

---

## Signal path

```text
Phone
  |
  | Bluetooth audio
  v
APB8202 V1.3 (CW6638M)
  |\
  | \ L-OUT / R-OUT analog stereo
  |  \____________________________> ESP32-S3 ADC -> PCM -> USB Host -> NRG
  |
  +---- TXD pin 5 -----------------> ESP32-S3 GPIO18 RX
          passive UART/HCI discovery only
```

The original Bluetooth-speaker main PCB and power-amplifier section are **not used** in the final build. Only the loose APB8202 module is required.

---

# APB8202 V1.3 / Buildwin CW6638M

The verified factory information used by this project is deliberately separated from assumptions about the unknown firmware protocol.

```text
Bluetooth core:      CW6638M / CW6637 family
Bluetooth:           v2.1 + EDR
Module VIN:          2.2 V to 5.5 V
Project supply:      3.3 V
Audio:               16-bit stereo
Remote control:      AVRCP supported
UART:                present on TXD/RXD/CTS pins
Protocol handling:   treat as unknown binary/HCI-style data during discovery
```

The firmware does **not** currently assume an ASCII AT-command protocol, CRLF command framing, text connection notifications, or any Bluetooth-name command.

## 14-pin module pinout

| Pin | Label | Function |
|---:|---|---|
| 1 | XTAL_P | Crystal / alternate clock connection |
| 2 | XTAL_O | Crystal / alternate clock connection |
| 3 | VIN | Module power input, 2.2-5.5 V supported |
| 4 | GND | Primary digital ground |
| 5 | TXD | UART serial output |
| 6 | RXD | UART serial input |
| 7 | CTS | UART flow-control / multifunction pin |
| 8 | TP6 | Factory test pad |
| 9 | TP5 | Factory test pad |
| 10 | TP4 | Factory test pad |
| 11 | TP3 | Factory test pad |
| 12 | TP2 | Factory test pad |
| 13 | TP1 | Factory boot/test input |
| 14 | GND | Secondary ground |

Audio pads:

```text
L-OUT = left analog audio
R-OUT = right analog audio
AGND  = analog audio ground
```

## Safe UART discovery wiring

During protocol discovery only the APB8202 transmit line is connected:

```text
ESP32-S3                     APB8202
------------------------------------------------
3V3        ----------------> VIN  pin 3
GND        ----------------> GND  pin 4/14
GPIO18 RX  <---------------- TXD  pin 5

NOT CONNECTED:
GPIO17 TX  -X-              RXD  pin 6
            -X-              CTS  pin 7
```

**Leave APB8202 RXD pin 6 and CTS pin 7 unconnected during discovery.**

GPIO17 is reserved in `ProjectConfig.h` for possible later transmit use after the real protocol has been identified, but it is not driven by the current firmware.

## Passive UART/HCI monitor

`APB8202Monitor.cpp` is deliberately RX-only. `Serial1` is started with its TX pin disabled (`-1`), so the ESP32 cannot accidentally send guessed AT strings or unknown packets to the module.

Captured data is grouped into short bursts and printed in both hexadecimal and printable ASCII:

```text
[APB RAW] baud=115200 burst=3 len=8 HEX: 04 0E 04 01 03 0C 00 00  ASCII: ........
```

The bytes above are only an example of the output format; they are not claimed to be the APB8202's actual boot packet.

The monitor starts at 9600 baud and provides quick stepping through:

```text
9600
38400
115200
921600
```

Any arbitrary receive baud can also be selected manually.

For a real boot-signature test, choose one baud, then power-cycle/reset the APB8202 and watch the raw output. Repeat at the next baud. Simply changing baud after boot will not recreate bytes that were only transmitted during startup.

## Serial Monitor commands

Use the board's **COM USB-C port** for flashing and Serial Monitor.

```text
:baud 9600
:baud 38400
:baud 115200
:baud 921600
:nextbaud
:stats
:clear
:help
```

Any non-local command is rejected because APB transmit is disabled in discovery mode.

## Control protocol status

Media-control transmission is intentionally **not implemented yet**. The CW6638M documentation establishes that UART is used for audio/control transport, but the exact packet format exposed by this APB8202 firmware still has to be captured from the real hardware before the ESP32 sends anything.

Once the real framing/opcodes are identified, a separate protocol/control layer can be added without changing the analog audio or USB portions of the project.

---
# ESP32-S3 audio input wiring

The prototype uses:

```text
GPIO4 = left ADC input  = ADC1 channel 3
GPIO5 = right ADC input = ADC1 channel 4
```

## APB power

```text
APB8202 VIN  -> ESP32 3.3V
APB8202 GND  -> ESP32 GND
APB8202 AGND -> ESP32 GND
```

For UART discovery, only APB TXD pin 5 connects to ESP32 GPIO18 RX. APB RXD pin 6, CTS pin 7, and TP1-TP6 remain unconnected.

---

# Shared 1.65 V VBIAS

The ESP32 ADC cannot measure the negative half of a normal AC audio waveform.

We therefore bias both ADC inputs around half of 3.3 V.

VBIAS is simply the middle junction of two 10 kΩ resistors:

```text
ESP32 3.3V
    |
   10k
    |
    +---------- VBIAS ~1.65V
    |
   10k
    |
ESP32 GND
```

Add one 10 uF capacitor from VBIAS to ground:

```text
VBIAS ---- (+) 10uF (-) ---- GND
```

For an electrolytic 10 uF capacitor:

```text
positive leg -> VBIAS
negative / striped leg -> GND
```

Use a capacitor rated **6.3 V or higher**. 10 V, 16 V and 25 V are all fine.

Optional extra filtering:

```text
VBIAS ---- 100nF ---- GND
```

The 100 nF capacitor is optional for first testing.

Common capacitor codes:

```text
104 = 100 nF = 0.1 uF
105 = 1 uF
106 = 10 uF
```

---

# Left and right audio circuits

## Left

```text
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
```

## Right

```text
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
```

Both 10 kΩ bias resistors connect to the **same VBIAS point**:

```text
                   VBIAS
                     |
             +-------+-------+
             |               |
            10k             10k
             |               |
           GPIO4           GPIO5
```

Prefer non-polar 1 uF ceramic/film capacitors for the two audio coupling capacitors.

---

# Complete analog wiring

```text
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
```

Before connecting the APB audio signals, power the ESP32 and measure:

```text
VBIAS -> GND ~= 1.65 V
```

---

# NRG USB Audio 7.1

The NRG board was already tested directly on a Samsung USB-C phone and works, including its controls.

Its labelled USB pads are:

```text
TOP

[ GND  ]  shield / chassis
[ DGND ]  USB electrical ground
[ +5V  ]  USB VBUS
[ D-   ]  USB data minus
[ D+   ]  USB data plus

BOTTOM
```

ESP32-S3 native USB pins:

```text
GPIO19 = USB D-
GPIO20 = USB D+
```

If using a raw USB connection:

```text
ESP32-S3 GPIO19 -> NRG D-
ESP32-S3 GPIO20 -> NRG D+
USB 5V VBUS     -> NRG +5V
USB GND         -> NRG DGND
shield          -> NRG GND where appropriate
```

If your ESP32-S3 development board has a native USB-OTG USB-C connector, use that connector and a proper USB data/OTG connection.

## VBUS warning

The ESP32-S3 chip supports USB host mode, but **the chip does not magically create 5 V VBUS**.

Some development boards do not feed 5 V outward on the native USB connector when operating as host.

The NRG needs approximately 5 V on VBUS.

If the NRG does not power up, check:

```text
NRG +5V to NRG DGND
```

and verify approximately 5 V is present.

Never connect 5 V to the ESP32-S3 **3V3** pin.

---

# Why core 2.0.17 changes the firmware

Arduino-ESP32 2.0.17 is based on the ESP-IDF 4.4 generation.

Two important consequences:

1. The modern EspUsbHost 2.x library cannot be used because it requires Arduino-ESP32 3.2.0+.
2. The older ADC DMA API has a lower documented aggregate sample-rate ceiling.

Therefore this project uses:

```text
adc_digi_initialize()
adc_digi_controller_configure()
adc_digi_read_bytes()
```

and:

```text
usb_host_install()
usb_host_client_register()
usb_host_interface_claim()
usb_host_transfer_alloc()
usb_host_transfer_submit()
```

directly from the ESP-IDF APIs included with core 2.0.17.

---

# ADC rate on core 2.0.17

The IDF 4.4 ADC digital driver documents a maximum aggregate rate of about 83.3 k conversions/s.

We have two channels, so the code uses:

```text
80,000 ADC conversions/second total
= 40,000 samples/second LEFT
+ 40,000 samples/second RIGHT
```

The USB side then performs simple sample-rate conversion from the 40 kHz captured PCM to whichever NRG stream is selected.

The current USB output preference is:

```text
48,000 Hz stereo 16-bit
fallback:
44,100 Hz stereo 16-bit
```

---

# USB Audio support in this first core-2.0.17 build

The code implements a small USB Audio Class 1 host directly in the Arduino sketch.

It searches the NRG USB configuration descriptor for:

```text
USB Audio Class 1
AudioStreaming interface
stereo
16-bit PCM
48 kHz or 44.1 kHz
isochronous OUT endpoint
```

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

```text
Arduino IDE
Boards Manager
esp32 by Espressif Systems
Version: 2.0.17
```

Then set:

```text
Board:             ESP32S3 Dev Module
CPU Frequency:     240MHz
Flash Mode:        QIO
Flash Size:        16MB
Partition Scheme:  16M Flash (3MB APP / 9.9MB FATFS)
PSRAM:             OPI PSRAM
```

No additional USB host library is required.

In particular:

```text
DO NOT install/use EspUsbHost for this core-2.0.17 version.
```

The USB Host code comes from the ESP-IDF libraries bundled inside the ESP32 board package.

---

# Development / flashing

The native USB peripheral is needed for the NRG host connection.

During development, it is easiest to use a board with:

```text
one USB/UART connector for programming + Serial Monitor
and
one native USB-OTG connector for the NRG
```

If your board has only one USB connector, you may need an external USB-to-UART programmer while the native USB peripheral is being used as the NRG host.

---

# Serial output

The sketch prints information such as:

```text
[APB MON] RX-only GPIO18 @ 9600 baud, 8N1
[APB MON] Leave APB RXD pin 6 and CTS pin 7 unconnected.
[ADC] running: GPIO4/GPIO5, 40000 Hz/channel
[USB] host library installed
[USB] client registered; waiting for NRG
[APB RAW] baud=9600 burst=1 len=... HEX: ... ASCII: ...
```

Once per second it also prints:

```text
[STAT] ring=... adc_drop=... usb_starve=... usb=... rate=... apb_baud=9600 apb_bytes=... apb_bursts=...
```

---
# Troubleshooting

## APB8202 passive UART is silent

Check:

- APB TXD pin 5 -> ESP32 GPIO18 RX
- common ground
- APB powered from the project's 3.3 V rail
- APB RXD pin 6 is still unconnected
- APB CTS pin 7 is still unconnected
- select a baud with `:baud ...`
- power-cycle/reset the APB8202 after selecting the baud if you are looking for boot-only traffic
- inspect both HEX and ASCII output rather than assuming text

Useful starting rates are 9600, 38400, 115200 and 921600. If none reveal stable framing, try additional rates using `:baud <number>`.

The passive monitor never transmits to the APB8202, so protocol discovery cannot accidentally issue an unknown command.

## NRG does not power on

The board is probably not sourcing 5 V VBUS.

Measure NRG `+5V` to `DGND`.

## NRG powers but there is no USB device message

Check:

- GPIO19 is D-
- GPIO20 is D+
- cable carries USB data
- connector is the native USB-OTG port
- common ground
- 5 V VBUS
- core is actually 2.0.17
- Flash Size is 16MB
- Flash Mode is QIO
- PSRAM is OPI PSRAM

## "no supported UAC1 stereo 16-bit 48k/44.1k output stream"

The NRG is advertising a different USB Audio descriptor than expected.

Capture the Serial output / descriptor information and the parser can be extended for the exact stream.

## Audio crackles / drops

Watch:

```text
adc_drop
usb_starve
```

A rising `adc_drop` means the USB side is not consuming captured PCM quickly enough.

A rising `usb_starve` means the USB side is consuming PCM faster than it is arriving or timing is unstable.

The first version uses a simple rate converter and the ESP32-S3 internal ADC, so it is a functional prototype rather than the final hi-fi version.

## Better final audio quality

A later revision can replace the internal ADC with a stereo I2S ADC.

The overall architecture remains:

```text
APB8202 -> digital capture -> ESP32-S3 -> USB host -> NRG
```

---

# Repository build check

GitHub Actions is pinned to **Arduino-ESP32 2.0.17** and compiles with the N16R8 memory settings:

```text
CPUFreq=240
FlashMode=qio
FlashSize=16M
PartitionScheme=app3M_fat9M_16MB
PSRAM=opi
```

A green workflow means the checked-in sketch compiled using both the required core version and the actual ESP32-S3-WROOM-1-N16R8 flash/PSRAM configuration.
