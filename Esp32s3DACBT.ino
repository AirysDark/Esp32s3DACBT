/*
  Esp32s3DACBT
  =============

  APB8202 V1.3 Bluetooth audio -> ESP32-S3 internal ADC
  -> ESP32-S3 USB Host -> NRG USB Audio 7.1 -> 3.5 mm

  Arduino requirements:
    - ESP32-S3
    - Arduino-ESP32 >= 3.2.0
    - Recommended first bring-up: Arduino-ESP32 3.3.10
    - EspUsbHost 2.x library (tested API based on current 2.9.x examples)

  Analog inputs:
    GPIO4 = APB8202 L-OUT through AC coupling / bias network
    GPIO5 = APB8202 R-OUT through AC coupling / bias network

  Native ESP32-S3 USB:
    GPIO19 = USB D-
    GPIO20 = USB D+

  IMPORTANT:
    The NRG must receive 5 V USB VBUS from the host connection.
    Not every ESP32-S3 development board supplies VBUS from its USB-C port.
*/

#include <Arduino.h>
#include <EspUsbHost.h>

#include "esp_adc/adc_continuous.h"
#include "esp_err.h"
#include "hal/adc_types.h"
#include "soc/soc_caps.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Esp32s3DACBT is written for ESP32-S3."
#endif

// -----------------------------------------------------------------------------
// USER SETTINGS
// -----------------------------------------------------------------------------

static constexpr uint8_t AUDIO_LEFT_PIN  = 4;
static constexpr uint8_t AUDIO_RIGHT_PIN = 5;

// The ADC hardware cycles through L then R.
// 96 k conversions/s total = ~48 k samples/s per channel.
static constexpr uint32_t ADC_CHANNEL_SAMPLE_RATE = 48000;
static constexpr uint32_t ADC_TOTAL_SAMPLE_RATE   = ADC_CHANNEL_SAMPLE_RATE * 2;

// Raw 12-bit ADC sample -> signed 16-bit PCM.
// 16 maps a 12-bit centered signal approximately into a 16-bit range.
// Reduce this if the sound clips.
static constexpr int32_t PCM_GAIN = 16;

// USB output formats currently accepted.
static constexpr uint32_t USB_RATE_PREFERRED = 48000;
static constexpr uint32_t USB_RATE_FALLBACK  = 44100;

// Wait for this many captured stereo frames before starting playback.
// 1024 frames at 48 kHz is about 21 ms.
static constexpr uint32_t AUDIO_PREFILL_FRAMES = 1024;

// -----------------------------------------------------------------------------
// PCM RING BUFFER
// -----------------------------------------------------------------------------

struct StereoFrame
{
  int16_t left;
  int16_t right;
};

// Must be a power of two.
// 8192 stereo frames = about 170 ms at 48 kHz.
static constexpr uint32_t PCM_RING_FRAMES = 8192;
static constexpr uint32_t PCM_RING_MASK   = PCM_RING_FRAMES - 1;

static_assert((PCM_RING_FRAMES & (PCM_RING_FRAMES - 1)) == 0,
              "PCM_RING_FRAMES must be a power of two");

static StereoFrame pcmRing[PCM_RING_FRAMES];

static volatile uint32_t pcmHead = 0;
static volatile uint32_t pcmTail = 0;

static volatile uint32_t adcDroppedFrames = 0;
static volatile uint32_t usbStarveEvents  = 0;

