#pragma once

#include <stdint.h>

namespace APB8202Control {

// Starts Serial1 using the pins/settings in ProjectConfig.h.
bool begin();

// Call from Arduino loop(). Bridges APB UART output to the Serial Monitor and
// accepts manual commands typed into the Serial Monitor.
void update();

// Sends an arbitrary command followed by CRLF.
void sendCommand(const char *command);

// Runtime baud-rate change for testing 9600 / 115200 etc.
void setBaud(uint32_t baud);
uint32_t baudRate();

// Candidate APB8202/CW6638M AT-command helpers.
// These command strings are implemented for hardware testing but must be
// verified against the actual module firmware.
void sendAT();
void queryName();
void resetModule();
void disconnect();
void playPause();
void nextTrack();
void previousTrack();
void volumeUp();
void volumeDown();

} // namespace APB8202Control
