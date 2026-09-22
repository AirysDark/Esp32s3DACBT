/*
  Esp32s3DACBT
  Core target: Arduino-ESP32 2.0.17

  Main sketch intentionally kept small.

  Modules:
    ProjectConfig.h      - hardware and timing constants
    AudioBuffer.*        - stereo PCM ring buffer + counters
    AdcAudio.*           - APB8202 analog capture using ADC DMA
    APB8202Monitor.*     - passive RX-only raw UART monitor
    UsbAudioHost.*       - USB Audio Class 1 host for the NRG

  Signal paths:
    APB8202 L/R -> ESP32-S3 ADC -> PCM buffer -> USB Host -> NRG
    APB8202 TXD  -> ESP32-S3 GPIO18 RX for passive UART/HCI discovery

  During discovery:
    APB8202 RXD is NOT connected.
    APB8202 CTS is NOT connected.
    No commands are transmitted to the APB8202.
*/

#include <Arduino.h>

#include "APB8202Monitor.h"
#include "AdcAudio.h"
#include "AudioBuffer.h"
#include "ProjectConfig.h"
#include "UsbAudioHost.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Esp32s3DACBT requires ESP32-S3"
#endif

void setup()
{
  Serial.begin(115200);

  Serial.println();
  Serial.println("============================================");
  Serial.println(" Esp32s3DACBT - Arduino-ESP32 core 2.0.17");
  Serial.println(" APB8202 -> ADC + passive UART -> USB NRG");
  Serial.println("============================================");

  if (!APB8202Monitor::begin()) {
    Serial.println("[FATAL] APB8202 passive UART monitor setup failed");

    while (true) {
      yield();
    }
  }

  if (!AdcAudio::begin()) {
    Serial.println("[FATAL] ADC setup failed");

    while (true) {
      yield();
    }
  }

  if (!UsbAudioHost::begin()) {
    Serial.println("[FATAL] USB host setup failed");

    while (true) {
      yield();
    }
  }
}

void loop()
{
  APB8202Monitor::update();

  static uint32_t lastReportMs = 0;

  if (millis() - lastReportMs >= 1000) {
    lastReportMs = millis();

    Serial.printf(
        "[STAT] ring=%lu/%lu adc_drop=%lu usb_starve=%lu usb=%s rate=%lu apb_baud=%lu apb_bytes=%lu apb_bursts=%lu\n",
        (unsigned long)AudioBuffer::available(),
        (unsigned long)(ProjectConfig::PCM_RING_FRAMES - 1),
        (unsigned long)AudioBuffer::droppedFrames(),
        (unsigned long)AudioBuffer::starvedFrames(),
        UsbAudioHost::isStreaming()
            ? "streaming"
            : "idle",
        (unsigned long)UsbAudioHost::sampleRate(),
        (unsigned long)APB8202Monitor::baudRate(),
        (unsigned long)APB8202Monitor::bytesSeen(),
        (unsigned long)APB8202Monitor::burstsSeen());
  }

  yield();
}
