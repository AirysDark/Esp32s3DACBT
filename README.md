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

## Standalone serial bridge / sniffing sketch

This standalone sketch can be flashed when you want a very simple UART bridge/scanner instead of the full project firmware. It prints every received APB8202 byte in hexadecimal and can also forward PC Serial Monitor bytes to the APB UART.

**For strict passive sniffing, leave APB8202 RXD pin 6 physically disconnected from ESP32 GPIO17.** If GPIO17 is connected, the bottom half of this sketch makes it an active two-way serial bridge.

Change `TEST_BAUD` and re-flash when testing another baud rate.

```cpp
#include <Arduino.h>

#define APB_UART_NUM 1
#define PIN_RX       18  // Connects to APB8202 TXD
#define PIN_TX       17  // Connects to APB8202 RXD

// HCI chips usually boot at 115200, but can scale up to high speeds like 921600.
// If the data looks like random symbols, change this number and re-flash.
#define TEST_BAUD 115200

HardwareSerial APB_Bus(APB_UART_NUM);

void setup() {
    // Open primary USB pipeline to your PC Monitor
    Serial.begin(115200);
    while(!Serial && millis() < 3000);

    Serial.println("\n=======================================================");
    Serial.printf("[INIT] ESP32-S3 Bridge Active. Listening to APB8202 at %d bps\n", TEST_BAUD);
    Serial.println("=======================================================");

    // Initialize the secondary communication bus with 8-N-1 configuration
    APB_Bus.begin(TEST_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
}

void loop() {
    // Read from the Buildwin chip and forward it instantly to your PC screen
    if (APB_Bus.available()) {
        while (APB_Bus.available()) {
            uint8_t b = APB_Bus.read();
            // Prints the raw hex character representation to prevent text parsing corruption
            if (b < 0x10) Serial.print("0");
            Serial.print(b, HEX);
            Serial.print(" ");
        }
        Serial.println(); // Line break after a packet block finishes streaming
    }

    // Read characters typed from your PC keyboard and forward them down to the chip
    if (Serial.available()) {
        while (Serial.available()) {
            APB_Bus.write(Serial.read());
        }
    }
}
```

Suggested first baud values to try:

```text
9600
38400
115200
921600
```
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


## Experimental: possible Bluetooth name change

**Status: unverified research path.** The current project does not know a confirmed runtime command for changing the APB8202/CW6638M Bluetooth broadcast name. The working assumption is that the name may be stored in a boot-time configuration/parameter area rather than exposed through a simple ASCII `AT+NAME=` command.

Possible reverse-engineering paths:

1. inspect any external SPI flash/EEPROM on the APB8202 board for a stored device-name string or parameter block;
2. investigate the factory test pads (`TP1`-`TP6`) for a vendor programming/configuration mode;
3. continue UART/HCI sniffing to look for vendor-specific parameter traffic during boot or factory configuration.

### Method 1: external SPI flash dump with a SOIC/SOP8 clip

If the APB8202 board contains a separate 8-pin SPI flash chip, a SOIC/SOP8 test clip may allow the ESP32-S3 to read it without soldering directly to the flash pins.

> Important: a test clip does **not** electrically isolate an in-circuit flash chip by itself. The APB8202 board should not be powered from its normal supply at the same time unless the flash and board power topology has been verified. Identify the exact memory part, its voltage, pinout and capacity before connecting or writing anything.

Proposed ESP32-S3 SPI wiring for a standard 3.3 V 25-series flash:

```text
ESP32-S3                SOP8 flash
----------------------------------------------
GPIO10  --------------> Pin 1  CS
GPIO13  <-------------- Pin 2  MISO / DO
3V3     --------------> Pin 3  WP#  (high)
GND     --------------> Pin 4  GND
GPIO11  --------------> Pin 5  MOSI / DI
GPIO12  --------------> Pin 6  CLK
3V3     --------------> Pin 7  HOLD#/RESET# (high)
3V3     --------------> Pin 8  VCC
```

Before any write attempt:

