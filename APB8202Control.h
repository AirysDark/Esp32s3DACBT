#pragma once

#include <stdint.h>

namespace APB8202Control {

enum ApbState {
  IDLE,
  DISCONNECTED,
  CONNECTED,
  CALL_INCOMING
};

// Starts Serial1 using the pins/settings in ProjectConfig.h.
bool begin();

// Non-blocking service routine. Call this as often as possible from loop().
void update();

// Sends an arbitrary APB8202 command followed by CRLF.
void sendCommand(const char *command);

// Current state parsed from asynchronous APB8202 notifications.
ApbState state();
const char *stateName();

// Digits parsed from the most recent CALL:<number> notification.
const char *callerNumber();

// Physical UART speed used by ESP32 Serial1.
void setUartBaud(uint32_t baud);
uint32_t baudRate();

// Verified APB8202 / CW6638M command helpers.
void sendAT();
void resetModule();
void queryAddress();
void queryBaud();
void setBaudIndex(uint8_t index);
void disconnect();

void playPause();
void pause();
void nextTrack();
void previousTrack();

void volumeUp();
void volumeDown();
void setVolume(uint8_t volume);

void clearPairHistory();

} // namespace APB8202Control
