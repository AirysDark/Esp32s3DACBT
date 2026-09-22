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
// APB8202 / CW6638M discovery UART
// -----------------------------------------------------------------------------

// Passive monitor wiring:
//   APB8202 TXD pin 5 -> ESP32-S3 GPIO18 RX
//   APB8202 RXD pin 6 -> leave unconnected
//   APB8202 CTS pin 7 -> leave unconnected
//
// During protocol discovery the ESP32 deliberately does not drive APB RXD/CTS.
static const uint8_t APB_UART_RX_GPIO = 18;

// Reserved for possible future use after the real protocol has been identified.
// Do not wire this during passive discovery.
static const uint8_t APB_UART_TX_GPIO_RESERVED = 17;

// Start at 9600 and manually cycle through the likely rates while rebooting the
// APB8202 to capture its true boot/output stream.
static const uint32_t APB_MONITOR_DEFAULT_BAUD = 9600;

// Group captured bytes into bursts after this much UART silence.
static const uint32_t APB_MONITOR_BURST_GAP_MS = 12;

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