- read the JEDEC ID (`0x9F`) and identify the actual flash part;
- derive the real capacity from the part/JEDEC ID instead of assuming 1 MB;
- make at least two complete dumps and verify that they are byte-for-byte identical;
- keep an untouched backup;
- search for the current Bluetooth name in ASCII and, if needed, UTF-16/other encodings;
- do not assume that replacing a same-length string is sufficient: the parameter area may use checksums, CRCs, lengths, pointers, compression or encryption.

Diagnostic ESP32-S3 raw SPI reader:

```cpp
#include <Arduino.h>
#include <SPI.h>

#define FLASH_CS   10
#define FLASH_MISO 13
#define FLASH_MOSI 11
#define FLASH_CLK  12

#define CMD_READ_ID   0x9F
#define CMD_READ_DATA 0x03

void setup() {
    Serial.begin(115200);
    pinMode(FLASH_CS, OUTPUT);
    digitalWrite(FLASH_CS, HIGH);

    SPI.begin(FLASH_CLK, FLASH_MISO, FLASH_MOSI, FLASH_CS);
    SPI.setDataMode(SPI_MODE0);
    SPI.setBitOrder(MSBFIRST);
    SPI.setFrequency(1000000); // Conservative diagnostic clock

    delay(2000);

    // Read JEDEC ID first.
    digitalWrite(FLASH_CS, LOW);
    SPI.transfer(CMD_READ_ID);
    uint8_t mfg  = SPI.transfer(0x00);
    uint8_t type = SPI.transfer(0x00);
    uint8_t cap  = SPI.transfer(0x00);
    digitalWrite(FLASH_CS, HIGH);

    Serial.printf(
        "[SPI] JEDEC ID -> MFG: 0x%02X, Type: 0x%02X, Cap: 0x%02X\n",
        mfg, type, cap);

    Serial.println("--- START OF RAW FLASH DUMP ---");

    // Diagnostic example only: this reads the first 1 MiB.
    // Change the range only after identifying the actual flash capacity.
    for (uint32_t addr = 0; addr < 1048576; addr += 16) {
        uint8_t chunk[16];

        digitalWrite(FLASH_CS, LOW);
        SPI.transfer(CMD_READ_DATA);
        SPI.transfer((addr >> 16) & 0xFF);
        SPI.transfer((addr >> 8) & 0xFF);
        SPI.transfer(addr & 0xFF);

        for (int i = 0; i < 16; i++) {
            chunk[i] = SPI.transfer(0x00);
        }

        digitalWrite(FLASH_CS, HIGH);

        for (int i = 0; i < 16; i++) {
            if (chunk[i] < 0x10) Serial.print("0");
            Serial.print(chunk[i], HEX);
            Serial.print(" ");
        }
        Serial.println();
    }

    Serial.println("--- END OF RAW FLASH DUMP ---");
}

void loop() {}
```

This code is intentionally **read-only**. A flash-writing routine should not be added until the exact memory device, writable regions and any configuration integrity/checksum scheme are known.

### Method 2: factory test pads (`TP1`-`TP6`)

The module exposes production/test pads on pins 8-13. These may provide a factory programming/configuration path, but the exact electrical protocol and boot entry sequence for this APB8202 firmware are not yet confirmed.

Current pin labels:

```text
Pin 8   TP6
Pin 9   TP5
Pin 10  TP4
Pin 11  TP3
Pin 12  TP2
Pin 13  TP1 / test-boot related input
```

Do **not** blindly pull TP1 or the other test pads to GND or 3.3 V. First determine their idle voltages and locate a reliable CW6638M/APB8202 programming procedure or capture the original factory-board behavior.

Vendor utilities sometimes referenced for Buildwin/Appotech devices include tools such as MPTool/ConfigApp, but support for this exact APB8202 firmware and a specific Bluetooth-name parameter has not been confirmed.

### Method 3: UART/HCI parameter discovery

The existing standalone serial bridge/sniffing sketch above remains useful for this path. If the factory firmware loads the Bluetooth name through UART/HCI/vendor-specific packets at boot, those packets may reveal the relevant opcode or parameter structure.

For passive capture use only:

