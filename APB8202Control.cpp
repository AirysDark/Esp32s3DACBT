#include "APB8202Control.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "ProjectConfig.h"

namespace {

static uint32_t currentBaud =
    ProjectConfig::APB_UART_DEFAULT_BAUD;

static char terminalLine[128];
static size_t terminalLength = 0;

static bool apbAtLineStart = true;

void printHelp()
{
  Serial.println();
  Serial.println("[APB] Serial Monitor control:");
  Serial.println("  Type any AT command and press Enter -> sent exactly + CRLF");
  Serial.println("  :probe       -> AT");
  Serial.println("  :name        -> AT+NAME?");
  Serial.println("  :play        -> AT+PLAY");
  Serial.println("  :next        -> AT+NEXT");
  Serial.println("  :prev        -> AT+PREV");
  Serial.println("  :vol+        -> AT+VOL+");
  Serial.println("  :vol-        -> AT+VOL-");
  Serial.println("  :disc        -> AT+DISC");
  Serial.println("  :reset       -> AT+RST");
  Serial.println("  :baud 9600   -> change APB UART baud");
  Serial.println("  :baud 115200 -> change APB UART baud");
  Serial.println("  :help        -> this help");
  Serial.println();
  Serial.println(
      "[APB] AT command names above are candidate commands until verified on the real module.");
}

bool configureUart(uint32_t baud)
{
  Serial1.end();
  delay(20);

  Serial1.setRxBufferSize(1024);

  Serial1.begin(
      baud,
      SERIAL_8N1,
      ProjectConfig::APB_UART_RX_GPIO,
      ProjectConfig::APB_UART_TX_GPIO);

  if (ProjectConfig::APB_USE_HARDWARE_FLOW_CONTROL) {
    const bool pinsOk = Serial1.setPins(
        ProjectConfig::APB_UART_RX_GPIO,
        ProjectConfig::APB_UART_TX_GPIO,
        -1,
        ProjectConfig::APB_UART_RTS_GPIO);

    const bool flowOk =
        Serial1.setHwFlowCtrlMode(UART_HW_FLOWCTRL_RTS);

    if (!pinsOk || !flowOk) {
      Serial.println(
          "[APB] ERROR: failed to enable UART RTS flow control");
      return false;
    }

    Serial.printf(
        "[APB] UART: %lu baud, RX=GPIO%u, TX=GPIO%u, RTS=GPIO%u -> APB CTS\n",
        (unsigned long)baud,
        ProjectConfig::APB_UART_RX_GPIO,
        ProjectConfig::APB_UART_TX_GPIO,
        ProjectConfig::APB_UART_RTS_GPIO);
  } else {
    Serial.printf(
        "[APB] UART: %lu baud, RX=GPIO%u, TX=GPIO%u, no HW flow control\n",
        (unsigned long)baud,
        ProjectConfig::APB_UART_RX_GPIO,
        ProjectConfig::APB_UART_TX_GPIO);

    Serial.println(
        "[APB] Hardware: APB CTS pin 7 must be tied to GND.");
  }

  currentBaud = baud;
  return true;
}

void printApbByte(uint8_t value)
{
  if (apbAtLineStart) {
    Serial.print("[APB RX] ");
    apbAtLineStart = false;
  }

  if (value == '\r') {
    Serial.write('\r');
    return;
  }

  if (value == '\n') {
    Serial.write('\n');
    apbAtLineStart = true;
    return;
  }

  if (value == '\t' || (value >= 32 && value <= 126)) {
    Serial.write(value);
    return;
  }

  // Keep binary/status bytes readable instead of dumping terminal garbage.
  Serial.printf("<%02X>", value);
}

void handleTerminalLine(char *line)
{
  if (line[0] == '\0') {
    return;
  }

  if (strcmp(line, ":help") == 0) {
    printHelp();
    return;
  }

  if (strcmp(line, ":probe") == 0) {
    APB8202Control::sendAT();
    return;
  }

  if (strcmp(line, ":name") == 0) {
    APB8202Control::queryName();
    return;
  }

  if (strcmp(line, ":play") == 0) {
    APB8202Control::playPause();
    return;
  }

  if (strcmp(line, ":next") == 0) {
    APB8202Control::nextTrack();
    return;
  }

  if (strcmp(line, ":prev") == 0) {
    APB8202Control::previousTrack();
    return;
  }

  if (strcmp(line, ":vol+") == 0) {
    APB8202Control::volumeUp();
    return;
  }

  if (strcmp(line, ":vol-") == 0) {
    APB8202Control::volumeDown();
    return;
  }

  if (strcmp(line, ":disc") == 0) {
    APB8202Control::disconnect();
    return;
  }

  if (strcmp(line, ":reset") == 0) {
    APB8202Control::resetModule();
    return;
  }

  if (strncmp(line, ":baud ", 6) == 0) {
    const uint32_t baud =
        strtoul(line + 6, NULL, 10);

    if (baud == 0) {
      Serial.println("[APB] Invalid baud rate");
      return;
    }

    APB8202Control::setBaud(baud);
    return;
  }

  // Anything not beginning with a local ':' command is sent directly to APB.
  APB8202Control::sendCommand(line);
}

void readSerialMonitor()
{
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();

    if (c == '\r' || c == '\n') {
      if (terminalLength > 0) {
        terminalLine[terminalLength] = '\0';
        handleTerminalLine(terminalLine);
        terminalLength = 0;
      }
      continue;
    }

    if (terminalLength < sizeof(terminalLine) - 1) {
      terminalLine[terminalLength++] = c;
    } else {
      terminalLength = 0;
      Serial.println("[APB] Terminal command too long; discarded");
    }
  }
}

void readApbUart()
{
  while (Serial1.available() > 0) {
    const int value = Serial1.read();

    if (value >= 0) {
      printApbByte((uint8_t)value);
    }
  }
}

} // namespace

namespace APB8202Control {

bool begin()
{
  Serial.println();
  Serial.println("=== APB8202 / CW6638M UART control ===");

  if (!configureUart(ProjectConfig::APB_UART_DEFAULT_BAUD)) {
    return false;
  }

  printHelp();

  // Do not automatically send control commands at boot. The exact command
  // dictionary still needs to be verified against this APB8202 firmware.
  return true;
}

void update()
{
  readApbUart();
  readSerialMonitor();
}

void sendCommand(const char *command)
{
  if (command == NULL || command[0] == '\0') {
    return;
  }

  Serial.printf("[APB TX] %s\\r\\n\n", command);

  Serial1.print(command);
  Serial1.print("\r\n");
}

void setBaud(uint32_t baud)
{
  if (baud == 0 || baud == currentBaud) {
    return;
  }

  Serial.printf(
      "[APB] Changing UART from %lu to %lu baud\n",
      (unsigned long)currentBaud,
      (unsigned long)baud);

  configureUart(baud);
}

uint32_t baudRate()
{
  return currentBaud;
}

void sendAT()
{
  sendCommand("AT");
}

void queryName()
{
  sendCommand("AT+NAME?");
}

void resetModule()
{
  sendCommand("AT+RST");
}

void disconnect()
{
  sendCommand("AT+DISC");
}

void playPause()
{
  sendCommand("AT+PLAY");
}

void nextTrack()
{
  sendCommand("AT+NEXT");
}

void previousTrack()
{
  sendCommand("AT+PREV");
}

void volumeUp()
{
  sendCommand("AT+VOL+");
}

void volumeDown()
{
  sendCommand("AT+VOL-");
}

} // namespace APB8202Control
