/*
  Esp32s3DACBT - APB8202 / CW6638M UART discovery build
  Core target: Arduino-ESP32 2.0.17

  Current hardware mapping established during bench testing:
    APB pin 3 VCC -> 3.3V
    APB pin 4 GND -> ESP32-S3 GND
    APB pin 5 TXD -> ESP32-S3 GPIO18 RX
    APB pin 6 RXD -> ESP32-S3 GPIO17 TX

  This discovery build deliberately does NOT start ADC audio or USB host.
  It exists only to identify the Bluetooth module UART protocol safely.
*/

#include <Arduino.h>

#include "APB8202Monitor.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Esp32s3DACBT requires ESP32-S3"
#endif

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("============================================");
  Serial.println(" APB8202 / CW6638M UART DISCOVERY MODE");
  Serial.println("============================================");
  Serial.println(" BT TX pin 5 -> GPIO18 RX");
  Serial.println(" BT RX pin 6 -> GPIO17 TX");
  Serial.println(" ADC audio: DISABLED");
  Serial.println(" USB host:  DISABLED");
  Serial.println();

  if (!APB8202Monitor::begin()) {
    Serial.println("[FATAL] APB8202 UART monitor setup failed");
    while (true) {
      yield();
    }
  }
}

void loop()
{
  APB8202Monitor::update();
  yield();
}