```text
APB8202 TXD pin 5 -> ESP32 GPIO18 RX
APB8202 RXD pin 6 -> leave unconnected
APB8202 CTS pin 7 -> leave unconnected
```

### Method 4: experimental HCI `Write Local Name` injection

If sniffing shows that the APB8202 firmware exposes a standard Bluetooth HCI UART/H4 controller interface, one possible **runtime** experiment is the Bluetooth Core `HCI_Write_Local_Name` command.

The standard opcode is:

```text
HCI_Write_Local_Name = 0x0C13
H4 command packet indicator = 0x01
parameter length = 248 bytes = 0xF8

packet prefix:
01 13 0C F8
```

The remaining 248 parameter bytes contain the local name, NUL-padded to the fixed command length.

Important limitations:

- seeing bytes beginning with `04 0F` or `04 10` is **consistent with H4 HCI event traffic**, but by itself does not prove that this APB8202 firmware exposes a normal host-controllable HCI transport;
- `HCI_Write_Local_Name` is a standard controller command, but there is no current proof that this module accepts it on pins 5/6;
- even if accepted, the name may be **volatile** and revert after reset/power-cycle;
- on Bluetooth 2.1+EDR firmware, discoverability data may also involve Extended Inquiry Response data, so changing the controller's local name does not guarantee every scan result immediately shows the new name;
- the correct UART baud and any required flow-control behavior must be established first;
- do not connect GPIO17 to APB RXD until passive sniffing has established that active transmission is appropriate.

Example injector for a confirmed standard H4/HCI UART path:

```cpp
#include <Arduino.h>
#include <string.h>

#define APB_UART_NUM 1
#define PIN_RX       18  // APB8202 TXD pin 5 -> ESP32 RX
#define PIN_TX       17  // ESP32 TX -> APB8202 RXD pin 6
#define TEST_BAUD    115200 // Replace with the baud proven by sniffing

HardwareSerial APB_Bus(APB_UART_NUM);

const char* newBluetoothName = "CUSTOM_HUAWEI_NODE";

void injectNewBluetoothName() {
    Serial.println("[HCI TEST] Sending HCI_Write_Local_Name...");

    uint8_t hciPacket[252];
    memset(hciPacket, 0, sizeof(hciPacket));

    // H4 command packet + opcode 0x0C13 + 248-byte parameter block.
    hciPacket[0] = 0x01;
    hciPacket[1] = 0x13;
    hciPacket[2] = 0x0C;
    hciPacket[3] = 0xF8;

    size_t nameLen = strlen(newBluetoothName);
    if (nameLen > 247) nameLen = 247;
    memcpy(&hciPacket[4], newBluetoothName, nameLen);

    APB_Bus.write(hciPacket, sizeof(hciPacket));
    APB_Bus.flush();

    Serial.println("[HCI TEST] Packet transmitted; waiting for raw response.");
}

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("=== ESP32-S3 APB8202 HCI name-injection experiment ===");

    APB_Bus.begin(TEST_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

    // Fixed delay is only a first experiment. A better version should trigger
    // from an observed/decoded controller-ready event rather than guessing time.
    delay(500);
    injectNewBluetoothName();
}

void loop() {
    if (APB_Bus.available()) {
        Serial.print("[APB RESPONSE] ");

        while (APB_Bus.available()) {
            uint8_t b = APB_Bus.read();
            if (b < 0x10) Serial.print("0");
            Serial.print(b, HEX);
            Serial.print(" ");
        }

        Serial.println();
    }
}
```

For a standard HCI controller, the useful proof is not simply that bytes were transmitted. The response should be decoded as HCI events and matched to opcode `0x0C13` with a success status before treating the test as accepted.

If that works, test whether the visible Bluetooth name actually changes and whether it survives a complete power cycle. Persistence would imply that the firmware mirrors the value into non-volatile configuration; reversion would indicate a runtime-only controller name.

#### Method 4B: boot-triggered sniffer + HCI name injector

This is the exact **sniff-then-inject** workflow intended for the Bluetooth-name experiment.

**Status: experimental.** The code below assumes that passive sniffing has already shown a usable H4/HCI-style UART on APB8202 pins 5/6 and that the correct baud is known. It should not be treated as a confirmed flash-programming method.

