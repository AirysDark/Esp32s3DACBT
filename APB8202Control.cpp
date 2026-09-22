#include "APB8202Control.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "ProjectConfig.h"

namespace {

static uint32_t currentBaud =
    ProjectConfig::APB_UART_DEFAULT_BAUD;

static APB8202Control::ApbState currentState =
    APB8202Control::IDLE;

// APB UART receive line parser.
static char apbLine[160];
static size_t apbLineLength = 0;
static bool apbSawCR = false;

// Serial Monitor local command parser.
static char terminalLine[160];
static size_t terminalLength = 0;

// CALL:<number> storage. Digits only, NUL terminated.
static char lastCallerNumber[40] = "";

void printHelp()
{
  Serial.println();
  Serial.println("[APB] Serial Monitor control:");
  Serial.println("  Any raw AT command + Enter -> forwarded with CRLF");
  Serial.println("  :probe        -> AT");
  Serial.println("  :addr         -> AT+LADDR?");
  Serial.println("  :baud?        -> AT+BAUD?");
  Serial.println("  :baudidx N    -> AT+BAUD<N>  (example N=4,6,8)");
  Serial.println("  :play         -> AT+PLAY");
  Serial.println("  :pause        -> AT+PAUSE");
  Serial.println("  :next         -> AT+NEXT");
  Serial.println("  :prev         -> AT+PREV");
  Serial.println("  :vol+         -> AT+VOL+");
  Serial.println("  :vol-         -> AT+VOL-");
  Serial.println("  :vol N        -> AT+VOL=N (0..30)");
  Serial.println("  :disc         -> AT+DISC");
  Serial.println("  :clearpairs   -> AT+RESETPDL");
  Serial.println("  :reset        -> AT+RST");
  Serial.println("  :uart 9600    -> ESP32 Serial1 physical baud");
  Serial.println("  :uart 115200  -> ESP32 Serial1 physical baud");
  Serial.println("  :state        -> show parsed state/caller");
  Serial.println("  :help         -> this help");
  Serial.println();
}

bool configureUart(uint32_t baud)
{
  Serial1.end();

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
        "[APB] UART %lu 8N1 RX=GPIO%u TX=GPIO%u RTS=GPIO%u -> APB CTS\n",
        (unsigned long)baud,
        ProjectConfig::APB_UART_RX_GPIO,
        ProjectConfig::APB_UART_TX_GPIO,
        ProjectConfig::APB_UART_RTS_GPIO);
  } else {
    Serial.printf(
        "[APB] UART %lu 8N1 RX=GPIO%u TX=GPIO%u no HW flow control\n",
        (unsigned long)baud,
        ProjectConfig::APB_UART_RX_GPIO,
        ProjectConfig::APB_UART_TX_GPIO);

    Serial.println(
        "[APB] APB8202 CTS pin 7 MUST be tied directly to GND.");
  }

  currentBaud = baud;
  return true;
}

void setState(APB8202Control::ApbState newState)
{
  if (currentState == newState) {
    return;
  }

  currentState = newState;

  Serial.printf(
      "[APB STATE] %s\n",
      APB8202Control::stateName());
}

void parseCallerNumber(const char *line)
{
  const char *call = strstr(line, "CALL:");

  if (call == NULL) {
    return;
  }

  call += 5;

  size_t out = 0;

  while (*call != '\0' &&
         out < sizeof(lastCallerNumber) - 1) {
    if (*call >= '0' && *call <= '9') {
      lastCallerNumber[out++] = *call;
    }

    ++call;
  }

  lastCallerNumber[out] = '\0';

  Serial.printf(
      "[APB CALL] number=%s\n",
      lastCallerNumber[0] != '\0'
          ? lastCallerNumber
          : "(not supplied)");
}

void processApbLine(const char *line)
{
  if (line == NULL || line[0] == '\0') {
    return;
  }

  Serial.printf("[APB RX] %s\n", line);

  // IMPORTANT: DISCONNECTED contains the substring CONNECTED.
  // Test disconnect first so it cannot be misclassified.
  if (strstr(line, "DISCONNECT") != NULL) {
    setState(APB8202Control::DISCONNECTED);
    return;
  }

  if (strstr(line, "CALL:") != NULL) {
    parseCallerNumber(line);
    setState(APB8202Control::CALL_INCOMING);
    return;
  }

  if (strstr(line, "CONNECTED") != NULL) {
    setState(APB8202Control::CONNECTED);
    return;
  }

  // OK, ERROR, +LADDR:, +BAUD:, +VOL: and other command responses are
  // intentionally logged above but do not alter the connection state.
}

void finishApbLine()
{
  if (apbLineLength == 0) {
    return;
  }

  apbLine[apbLineLength] = '\0';
  processApbLine(apbLine);
  apbLineLength = 0;
}

