#pragma once

#include <stdint.h>

namespace ProjectConfig {

static const uint8_t AUDIO_LEFT_GPIO  = 4;
static const uint8_t AUDIO_RIGHT_GPIO = 5;

static const uint8_t USB_D_MINUS_GPIO = 19;
static const uint8_t USB_D_PLUS_GPIO  = 20;

// APB8202 / CW6638M receive-only discovery:
//   module pin 5 signal -> ESP32-S3 GPIO18 RX
//   module pin 6 -> disconnected (ESP32 UART TX disabled)
//   module pin 7 CTS -> disconnected
static const int8_t APB_UART_RX_GPIO = 18;
static const int8_t APB_UART_TX_GPIO = -1;

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