static inline uint32_t atomicLoadU32(volatile uint32_t *value)
{
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline void atomicStoreU32(volatile uint32_t *value, uint32_t newValue)
{
  __atomic_store_n(value, newValue, __ATOMIC_RELEASE);
}

static inline void atomicIncU32(volatile uint32_t *value)
{
  __atomic_fetch_add(value, 1, __ATOMIC_RELAXED);
}

static uint32_t pcmAvailable()
{
  const uint32_t head = atomicLoadU32(&pcmHead);
  const uint32_t tail = atomicLoadU32(&pcmTail);
  return (head - tail) & PCM_RING_MASK;
}

static void pcmClear()
{
  const uint32_t head = atomicLoadU32(&pcmHead);
  atomicStoreU32(&pcmTail, head);
}

static bool pcmPush(const StereoFrame &frame)
{
  const uint32_t head = atomicLoadU32(&pcmHead);
  const uint32_t next = (head + 1) & PCM_RING_MASK;
  const uint32_t tail = atomicLoadU32(&pcmTail);

  if (next == tail)
  {
    atomicIncU32(&adcDroppedFrames);
    return false;
  }

  pcmRing[head] = frame;
  atomicStoreU32(&pcmHead, next);
  return true;
}

static bool pcmPop(StereoFrame &frame)
{
  const uint32_t tail = atomicLoadU32(&pcmTail);
  const uint32_t head = atomicLoadU32(&pcmHead);

  if (tail == head)
  {
    return false;
  }

  frame = pcmRing[tail];
  atomicStoreU32(&pcmTail, (tail + 1) & PCM_RING_MASK);
  return true;
}

// -----------------------------------------------------------------------------
// ADC CONTINUOUS / DMA CAPTURE
// -----------------------------------------------------------------------------

static adc_continuous_handle_t adcHandle = nullptr;
static adc_channel_t adcLeftChannel;
static adc_channel_t adcRightChannel;

static TaskHandle_t adcTaskHandle = nullptr;

// Read roughly 2 ms of aggregate ADC data per DMA frame:
// 192 conversions / 96,000 conversions/s = 2 ms.
// Each two conversions should produce one stereo frame.
static constexpr uint32_t ADC_CONVERSIONS_PER_READ = 192;
static constexpr uint32_t ADC_READ_BYTES =
    ADC_CONVERSIONS_PER_READ * SOC_ADC_DIGI_RESULT_BYTES;

static int16_t clamp16(int32_t value)
{
  if (value > 32767)
    return 32767;
  if (value < -32768)
    return -32768;
  return static_cast<int16_t>(value);
}

// Slow moving DC estimate removes the 1.65 V bias from each channel.
// dcQ8 is the raw ADC DC estimate in Q8 fixed-point format.
static int16_t adcRawToPcm(uint16_t raw, int32_t &dcQ8)
{
  const int32_t rawQ8 = static_cast<int32_t>(raw) << 8;

  // Slow DC tracker. 2^11 samples is roughly 43 ms at 48 kHz.
  dcQ8 += (rawQ8 - dcQ8) >> 11;

  const int32_t centered = (rawQ8 - dcQ8) >> 8;
  const int32_t pcm = centered * PCM_GAIN;

  return clamp16(pcm);
}

static void adcCaptureTask(void *parameter)
{
  (void)parameter;

  alignas(4) uint8_t rawBuffer[ADC_READ_BYTES];

  // Start near the expected 12-bit midpoint.
  int32_t dcLeftQ8  = 2048 << 8;
  int32_t dcRightQ8 = 2048 << 8;

  uint16_t pendingLeft = 2048;
  bool haveLeft = false;

  while (true)
  {
    uint32_t bytesRead = 0;

    const esp_err_t err = adc_continuous_read(
        adcHandle,
        rawBuffer,
        sizeof(rawBuffer),
        &bytesRead,
        1000);

    if (err == ESP_ERR_TIMEOUT)
    {
      continue;
    }

    if (err != ESP_OK)
    {
      Serial.printf("[ADC] read error: %s\n", esp_err_to_name(err));
      delay(10);
      continue;
    }

    for (uint32_t offset = 0;
         offset + SOC_ADC_DIGI_RESULT_BYTES <= bytesRead;
         offset += SOC_ADC_DIGI_RESULT_BYTES)
    {
      const adc_digi_output_data_t *sample =
          reinterpret_cast<const adc_digi_output_data_t *>(&rawBuffer[offset]);

      // ESP32-S3 continuous ADC uses TYPE2 format.
      const uint8_t channel = sample->type2.channel;
      const uint16_t value  = sample->type2.data;

      if (channel == static_cast<uint8_t>(adcLeftChannel))
      {
        pendingLeft = value;
        haveLeft = true;
      }
      else if (channel == static_cast<uint8_t>(adcRightChannel))
      {
        // The configured pattern is LEFT then RIGHT.
        // If a buffer begins with RIGHT, ignore that first unmatched sample.
        if (!haveLeft)
        {
          continue;
        }

        StereoFrame frame;
        frame.left  = adcRawToPcm(pendingLeft, dcLeftQ8);
        frame.right = adcRawToPcm(value, dcRightQ8);

        pcmPush(frame);
        haveLeft = false;
      }
    }
  }
}

static bool startAdcCapture()
{
  adc_unit_t leftUnit;
  adc_unit_t rightUnit;

  esp_err_t err = adc_continuous_io_to_channel(
      AUDIO_LEFT_PIN, &leftUnit, &adcLeftChannel);

  if (err != ESP_OK)
  {
    Serial.printf("[ADC] GPIO%u is not a continuous ADC pin: %s\n",
                  AUDIO_LEFT_PIN, esp_err_to_name(err));
    return false;
  }

  err = adc_continuous_io_to_channel(
      AUDIO_RIGHT_PIN, &rightUnit, &adcRightChannel);

  if (err != ESP_OK)
  {
    Serial.printf("[ADC] GPIO%u is not a continuous ADC pin: %s\n",
                  AUDIO_RIGHT_PIN, esp_err_to_name(err));
    return false;
  }

  if (leftUnit != ADC_UNIT_1 || rightUnit != ADC_UNIT_1)
  {
    Serial.println("[ADC] Both audio pins must be on ADC1 for ESP32-S3 continuous mode.");
    return false;
  }

  adc_continuous_handle_cfg_t handleConfig = {};
  handleConfig.max_store_buf_size = 8192;
  handleConfig.conv_frame_size = ADC_READ_BYTES;

  err = adc_continuous_new_handle(&handleConfig, &adcHandle);
  if (err != ESP_OK)
  {
    Serial.printf("[ADC] adc_continuous_new_handle failed: %s\n",
                  esp_err_to_name(err));
    return false;
  }

  adc_digi_pattern_config_t pattern[2] = {};

  pattern[0].atten = ADC_ATTEN_DB_12;
  pattern[0].channel = static_cast<uint8_t>(adcLeftChannel);
  pattern[0].unit = ADC_UNIT_1;
  pattern[0].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

  pattern[1].atten = ADC_ATTEN_DB_12;
  pattern[1].channel = static_cast<uint8_t>(adcRightChannel);
  pattern[1].unit = ADC_UNIT_1;
  pattern[1].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

  adc_continuous_config_t adcConfig = {};
  adcConfig.pattern_num = 2;
  adcConfig.adc_pattern = pattern;
  adcConfig.sample_freq_hz = ADC_TOTAL_SAMPLE_RATE;
  adcConfig.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  adcConfig.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

  err = adc_continuous_config(adcHandle, &adcConfig);
  if (err != ESP_OK)
  {
    Serial.printf("[ADC] adc_continuous_config failed: %s\n",
                  esp_err_to_name(err));
    return false;
  }

  err = adc_continuous_start(adcHandle);
  if (err != ESP_OK)
  {
    Serial.printf("[ADC] adc_continuous_start failed: %s\n",
                  esp_err_to_name(err));
    return false;
  }

  const BaseType_t taskResult = xTaskCreate(
      adcCaptureTask,
      "adc_audio",
      4096,
      nullptr,
      3,
      &adcTaskHandle);

  if (taskResult != pdPASS)
  {
    Serial.println("[ADC] Failed to create ADC capture task.");
    return false;
  }

  Serial.printf("[ADC] LEFT GPIO%u = ADC1 channel %u\n",
                AUDIO_LEFT_PIN,
                static_cast<unsigned>(adcLeftChannel));

  Serial.printf("[ADC] RIGHT GPIO%u = ADC1 channel %u\n",
                AUDIO_RIGHT_PIN,
                static_cast<unsigned>(adcRightChannel));

  Serial.printf("[ADC] aggregate=%lu Hz, per-channel~%lu Hz\n",
                static_cast<unsigned long>(ADC_TOTAL_SAMPLE_RATE),
                static_cast<unsigned long>(ADC_CHANNEL_SAMPLE_RATE));

  return true;
}

// -----------------------------------------------------------------------------
// USB AUDIO HOST
// -----------------------------------------------------------------------------

EspUsbHost usb;

static volatile uint8_t audioAddress = 0;
static volatile uint32_t activeUsbRate = 0;

// Incremented whenever a stream starts/stops so the audio callback resets
// its local resampler state.
static volatile uint32_t streamGeneration = 0;

static bool accept48kStereo16(uint32_t sampleRate,
                              uint8_t channels,
                              uint8_t bitsPerSample)
{
  return sampleRate == USB_RATE_PREFERRED &&
         channels == 2 &&
         bitsPerSample == 16;
}

static bool accept441kStereo16(uint32_t sampleRate,
                               uint8_t channels,
                               uint8_t bitsPerSample)
{
  return sampleRate == USB_RATE_FALLBACK &&
         channels == 2 &&
         bitsPerSample == 16;
}

static void writeStereoFrame(EspUsbHostAudioOutputRequest &request,
                             size_t frameIndex,
                             const StereoFrame &frame)
{
  // This sketch only selects 16-bit stereo streams.
  const size_t base =
      (frameIndex * request.channels) * request.bytesPerSample;

  request.data[base + 0] = static_cast<uint8_t>(frame.left & 0xff);
  request.data[base + 1] = static_cast<uint8_t>((frame.left >> 8) & 0xff);

  request.data[base + 2] = static_cast<uint8_t>(frame.right & 0xff);
  request.data[base + 3] = static_cast<uint8_t>((frame.right >> 8) & 0xff);
}

static void fillSilence(EspUsbHostAudioOutputRequest &request,
                        size_t firstFrame = 0)
{
  for (size_t frame = firstFrame; frame < request.frameCount; ++frame)
  {
    StereoFrame zero = {0, 0};
    writeStereoFrame(request, frame, zero);
  }

  request.writtenFrames = request.frameCount;
}

static void fillUsbAudio(EspUsbHostAudioOutputRequest &request)
{
  // Local state belongs to the USB callback/consumer side only.
  static uint32_t seenGeneration = 0;
  static bool primed = false;
  static StereoFrame current = {0, 0};
  static uint32_t resamplePhase = 0;

  const uint32_t generation = atomicLoadU32(&streamGeneration);

  if (generation != seenGeneration)
  {
    seenGeneration = generation;
    primed = false;
    current.left = 0;
    current.right = 0;
    resamplePhase = 0;
  }

  if (request.channels != 2 ||
      request.bytesPerSample != 2 ||
      (request.sampleRate != USB_RATE_PREFERRED &&
       request.sampleRate != USB_RATE_FALLBACK))
  {
    fillSilence(request);
    return;
  }

  if (!primed)
  {
    if (pcmAvailable() < AUDIO_PREFILL_FRAMES)
    {
      fillSilence(request);
      return;
    }

    if (!pcmPop(current))
    {
      fillSilence(request);
      return;
    }

    primed = true;
    resamplePhase = 0;
  }

  for (size_t frame = 0; frame < request.frameCount; ++frame)
  {
    writeStereoFrame(request, frame, current);

    // Input is nominally 48 kHz.
    // At 48 kHz USB output this consumes one input frame per output frame.
    // At 44.1 kHz USB output this occasionally consumes two input frames,
    // implementing a simple nearest-neighbour downsample.
    resamplePhase += ADC_CHANNEL_SAMPLE_RATE;

    while (resamplePhase >= request.sampleRate)
    {
      StereoFrame next;

      if (!pcmPop(next))
      {
        atomicIncU32(&usbStarveEvents);
        primed = false;

        // Finish the rest of this USB packet as silence, then wait for
        // the input ring to refill before resuming.
        fillSilence(request, frame + 1);
        return;
      }

      current = next;
      resamplePhase -= request.sampleRate;
    }
  }

  request.writtenFrames = request.frameCount;
}

// -----------------------------------------------------------------------------
// SETUP / LOOP
// -----------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" Esp32s3DACBT");
  Serial.println(" APB8202 -> ESP32-S3 -> USB -> NRG");
  Serial.println("========================================");

  if (!startAdcCapture())
  {
    Serial.println("[FATAL] ADC capture failed.");
    while (true)
    {
      delay(1000);
    }
  }

  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &info)
  {
    Serial.print("[USB] connected: ");
    espUsbHostPrint(info);

    if (!usb.audioOutputReady(info.address))
    {
      Serial.println("[USB] Device has no ready USB Audio OUT interface.");
      return;
    }

    EspUsbHostAudioStreamInfo streams[ESP_USB_HOST_MAX_AUDIO_STREAMS];
    const size_t count =
        usb.getAudioStreams(info.address,
                            streams,
                            ESP_USB_HOST_MAX_AUDIO_STREAMS);

    Serial.printf("[USB] audio streams found: %u\n",
                  static_cast<unsigned>(count));

    for (size_t i = 0; i < count; ++i)
    {
      espUsbHostPrint(streams[i]);
    }

    // Prefer 48 kHz because our ADC source is captured at 48 kHz/channel.
    EspUsbHostAudioStreamSelection selected =
        espUsbHostSelectAudioOutputStream(
            streams, count, accept48kStereo16);

    // Fall back to 44.1 kHz if 48 kHz is unavailable.
    if (!selected)
    {
      selected =
          espUsbHostSelectAudioOutputStream(
              streams, count, accept441kStereo16);
    }

    if (!selected)
    {
      Serial.println(
          "[USB] No supported 16-bit stereo 48 kHz / 44.1 kHz stream.");
      return;
    }

    pcmClear();
    atomicStoreU32(&activeUsbRate, selected.sampleRate);
    atomicIncU32(&streamGeneration);

    if (usb.audioOutputStart(
            streams[selected.index],
            selected.sampleRate,
            info.address))
    {
      audioAddress = info.address;

      Serial.printf("[USB] audio started: %lu Hz, stereo, 16-bit, addr=%u\n",
                    static_cast<unsigned long>(selected.sampleRate),
                    info.address);
    }
    else
    {
      Serial.printf("[USB] audioOutputStart failed for addr=%u\n",
                    info.address);
    }
  });

  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &info)
  {
    Serial.print("[USB] disconnected: ");
    espUsbHostPrint(info);

    if (info.address == audioAddress)
    {
      audioAddress = 0;
      atomicStoreU32(&activeUsbRate, 0);
      atomicIncU32(&streamGeneration);
      pcmClear();
    }
  });

  usb.onAudioOutputRequest([](EspUsbHostAudioOutputRequest &request)
  {
    // Keep this callback short. EspUsbHost calls it from its USB task.
    fillUsbAudio(request);
  });

  if (!usb.begin())
  {
    Serial.printf("[FATAL] usb.begin failed: %s\n", usb.lastErrorName());

    while (true)
    {
      delay(1000);
    }
  }

  Serial.println("[USB] Host started. Connect the NRG to the native USB OTG port.");
}

void loop()
{
  static uint32_t lastReportMs = 0;

  if (millis() - lastReportMs >= 1000)
  {
    lastReportMs = millis();

    const uint8_t address = audioAddress;

    Serial.printf(
        "[STAT] ring=%lu/%lu adc_drop=%lu usb_starve=%lu",
        static_cast<unsigned long>(pcmAvailable()),
        static_cast<unsigned long>(PCM_RING_FRAMES - 1),
        static_cast<unsigned long>(atomicLoadU32(&adcDroppedFrames)),
        static_cast<unsigned long>(atomicLoadU32(&usbStarveEvents)));

    if (address != 0)
    {
      Serial.printf(
          " usb_rate=%lu feedback=%lu host_underruns=%lu",
          static_cast<unsigned long>(usb.audioOutputRate(address)),
          static_cast<unsigned long>(usb.audioOutputFeedbackRate(address)),
          static_cast<unsigned long>(usb.audioOutputUnderruns(address)));
    }

    Serial.println();
  }

  delay(10);
}
