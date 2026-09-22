#pragma once

#include <stdint.h>

namespace ProjectConfig {

// -----------------------------------------------------------------------------
// Hardware
// -----------------------------------------------------------------------------

static const uint8_t AUDIO_LEFT_GPIO  = 4;
static const uint8_t AUDIO_RIGHT_GPIO = 5;

static const uint8_t USB_D_MINUS_GPIO = 19;
static const uint8_t USB_D_PLUS_GPIO  = 20;

// -----------------------------------------------------------------------------
// Audio capture
// -----------------------------------------------------------------------------

// Arduino-ESP32 2.0.17 / ESP-IDF 4.4 ADC digital driver is documented around
// an 83.3 kHz aggregate conversion ceiling. Two channels at 40 kHz = 80 kHz.
static const uint32_t ADC_PER_CHANNEL_RATE = 40000;
static const uint32_t ADC_TOTAL_RATE       = 80000;

// Raw 12-bit centered ADC data -> signed 16-bit PCM gain.
static const int32_t PCM_GAIN = 16;

// -----------------------------------------------------------------------------
// PCM buffering
// -----------------------------------------------------------------------------

// Must remain a power of two.
static const uint32_t PCM_RING_FRAMES = 8192;
static const uint32_t PCM_RING_MASK   = PCM_RING_FRAMES - 1;

// -----------------------------------------------------------------------------
// USB audio
// -----------------------------------------------------------------------------

static const uint32_t USB_RATE_PREFERRED = 48000;
static const uint32_t USB_RATE_FALLBACK  = 44100;
static const int ISO_TRANSFER_COUNT      = 8;

// Wait for a little captured audio before USB streaming begins.
static const uint32_t USB_PREFILL_FRAMES = 400;

} // namespace ProjectConfig
