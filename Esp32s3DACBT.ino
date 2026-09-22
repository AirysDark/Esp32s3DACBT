/*
  Esp32s3DACBT
  Core target: Arduino-ESP32 2.0.17

  Main sketch intentionally kept small.

  Modules:
    ProjectConfig.h   - hardware and timing constants
    AudioBuffer.*     - stereo PCM ring buffer + counters
    AdcAudio.*        - APB8202 analog capture using ADC DMA
    UsbAudioHost.*    - USB Audio Class 1 host for the NRG

  Signal path:
    APB8202 L/R -> ESP32-S3 ADC -> PCM buffer
    -> ESP32-S3 USB Host -> NRG USB Audio 7.1
*/

#include <Arduino.h>

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
  delay(1000);

  Serial.println();
  Serial.println("============================================");
  Serial.println(" Esp32s3DACBT - Arduino-ESP32 core 2.0.17");
  Serial.println(" APB8202 -> ADC -> ESP32-S3 USB Host -> NRG");
  Serial.println("============================================");

  if (!AdcAudio::begin()) {
    Serial.println("[FATAL] ADC setup failed");

    while (true) {
      delay(1000);
    }
  }

  if (!UsbAudioHost::begin()) {
    Serial.println("[FATAL] USB host setup failed");

    while (true) {
      delay(1000);
    }
  }
}

void loop()
{
  static uint32_t lastReportMs = 0;

  if (millis() - lastReportMs >= 1000) {
    lastReportMs = millis();

    Serial.printf(
        "[STAT] ring=%lu/%lu adc_drop=%lu usb_starve=%lu usb=%s rate=%lu\n",
        (unsigned long)AudioBuffer::available(),
        (unsigned long)(ProjectConfig::PCM_RING_FRAMES - 1),
        (unsigned long)AudioBuffer::droppedFrames(),
        (unsigned long)AudioBuffer::starvedFrames(),
        UsbAudioHost::isStreaming()
            ? "streaming"
            : "idle",
        (unsigned long)UsbAudioHost::sampleRate());
  }

  delay(10);
}