void consumeApbByte(char c)
{
  // Protocol lines are CRLF terminated. Keep this non-blocking and
  // character-by-character.
  if (c == '\r') {
    apbSawCR = true;
    return;
  }

  if (c == '\n') {
    if (apbSawCR || apbLineLength > 0) {
      finishApbLine();
    }

    apbSawCR = false;
    return;
  }

  // A CR that was not followed by LF is treated as a delimiter to avoid
  // merging malformed/firmware-specific output into the next line.
  if (apbSawCR) {
    finishApbLine();
    apbSawCR = false;
  }

  if (apbLineLength < sizeof(apbLine) - 1) {
    apbLine[apbLineLength++] = c;
  } else {
    apbLineLength = 0;
    apbSawCR = false;
    Serial.println(
        "[APB] RX line overflow; discarded");
  }
}

void readApbUart()
{
  while (Serial1.available() > 0) {
    const int value = Serial1.read();

    if (value >= 0) {
      consumeApbByte((char)value);
    }
  }
}

void printState()
{
  Serial.printf(
      "[APB] state=%s uart=%lu caller=%s\n",
      APB8202Control::stateName(),
      (unsigned long)APB8202Control::baudRate(),
      APB8202Control::callerNumber()[0] != '\0'
          ? APB8202Control::callerNumber()
          : "(none)");
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

  if (strcmp(line, ":probe") == 0) {
    APB8202Control::sendAT();
    return;
  }

  if (strcmp(line, ":addr") == 0) {
    APB8202Control::queryAddress();
    return;
  }

  if (strcmp(line, ":baud?") == 0) {
    APB8202Control::queryBaud();
    return;
  }

  if (strncmp(line, ":baudidx ", 9) == 0) {
    const long index = strtol(line + 9, NULL, 10);

    if (index < 1 || index > 8) {
      Serial.println("[APB] baud index must be 1..8");
      return;
    }

    APB8202Control::setBaudIndex((uint8_t)index);
    return;
  }

  if (strcmp(line, ":play") == 0) {
    APB8202Control::playPause();
    return;
  }

  if (strcmp(line, ":pause") == 0) {
    APB8202Control::pause();
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

  if (strncmp(line, ":vol ", 5) == 0) {
    const long volume = strtol(line + 5, NULL, 10);

    if (volume < 0 || volume > 30) {
      Serial.println("[APB] volume must be 0..30");
      return;
    }

    APB8202Control::setVolume((uint8_t)volume);
    return;
  }

  if (strcmp(line, ":disc") == 0) {
    APB8202Control::disconnect();
    return;
  }

  if (strcmp(line, ":clearpairs") == 0) {
    APB8202Control::clearPairHistory();
    return;
  }

  if (strcmp(line, ":reset") == 0) {
    APB8202Control::resetModule();
    return;
  }

  if (strncmp(line, ":uart ", 6) == 0) {
    const uint32_t baud =
        strtoul(line + 6, NULL, 10);

    if (baud == 0) {
      Serial.println("[APB] invalid UART baud");
      return;
    }

    APB8202Control::setUartBaud(baud);
    return;
  }

  if (strcmp(line, ":state") == 0) {
    printState();
    return;
  }

  // Anything else is a raw APB command.
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
      Serial.println(
          "[APB] terminal command too long; discarded");
    }
  }
}

} // namespace

namespace APB8202Control {

bool begin()
{
  Serial.println();
  Serial.println("=== APB8202 / CW6638M control ===");

  currentState = IDLE;
  lastCallerNumber[0] = '\0';
  apbLineLength = 0;
  apbSawCR = false;
  terminalLength = 0;

  if (!configureUart(
          ProjectConfig::APB_UART_DEFAULT_BAUD)) {
    return false;
  }

  printHelp();

  // Ping the module once. No blocking wait is used; any response is processed
  // asynchronously by update().
  sendAT();

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

ApbState state()
{
  return currentState;
}

const char *stateName()
{
  switch (currentState) {
    case IDLE:
      return "IDLE";

    case DISCONNECTED:
      return "DISCONNECTED";

    case CONNECTED:
      return "CONNECTED";

    case CALL_INCOMING:
      return "CALL_INCOMING";

    default:
      return "UNKNOWN";
  }
}

const char *callerNumber()
{
  return lastCallerNumber;
}

void setUartBaud(uint32_t baud)
{
  if (baud == 0 || baud == currentBaud) {
    return;
  }

  Serial.printf(
      "[APB] changing ESP32 UART from %lu to %lu baud\n",
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

void resetModule()
{
  sendCommand("AT+RST");
}

void queryAddress()
{
  sendCommand("AT+LADDR?");
}

void queryBaud()
{
  sendCommand("AT+BAUD?");
}

void setBaudIndex(uint8_t index)
{
  if (index < 1 || index > 8) {
    Serial.println("[APB] baud index must be 1..8");
    return;
  }

  char command[16];
  snprintf(
      command,
      sizeof(command),
      "AT+BAUD%u",
      index);

  sendCommand(command);
}

void disconnect()
{
  sendCommand("AT+DISC");
}

void playPause()
{
  sendCommand("AT+PLAY");
}

void pause()
{
  sendCommand("AT+PAUSE");
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

void setVolume(uint8_t volume)
{
  if (volume > 30) {
    volume = 30;
  }

  char command[20];
  snprintf(
      command,
      sizeof(command),
      "AT+VOL=%u",
      volume);

  sendCommand(command);
}

void clearPairHistory()
{
  sendCommand("AT+RESETPDL");
}

} // namespace APB8202Control
