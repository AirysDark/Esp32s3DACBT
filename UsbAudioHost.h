#pragma once

#include <stdint.h>

namespace UsbAudioHost {

bool begin();

bool isStreaming();
uint32_t sampleRate();

} // namespace UsbAudioHost