Process:

1. ESP32-S3 boots and listens to APB8202 TXD on GPIO18.
2. Power-cycle/reset the APB8202.
3. ESP32-S3 watches the raw boot stream.
4. When a candidate HCI event packet is detected, the ESP32-S3 sends `HCI_Write_Local_Name` (`0x0C13`) down APB8202 RXD through GPIO17.
5. The response is captured in raw HEX so acceptance can be verified.
6. After a successful command, scan for the new Bluetooth name and then power-cycle the APB8202 to determine whether the change is volatile or persistent.

Target wiring:

```text
ESP32-S3                     APB8202
------------------------------------------------
3V3        ----------------> VIN  pin 3
GND        ----------------> GND  pin 4/14
GPIO18 RX  <---------------- TXD  pin 5
GPIO17 TX  ----------------> RXD  pin 6

LEAVE UNCONNECTED DURING THIS TEST:
CTS pin 7
TP1-TP6 / pins 8-13
```

Do not connect GPIO17 until the passive sniffing stage has established that active transmission is appropriate for the module.

Experimental injector sketch:

```cpp
#include <Arduino.h>
#include <string.h>

#define APB_UART_NUM 1
#define PIN_RX       18  // APB8202 Pin 5 TXD -> ESP32 RX
#define PIN_TX       17  // ESP32 TX -> APB8202 Pin 6 RXD

// Replace with the baud actually discovered during sniffing.
#define MODULE_BAUD_RATE 115200

HardwareSerial APB_Bus(APB_UART_NUM);

// Keep the test name short for easy scan verification.
const char* targetBluetoothName = "REPROGRAMMED_AUDIO";

bool nameInjected = false;

void injectNewDeviceIdentity() {
    Serial.println();
    Serial.println("[HCI TEST] Candidate HCI signature detected; sending Write Local Name...");

    uint8_t packet[252];
    memset(packet, 0, sizeof(packet));

    // H4 command packet:
    //   0x01       = HCI command packet indicator
    //   0x0C13     = HCI_Write_Local_Name opcode, little-endian on wire
    //   0xF8       = 248-byte parameter block
    packet[0] = 0x01;
    packet[1] = 0x13;
    packet[2] = 0x0C;
    packet[3] = 0xF8;

    size_t nameLength = strlen(targetBluetoothName);

    // HCI Write Local Name provides 248 parameter bytes.
    // Leave room for NUL padding.
    if (nameLength > 247) {
        nameLength = 247;
    }

    memcpy(&packet[4], targetBluetoothName, nameLength);

    APB_Bus.write(packet, sizeof(packet));
    APB_Bus.flush();

    nameInjected = true;
    Serial.println("[HCI TEST] 252-byte Write Local Name frame transmitted.");
}

void printRawByte(uint8_t b) {
    if (b < 0x10) Serial.print("0");
    Serial.print(b, HEX);
    Serial.print(" ");
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {
        yield();
    }

    Serial.println();
    Serial.println("=======================================================");
    Serial.println("ESP32-S3 APB8202 SNIFFER + HCI NAME INJECTOR");
    Serial.println("=======================================================");

    APB_Bus.begin(
        MODULE_BAUD_RATE,
        SERIAL_8N1,
        PIN_RX,
        PIN_TX);

    Serial.printf(
        "[STATUS] Listening at %lu baud. Reset/power-cycle APB8202 now.\n",
        (unsigned long)MODULE_BAUD_RATE);
}

void loop() {
    if (!APB_Bus.available()) {
        return;
    }

    uint8_t firstByte = APB_Bus.peek();

    // 0x04 is the H4 packet indicator for an HCI Event packet.
    // It is only a candidate trigger; a production version should decode the
    // complete HCI event packet before deciding that the controller is ready.
    if (!nameInjected && firstByte == 0x04) {
        Serial.println("[SNIFF] Candidate H4 HCI event detected.");

        // Drain and print the currently buffered event bytes before injection.
        Serial.print("[SNIFFED RAW HEX] ");

        while (APB_Bus.available()) {
            printRawByte((uint8_t)APB_Bus.read());
        }

        Serial.println();
        injectNewDeviceIdentity();
        return;
    }

    Serial.print("[APB RAW] ");

    while (APB_Bus.available()) {
        printRawByte((uint8_t)APB_Bus.read());
    }

    Serial.println();
}
```

