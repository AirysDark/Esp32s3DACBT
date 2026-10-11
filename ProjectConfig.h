#pragma once

#include <stdint.h>

namespace ProjectConfig {

static const uint8_t AUDIO_LEFT_GPIO  = 4;
static const uint8_t AUDIO_RIGHT_GPIO = 5;

static const uint8_t USB_D_MINUS_GPIO = 19;
static const uint8_t USB_D_PLUS_GPIO  = 20;

// APB passive observation pins.
static const int8_t APB_PIN5_GPIO = 18;
static const int8_t APB_PIN6_GPIO = 17;

// ESP32-controlled APB power switch.
// GPIO8 -> IRLZ44N pin 1 (Gate) directly.
// 10k from IRLZ44N pin 1 (Gate) to common GND keeps the APB OFF during reset/boot.
static const uint8_t APB_POWER_GPIO = 8;
static const uint32_t APB_POWER_OFF_SETTLE_MS = 500;
static const uint32_t APB_CAPTURE_MS = 6500;

static const uint32_t APB_MONITOR_DEFAULT_BAUD = 9600;
static const uint32_t APB_MONITOR_BURST_GAP_MS = 12;

static const uint32_t ADC_PER_CHANNEL_RATE = 40000;
static const uint32_t ADC_TOTAL_RATE       = 80000;
static const int32_t PCM_GAIN = 16;

static const uint32_t PCM_RING_FRAMES = 8192;
static const uint32_t PCM_RING_MASK   = PCM_RING_FRAMES - 1;

static const uint32_t USB_RATE_PREFERRED = 48000;
static const uint32_t USB_RATE_FALLBACK  = 44100;
static const int ISO_TRANSFER_COUNT      = 8;
static const uint32_t USB_PREFILL_FRAMES = 400;

} // namespace ProjectConfig
