#include "APB8202Monitor.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "ProjectConfig.h"

namespace {

static uint32_t currentBaud =
    ProjectConfig::APB_MONITOR_DEFAULT_BAUD;

static uint32_t totalBytes = 0;
static uint32_t totalBursts = 0;

static uint8_t rawBurst[96];
static size_t rawBurstLength = 0;
static uint32_t lastByteMs = 0;

static char terminalLine[96];
static size_t terminalLength = 0;

static const uint32_t kProbeBauds[] = {
    9600,
    38400,
    115200,
    921600
};

static const size_t kProbeBaudCount =
    sizeof(kProbeBauds) / sizeof(kProbeBauds[0]);

void printHelp()
{
  Serial.println();
  Serial.println("[APB MON] Passive RX-only monitor");
  Serial.println("  APB TXD pin 5 -> ESP32 GPIO18 RX");
  Serial.println("  APB RXD pin 6 -> leave unconnected");
  Serial.println("  APB CTS pin 7 -> leave unconnected");
  Serial.println();
  Serial.println("Local Serial Monitor commands:");
  Serial.println("  :baud 9600");
  Serial.println("  :baud 38400");
  Serial.println("  :baud 115200");
  Serial.println("  :baud 921600");
  Serial.println("  :nextbaud");
  Serial.println("  :stats");
  Serial.println("  :clear");
  Serial.println("  :help");
  Serial.println();
  Serial.println(
      "[APB MON] No data is ever transmitted to APB8202 in discovery mode.");
}

bool configureRxOnly(uint32_t baud)
{
  if (baud == 0) {
    return false;
  }

  if (rawBurstLength > 0) {
    // Flush any bytes captured using the old baud before switching.
    // The helper is declared below.
  }

  Serial1.end();

  Serial1.setRxBufferSize(2048);

  // RX-only. TX is deliberately disabled (-1), so this firmware cannot send
  // accidental AT strings or unknown packets into the APB8202.
  Serial1.begin(
      baud,
      SERIAL_8N1,
      ProjectConfig::APB_UART_RX_GPIO,
      -1);

  currentBaud = baud;

  Serial.printf(
      "[APB MON] RX-only GPIO%u @ %lu baud, 8N1\n",
      ProjectConfig::APB_UART_RX_GPIO,
      (unsigned long)currentBaud);

  Serial.println(
      "[APB MON] Leave APB RXD pin 6 and CTS pin 7 unconnected.");

  return true;
}

void printBurst()
{
  if (rawBurstLength == 0) {
    return;
  }

  ++totalBursts;

  Serial.printf(
      "[APB RAW] baud=%lu burst=%lu len=%u HEX:",
      (unsigned long)currentBaud,
      (unsigned long)totalBursts,
      (unsigned int)rawBurstLength);

  for (size_t i = 0; i < rawBurstLength; ++i) {
    Serial.printf(" %02X", rawBurst[i]);
  }

  Serial.print("  ASCII: ");

  for (size_t i = 0; i < rawBurstLength; ++i) {
    const uint8_t b = rawBurst[i];

    if (b >= 32 && b <= 126) {
      Serial.write(b);
    } else {
      Serial.write('.');
    }
  }

  Serial.println();

  rawBurstLength = 0;
}

void captureUart()
{
  while (Serial1.available() > 0) {
    const int value = Serial1.read();

    if (value < 0) {
      break;
    }

    if (rawBurstLength >= sizeof(rawBurst)) {
      printBurst();
    }

    rawBurst[rawBurstLength++] =
        static_cast<uint8_t>(value);

    ++totalBytes;
    lastByteMs = millis();
  }

  if (rawBurstLength > 0 &&
      (millis() - lastByteMs) >=
          ProjectConfig::APB_MONITOR_BURST_GAP_MS) {
    printBurst();
  }
}

void printStats()
{
  Serial.printf(
      "[APB MON] baud=%lu bytes=%lu bursts=%lu\n",
      (unsigned long)currentBaud,
      (unsigned long)totalBytes,
      (unsigned long)totalBursts);
}

void handleTerminalLine(char *line)
{
  if (line == NULL || line[0] == '\0') {
    return;
  }

  if (strcmp(line, ":help") == 0) {
    printHelp();
    return;
  }

  if (strcmp(line, ":stats") == 0) {
    printStats();
    return;
  }

  if (strcmp(line, ":clear") == 0) {
    APB8202Monitor::clearCounters();
    Serial.println("[APB MON] counters cleared");
    return;
  }

  if (strcmp(line, ":nextbaud") == 0) {
    APB8202Monitor::nextBaud();
    return;
  }

  if (strncmp(line, ":baud ", 6) == 0) {
    const uint32_t baud =
        strtoul(line + 6, NULL, 10);

    if (!APB8202Monitor::setBaud(baud)) {
      Serial.println("[APB MON] invalid baud");
    }

    return;
  }

  Serial.println(
      "[APB MON] TX is disabled. Only local ':' monitor commands are accepted.");
}

void readSerialMonitor()
{
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());

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
      Serial.println(
          "[APB MON] terminal command too long; discarded");
    }
  }
}

} // namespace

namespace APB8202Monitor {

bool begin()
{
  Serial.println();
  Serial.println("=== APB8202 / CW6638M passive UART monitor ===");

  totalBytes = 0;
  totalBursts = 0;
  rawBurstLength = 0;
  terminalLength = 0;
  lastByteMs = millis();

  if (!configureRxOnly(
          ProjectConfig::APB_MONITOR_DEFAULT_BAUD)) {
    return false;
  }

  printHelp();
  return true;
}

void update()
{
  captureUart();
  readSerialMonitor();
}

bool setBaud(uint32_t baud)
{
  if (baud == 0) {
    return false;
  }

  if (rawBurstLength > 0) {
    printBurst();
  }

  if (baud == currentBaud) {
    Serial.printf(
        "[APB MON] already at %lu baud\n",
        (unsigned long)baud);
    return true;
  }

  return configureRxOnly(baud);
}

bool nextBaud()
{
  size_t nextIndex = 0;

  for (size_t i = 0; i < kProbeBaudCount; ++i) {
    if (kProbeBauds[i] == currentBaud) {
      nextIndex = (i + 1) % kProbeBaudCount;
      return setBaud(kProbeBauds[nextIndex]);
    }
  }

  return setBaud(kProbeBauds[0]);
}

uint32_t baudRate()
{
  return currentBaud;
}

uint32_t bytesSeen()
{
  return totalBytes;
}

uint32_t burstsSeen()
{
  return totalBursts;
}

void clearCounters()
{
  totalBytes = 0;
  totalBursts = 0;
  rawBurstLength = 0;
}

} // namespace APB8202Monitor
