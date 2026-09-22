/*
  Esp32s3DACBT
  Core target: Arduino-ESP32 2.0.17

  Main sketch intentionally kept small.

  Modules:
    ProjectConfig.h      - hardware and timing constants
    AudioBuffer.*        - stereo PCM ring buffer + counters
    AdcAudio.*           - APB8202 analog capture using ADC DMA
    APB8202Control.*     - APB8202 UART state/control parser
    UsbAudioHost.*       - USB Audio Class 1 host for the NRG

  Signal paths:
    APB8202 L/R -> ESP32-S3 ADC -> PCM buffer -> USB Host -> NRG
    APB8202 TX/RX <-> ESP32-S3 Serial1 for control/status
*/

#include <Arduino.h>

#include "APB8202Control.h"
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
  Serial.println(" APB8202 -> ADC/UART -> ESP32-S3 -> USB NRG");
  Serial.println("============================================");

  if (!APB8202Control::begin()) {
    Serial.println("[FATAL] APB8202 UART setup failed");

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
  APB8202Control::update();

  static uint32_t lastReportMs = 0;

  if (millis() - lastReportMs >= 1000) {
    lastReportMs = millis();

    Serial.printf(
        "[STAT] ring=%lu/%lu adc_drop=%lu usb_starve=%lu usb=%s rate=%lu apb=%s apb_baud=%lu caller=%s\n",
        (unsigned long)AudioBuffer::available(),
        (unsigned long)(ProjectConfig::PCM_RING_FRAMES - 1),
        (unsigned long)AudioBuffer::droppedFrames(),
        (unsigned long)AudioBuffer::starvedFrames(),
        UsbAudioHost::isStreaming()
            ? "streaming"
            : "idle",
        (unsigned long)UsbAudioHost::sampleRate(),
        APB8202Control::stateName(),
        (unsigned long)APB8202Control::baudRate(),
        APB8202Control::callerNumber()[0] != '\0'
            ? APB8202Control::callerNumber()
            : "-");
  }

  yield();
}