Important interpretation notes:

- `0x04` means **H4 HCI Event packet indicator**, not specifically "controller ready". A robust injector should decode the event type and payload rather than triggering on every `0x04` byte.
- `HCI_Write_Local_Name` changes the controller's local-name parameter if the command is supported. Calling this a **flash overwrite** is only justified if a later power-cycle proves the APB8202 firmware stores that value non-volatilely.
- the Bluetooth specification allows a 248-byte Local Name parameter; the earlier 32-character clamp was only a conservative UI choice, not the HCI command limit.
- success should be established by decoding the matching HCI Command Complete/Command Status response for opcode `0x0C13`, not merely by seeing bytes transmitted.
- if the name changes but reverts after reset, this method is a runtime override only.
- if no valid HCI acknowledgement is received, return to passive sniffing rather than repeatedly injecting packets.


### Method 5: ESP32-S3 self-advertised name takeover — BLE only

This is a **separate workaround**, not a replacement for the APB8202 A2DP audio link.

The ESP32-S3 can advertise its own programmable **Bluetooth Low Energy (BLE)** device name and store that name in NVS, but it cannot become a Bluetooth Classic A2DP sink. Espressif documents the ESP32-S3 as **Bluetooth LE only**: Bluetooth Classic/BR-EDR is not supported, and ESP32-S3 also does not provide LE Audio. Therefore libraries such as `ESP32-A2DP` / `BluetoothA2DPSink` cannot turn this ESP32-S3 into the phone's normal Bluetooth-audio receiver.

That means an "antenna trace cut + ESP32-S3 A2DP takeover" is **not viable on this hardware**. Cutting or disabling the APB8202 RF path would remove the only currently available Classic Bluetooth audio receiver from this project.

What *is* possible on the existing ESP32-S3:

- expose a separate BLE device with any runtime name you choose;
- save that BLE name in ESP32 NVS using `Preferences.h`; 
- use BLE for configuration/control/status;
- keep the APB8202 handling the actual Classic Bluetooth A2DP audio;
- continue trying to change the APB8202 name through the flash/HCI/factory methods above.

If a future redesign truly needs the ESP chip itself to receive normal phone A2DP audio, use hardware with Bluetooth Classic support (for example the original ESP32 family) or a separate Classic-Bluetooth audio controller.

#### Important correction to the proposed I2S pin example

The proposed `GPIO22` and `GPIO23` pins are not ESP32-S3 GPIOs. ESP32-S3 numbering jumps from GPIO21 to GPIO26. If an I2S DAC is added later, use free GPIOs that actually exist and do not conflict with this project. For example, a candidate mapping could be:

```text
ESP32-S3                I2S DAC
--------------------------------
GPIO6   --------------> BCLK
GPIO7   --------------> LRCK / WS
GPIO8   --------------> DIN
3V3     --------------> VCC   (only if the DAC board supports 3.3 V)
GND     --------------> GND
```

This mapping is only a project pin-allocation example; I2S signals can be routed through the ESP32-S3 GPIO matrix, so the final pins should be selected after checking the exact carrier board and all other project connections.

#### BLE runtime-name concept on ESP32-S3

A BLE name can be made persistent with `Preferences.h`, but this changes only the ESP32-S3's BLE identity. It does **not** rename the APB8202 and does **not** make the ESP32-S3 an A2DP sink.

Conceptual storage flow:

```text
Serial/UI requests new BLE name
        |
        v
Preferences NVS stores string
        |
        v
BLE stack restarts/updates advertising name
        |
        v
ESP32-S3 appears under new BLE name

APB8202 Classic Bluetooth audio name remains separate
```


If the Bluetooth name is eventually located in external flash or a confirmed configuration packet, document the exact offset/opcode, surrounding bytes, checksum behavior and restore procedure before enabling automated modification.

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
