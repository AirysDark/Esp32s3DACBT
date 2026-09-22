#include "AdcAudio.h"

#include <Arduino.h>

#include "AudioBuffer.h"
#include "ProjectConfig.h"

#include "driver/adc.h"
#include "esp_err.h"
#include "hal/adc_types.h"
#include "soc/soc_caps.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Esp32s3DACBT requires ESP32-S3"
#endif

namespace {

static const adc1_channel_t ADC_LEFT_CH  = ADC1_CHANNEL_3; // GPIO4
static const adc1_channel_t ADC_RIGHT_CH = ADC1_CHANNEL_4; // GPIO5

int16_t clamp16(int32_t value)
{
  if (value > 32767) {
    return 32767;
  }

  if (value < -32768) {
    return -32768;
  }

  return (int16_t)value;
}

int16_t rawToPcm(uint16_t raw, int32_t &dcQ8)
{
  const int32_t rawQ8 = ((int32_t)raw) << 8;

  // Slow moving DC estimate removes the 1.65 V analog bias.
  dcQ8 += (rawQ8 - dcQ8) >> 11;

  const int32_t centered = (rawQ8 - dcQ8) >> 8;

  return clamp16(centered * ProjectConfig::PCM_GAIN);
}

void captureTask(void *arg)
{
  (void)arg;

  uint8_t buffer[512];

  int32_t dcLeftQ8  = 2048 << 8;
  int32_t dcRightQ8 = 2048 << 8;

  uint16_t pendingLeft = 2048;
  bool haveLeft = false;

  while (true) {
    uint32_t bytesRead = 0;

    const esp_err_t err =
        adc_digi_read_bytes(
            buffer,
            sizeof(buffer),
            &bytesRead,
            1000);

    if (err == ESP_ERR_TIMEOUT) {
      continue;
    }

    if (err != ESP_OK) {
      Serial.printf(
          "[ADC] read error: %s\n",
          esp_err_to_name(err));
      delay(5);
      continue;
    }

    for (uint32_t offset = 0;
         offset + sizeof(adc_digi_output_data_t) <= bytesRead;
         offset += sizeof(adc_digi_output_data_t)) {

      const adc_digi_output_data_t *sample =
          reinterpret_cast<const adc_digi_output_data_t *>(
              &buffer[offset]);

      const uint8_t channel = sample->type2.channel;
      const uint16_t raw = sample->type2.data;

      if (channel == (uint8_t)ADC_LEFT_CH) {
        pendingLeft = raw;
        haveLeft = true;
      }
      else if (channel == (uint8_t)ADC_RIGHT_CH) {
        if (!haveLeft) {
          continue;
        }

        StereoFrame frame;
        frame.left  = rawToPcm(pendingLeft, dcLeftQ8);
        frame.right = rawToPcm(raw, dcRightQ8);

        AudioBuffer::push(frame);
        haveLeft = false;
      }
    }
  }
}

} // namespace

namespace AdcAudio {

bool begin()
{
  adc_digi_init_config_t initConfig = {};
  initConfig.max_store_buf_size = 4096;
  initConfig.conv_num_each_intr = 256;
  initConfig.adc1_chan_mask =
      (1UL << (uint8_t)ADC_LEFT_CH) |
      (1UL << (uint8_t)ADC_RIGHT_CH);
  initConfig.adc2_chan_mask = 0;

  esp_err_t err = adc_digi_initialize(&initConfig);

  if (err != ESP_OK) {
    Serial.printf(
        "[ADC] adc_digi_initialize failed: %s\n",
        esp_err_to_name(err));
    return false;
  }

  static adc_digi_pattern_config_t pattern[2];
  memset(pattern, 0, sizeof(pattern));

  pattern[0].atten = ADC_ATTEN_DB_12;
  pattern[0].channel = (uint8_t)ADC_LEFT_CH;
  pattern[0].unit = ADC_UNIT_1;
  pattern[0].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

  pattern[1].atten = ADC_ATTEN_DB_12;
  pattern[1].channel = (uint8_t)ADC_RIGHT_CH;
  pattern[1].unit = ADC_UNIT_1;
  pattern[1].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

  adc_digi_configuration_t config = {};
  config.conv_limit_en = false;
  config.conv_limit_num = 0;
  config.pattern_num = 2;
  config.adc_pattern = pattern;
  config.sample_freq_hz = ProjectConfig::ADC_TOTAL_RATE;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

  err = adc_digi_controller_configure(&config);

  if (err != ESP_OK) {
    Serial.printf(
        "[ADC] adc_digi_controller_configure failed: %s\n",
        esp_err_to_name(err));
    return false;
  }

  err = adc_digi_start();

  if (err != ESP_OK) {
    Serial.printf(
        "[ADC] adc_digi_start failed: %s\n",
        esp_err_to_name(err));
    return false;
  }

  const BaseType_t taskResult =
      xTaskCreatePinnedToCore(
          captureTask,
          "adc_audio",
          4096,
          NULL,
          4,
          NULL,
          1);

  if (taskResult != pdPASS) {
    Serial.println("[ADC] failed to create capture task");
    return false;
  }

  Serial.printf(
      "[ADC] running: GPIO%u/GPIO%u, %lu Hz/channel (%lu total)\n",
      ProjectConfig::AUDIO_LEFT_GPIO,
      ProjectConfig::AUDIO_RIGHT_GPIO,
      (unsigned long)ProjectConfig::ADC_PER_CHANNEL_RATE,
      (unsigned long)ProjectConfig::ADC_TOTAL_RATE);

  return true;
}

} // namespace AdcAudio
