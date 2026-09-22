/*
  Esp32s3DACBT
  Core target: Arduino-ESP32 2.0.17

  APB8202 V1.3 analog stereo -> ESP32-S3 ADC DMA
  -> ESP-IDF USB Host (UAC1) -> NRG USB Audio 7.1

  Audio input:
    GPIO4 = left  (ADC1_CH3)
    GPIO5 = right (ADC1_CH4)

  Native USB:
    GPIO19 = D-
    GPIO20 = D+

  IMPORTANT:
    - This build intentionally does NOT use EspUsbHost. Current EspUsbHost 2.x
      requires Arduino-ESP32 >= 3.2.0.
    - Arduino-ESP32 2.0.17 ships ESP-IDF 4.4.x USB Host APIs, which are used
      directly here.
    - The ESP32-S3 board must provide 5 V VBUS to the NRG in host mode.
*/

#include <Arduino.h>

#include "driver/adc.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/adc_types.h"
#include "soc/soc_caps.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Esp32s3DACBT requires ESP32-S3"
#endif

// -----------------------------------------------------------------------------
// AUDIO INPUT
// -----------------------------------------------------------------------------

static const adc1_channel_t ADC_LEFT_CH  = ADC1_CHANNEL_3; // GPIO4
static const adc1_channel_t ADC_RIGHT_CH = ADC1_CHANNEL_4; // GPIO5

// Arduino-ESP32 2.0.17 / IDF 4.4 ADC continuous driver is documented up to
// about 83.3 k conversions/s total. Two channels at 40 kHz each = 80 k total.
static const uint32_t ADC_PER_CHANNEL_RATE = 40000;
static const uint32_t ADC_TOTAL_RATE       = 80000;
static const int32_t PCM_GAIN              = 16;

struct StereoFrame {
  int16_t left;
  int16_t right;
};

static const uint32_t PCM_RING_FRAMES = 8192;
static const uint32_t PCM_RING_MASK   = PCM_RING_FRAMES - 1;

static StereoFrame pcmRing[PCM_RING_FRAMES];
static volatile uint32_t pcmHead = 0;
static volatile uint32_t pcmTail = 0;
static volatile uint32_t adcDropped = 0;
static volatile uint32_t usbStarved = 0;

static portMUX_TYPE pcmMux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t pcmAvailable()
{
  uint32_t h, t;
  portENTER_CRITICAL(&pcmMux);
  h = pcmHead;
  t = pcmTail;
  portEXIT_CRITICAL(&pcmMux);
  return (h - t) & PCM_RING_MASK;
}

static void pcmClear()
{
  portENTER_CRITICAL(&pcmMux);
  pcmTail = pcmHead;
  portEXIT_CRITICAL(&pcmMux);
}

static bool pcmPush(const StereoFrame &f)
{
  bool ok = false;
  portENTER_CRITICAL(&pcmMux);
  const uint32_t next = (pcmHead + 1) & PCM_RING_MASK;
  if (next != pcmTail) {
    pcmRing[pcmHead] = f;
    pcmHead = next;
    ok = true;
  } else {
    adcDropped++;
  }
  portEXIT_CRITICAL(&pcmMux);
  return ok;
}

static bool pcmPop(StereoFrame &f)
{
  bool ok = false;
  portENTER_CRITICAL(&pcmMux);
  if (pcmTail != pcmHead) {
    f = pcmRing[pcmTail];
    pcmTail = (pcmTail + 1) & PCM_RING_MASK;
    ok = true;
  }
  portEXIT_CRITICAL(&pcmMux);
  return ok;
}

static int16_t clamp16(int32_t v)
{
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static int16_t rawToPcm(uint16_t raw, int32_t &dcQ8)
{
  const int32_t rawQ8 = ((int32_t)raw) << 8;
  dcQ8 += (rawQ8 - dcQ8) >> 11; // slow DC-bias tracker
  const int32_t centered = (rawQ8 - dcQ8) >> 8;
  return clamp16(centered * PCM_GAIN);
}

static void adcTask(void *arg)
{
  (void)arg;

  uint8_t buf[512];
  int32_t dcLeftQ8  = 2048 << 8;
  int32_t dcRightQ8 = 2048 << 8;

  uint16_t pendingLeft = 2048;
  bool haveLeft = false;

  while (true) {
    uint32_t bytesRead = 0;
    esp_err_t err = adc_digi_read_bytes(buf, sizeof(buf), &bytesRead, 1000);

    if (err == ESP_ERR_TIMEOUT) {
      continue;
    }
    if (err != ESP_OK) {
      Serial.printf("[ADC] read error: %s\n", esp_err_to_name(err));
      delay(5);
      continue;
    }

    for (uint32_t off = 0;
         off + sizeof(adc_digi_output_data_t) <= bytesRead;
         off += sizeof(adc_digi_output_data_t)) {

      const adc_digi_output_data_t *s =
          reinterpret_cast<const adc_digi_output_data_t *>(&buf[off]);

      const uint8_t ch = s->type2.channel;
      const uint16_t raw = s->type2.data;

      if (ch == (uint8_t)ADC_LEFT_CH) {
        pendingLeft = raw;
        haveLeft = true;
      } else if (ch == (uint8_t)ADC_RIGHT_CH) {
        if (!haveLeft) {
          continue;
        }

        StereoFrame f;
        f.left  = rawToPcm(pendingLeft, dcLeftQ8);
        f.right = rawToPcm(raw, dcRightQ8);
        pcmPush(f);
        haveLeft = false;
      }
    }
  }
}

static bool startAdc()
{
  adc_digi_init_config_t initCfg = {};
  initCfg.max_store_buf_size = 4096;
  initCfg.conv_num_each_intr = 256;
  initCfg.adc1_chan_mask =
      (1UL << (uint8_t)ADC_LEFT_CH) |
      (1UL << (uint8_t)ADC_RIGHT_CH);
  initCfg.adc2_chan_mask = 0;

  esp_err_t err = adc_digi_initialize(&initCfg);
  if (err != ESP_OK) {
    Serial.printf("[ADC] adc_digi_initialize failed: %s\n", esp_err_to_name(err));
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

  adc_digi_configuration_t cfg = {};
  cfg.conv_limit_en = false;
  cfg.conv_limit_num = 0;
  cfg.pattern_num = 2;
  cfg.adc_pattern = pattern;
  cfg.sample_freq_hz = ADC_TOTAL_RATE;
  cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

  err = adc_digi_controller_configure(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[ADC] adc_digi_controller_configure failed: %s\n",
                  esp_err_to_name(err));
    return false;
  }

  err = adc_digi_start();
  if (err != ESP_OK) {
    Serial.printf("[ADC] adc_digi_start failed: %s\n", esp_err_to_name(err));
    return false;
  }

  BaseType_t ok = xTaskCreatePinnedToCore(
      adcTask, "adc_audio", 4096, NULL, 4, NULL, 1);

  if (ok != pdPASS) {
    Serial.println("[ADC] failed to create capture task");
    return false;
  }

  Serial.printf("[ADC] running: GPIO4/GPIO5, %lu Hz/channel (%lu total)\n",
                (unsigned long)ADC_PER_CHANNEL_RATE,
                (unsigned long)ADC_TOTAL_RATE);
  return true;
}

// -----------------------------------------------------------------------------
// MINIMAL USB AUDIO CLASS 1 HOST FOR ARDUINO-ESP32 2.0.17
// -----------------------------------------------------------------------------

static const uint8_t USB_CLASS_AUDIO_CODE = 0x01;
static const uint8_t USB_SUBCLASS_AUDIOSTREAMING = 0x02;
static const uint8_t USB_DESC_CS_INTERFACE = 0x24;
static const uint8_t UAC_AS_FORMAT_TYPE = 0x02;
static const uint8_t UAC_FORMAT_TYPE_I = 0x01;
static const uint8_t UAC_SET_CUR = 0x01;
static const uint8_t UAC_EP_SAMPLING_FREQ_CONTROL = 0x01;

struct AudioStreamInfo {
  bool valid;
  uint8_t interfaceNumber;
  uint8_t alternateSetting;
  uint8_t endpointAddress;
  uint16_t maxPacketSize;
  uint8_t interval;
  uint32_t sampleRate;
};

static usb_host_client_handle_t usbClient = NULL;
static usb_device_handle_t audioDev = NULL;
static AudioStreamInfo audioStream = {};
static volatile bool usbStreaming = false;

static volatile uint8_t pendingDeviceAddress = 0;
static volatile bool pendingDisconnect = false;

static const int ISO_TRANSFER_COUNT = 8;
static usb_transfer_t *isoTransfers[ISO_TRANSFER_COUNT] = {0};

static uint32_t usbPacketRateAccumulator = 0;
static uint32_t resamplePhase = 0;
static StereoFrame resampleCurrent = {0, 0};
static bool resampleHaveCurrent = false;

static uint32_t read24(const uint8_t *p)
{
  return ((uint32_t)p[0]) |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16);
}

static uint32_t chooseRate(const uint8_t *fmt, uint8_t len)
{
  // UAC1 FORMAT_TYPE_I:
  // 0 len,1 type,2 subtype,3 formatType,4 channels,5 subframe,
  // 6 bitResolution,7 samFreqType, then 3-byte rates.
  if (len < 11) {
    return 0;
  }

  const uint8_t freqType = fmt[7];

  if (freqType == 0) {
    // Continuous range: lower + upper frequency.
    if (len < 14) return 0;
    const uint32_t lo = read24(fmt + 8);
    const uint32_t hi = read24(fmt + 11);
    if (48000 >= lo && 48000 <= hi) return 48000;
    if (44100 >= lo && 44100 <= hi) return 44100;
    return 0;
  }

  const uint8_t needed = 8 + (freqType * 3);
  if (len < needed) {
    return 0;
  }

  bool has441 = false;
  for (uint8_t i = 0; i < freqType; ++i) {
    const uint32_t r = read24(fmt + 8 + i * 3);
    if (r == 48000) return 48000;
    if (r == 44100) has441 = true;
  }

  return has441 ? 44100 : 0;
}

static bool parseUac1Output(const usb_config_desc_t *cfg, AudioStreamInfo &out)
{
  memset(&out, 0, sizeof(out));

  const uint8_t *base = reinterpret_cast<const uint8_t *>(cfg);
  const uint16_t total = cfg->wTotalLength;

  bool inCandidate = false;
  AudioStreamInfo candidate = {};
  bool formatGood = false;
  uint32_t selectedRate = 0;

  auto evaluateCandidate = [&]() -> bool {
    if (inCandidate &&
        candidate.endpointAddress != 0 &&
        candidate.maxPacketSize != 0 &&
        formatGood &&
        selectedRate != 0) {
      candidate.sampleRate = selectedRate;
      candidate.valid = true;
      out = candidate;
      return true;
    }
    return false;
  };

  uint16_t off = 0;
  while (off + 2 <= total) {
    const uint8_t *d = base + off;
    const uint8_t len = d[0];
    const uint8_t type = d[1];

    if (len < 2 || off + len > total) {
      break;
    }

    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= USB_INTF_DESC_SIZE) {
      if (evaluateCandidate()) {
        return true;
      }

      const usb_intf_desc_t *itf =
          reinterpret_cast<const usb_intf_desc_t *>(d);

      inCandidate =
          itf->bInterfaceClass == USB_CLASS_AUDIO_CODE &&
          itf->bInterfaceSubClass == USB_SUBCLASS_AUDIOSTREAMING &&
          itf->bAlternateSetting != 0 &&
          itf->bNumEndpoints > 0;

      memset(&candidate, 0, sizeof(candidate));
      formatGood = false;
      selectedRate = 0;

      if (inCandidate) {
        candidate.interfaceNumber = itf->bInterfaceNumber;
        candidate.alternateSetting = itf->bAlternateSetting;
      }
    }
    else if (inCandidate &&
             type == USB_DESC_CS_INTERFACE &&
             len >= 8 &&
             d[2] == UAC_AS_FORMAT_TYPE &&
             d[3] == UAC_FORMAT_TYPE_I) {

      const uint8_t channels = d[4];
      const uint8_t subframe = d[5];
      const uint8_t bits = d[6];

      if (channels == 2 && subframe == 2 && bits == 16) {
        selectedRate = chooseRate(d, len);
        formatGood = selectedRate != 0;
      }
    }
    else if (inCandidate &&
             type == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
             len >= USB_EP_DESC_SIZE) {

      const usb_ep_desc_t *ep =
          reinterpret_cast<const usb_ep_desc_t *>(d);

      const bool isOut =
          (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) == 0;
      const bool isIso =
          (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) ==
          USB_BM_ATTRIBUTES_XFER_ISOC;

      if (isOut && isIso) {
        candidate.endpointAddress = ep->bEndpointAddress;
        candidate.maxPacketSize = ep->wMaxPacketSize & 0x07FF;
        candidate.interval = ep->bInterval;
      }
    }

    off += len;
  }

  return evaluateCandidate();
}

struct ControlWait {
  volatile bool done;
  volatile usb_transfer_status_t status;
};

static void controlTransferCallback(usb_transfer_t *xfer)
{
  ControlWait *w = static_cast<ControlWait *>(xfer->context);
  w->status = xfer->status;
  w->done = true;
}

static bool setUac1SampleRate(uint8_t ep, uint32_t rate)
{
  usb_transfer_t *xfer = NULL;
  if (usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + 3, 0, &xfer) != ESP_OK) {
    return false;
  }

  ControlWait wait = {};
  wait.done = false;
  wait.status = USB_TRANSFER_STATUS_ERROR;

  usb_setup_packet_t *setup =
      reinterpret_cast<usb_setup_packet_t *>(xfer->data_buffer);

  setup->bmRequestType =
      USB_BM_REQUEST_TYPE_DIR_OUT |
      USB_BM_REQUEST_TYPE_TYPE_CLASS |
      USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;
  setup->bRequest = UAC_SET_CUR;
  setup->wValue = (uint16_t)UAC_EP_SAMPLING_FREQ_CONTROL << 8;
  setup->wIndex = ep;
  setup->wLength = 3;

  xfer->data_buffer[USB_SETUP_PACKET_SIZE + 0] = rate & 0xFF;
  xfer->data_buffer[USB_SETUP_PACKET_SIZE + 1] = (rate >> 8) & 0xFF;
  xfer->data_buffer[USB_SETUP_PACKET_SIZE + 2] = (rate >> 16) & 0xFF;

  xfer->device_handle = audioDev;
  xfer->bEndpointAddress = 0;
  xfer->callback = controlTransferCallback;
  xfer->context = &wait;
  xfer->num_bytes = USB_SETUP_PACKET_SIZE + 3;
  xfer->timeout_ms = 1000;

  esp_err_t err = usb_host_transfer_submit_control(usbClient, xfer);
  if (err != ESP_OK) {
    usb_host_transfer_free(xfer);
    return false;
  }

  const uint32_t start = millis();
  while (!wait.done && millis() - start < 1500) {
    usb_host_client_handle_events(usbClient, pdMS_TO_TICKS(10));
  }

  const bool ok =
      wait.done && wait.status == USB_TRANSFER_STATUS_COMPLETED;

  usb_host_transfer_free(xfer);
  return ok;
}

static void resetResampler()
{
  resamplePhase = 0;
  resampleCurrent.left = 0;
  resampleCurrent.right = 0;
  resampleHaveCurrent = false;
  usbPacketRateAccumulator = 0;
}

static StereoFrame nextUsbFrame(uint32_t outputRate)
{
  if (!resampleHaveCurrent) {
    if (!pcmPop(resampleCurrent)) {
      StereoFrame z = {0, 0};
      usbStarved++;
      return z;
    }
    resampleHaveCurrent = true;
  }

  StereoFrame out = resampleCurrent;

  resamplePhase += ADC_PER_CHANNEL_RATE;
  while (resamplePhase >= outputRate) {
    StereoFrame next;
    if (pcmPop(next)) {
      resampleCurrent = next;
    } else {
      resampleCurrent.left = 0;
      resampleCurrent.right = 0;
      usbStarved++;
    }
    resamplePhase -= outputRate;
  }

  return out;
}

static void prepareAndSubmitIso(usb_transfer_t *xfer);

static void isoTransferCallback(usb_transfer_t *xfer)
{
  if (!usbStreaming || audioDev == NULL) {
    return;
  }

  if (xfer->status != USB_TRANSFER_STATUS_COMPLETED &&
      xfer->status != USB_TRANSFER_STATUS_SKIPPED) {
    Serial.printf("[USB] isoch transfer status=%d\n", (int)xfer->status);
  }

  prepareAndSubmitIso(xfer);
}

static void prepareAndSubmitIso(usb_transfer_t *xfer)
{
  if (!usbStreaming || audioDev == NULL || !audioStream.valid) {
    return;
  }

  usbPacketRateAccumulator += audioStream.sampleRate;
  uint32_t frames = usbPacketRateAccumulator / 1000;
  usbPacketRateAccumulator %= 1000;

  if (frames == 0) frames = 1;

  const uint32_t bytes = frames * 4; // stereo * 16-bit
  if (bytes > xfer->data_buffer_size ||
      bytes > audioStream.maxPacketSize) {
    Serial.printf("[USB] packet too large: %lu > buffer=%u mps=%u\n",
                  (unsigned long)bytes,
                  (unsigned)xfer->data_buffer_size,
                  (unsigned)audioStream.maxPacketSize);
    usbStreaming = false;
    return;
  }

  for (uint32_t i = 0; i < frames; ++i) {
    StereoFrame f = nextUsbFrame(audioStream.sampleRate);
    const uint32_t o = i * 4;
    xfer->data_buffer[o + 0] = (uint8_t)(f.left & 0xFF);
    xfer->data_buffer[o + 1] = (uint8_t)((f.left >> 8) & 0xFF);
    xfer->data_buffer[o + 2] = (uint8_t)(f.right & 0xFF);
    xfer->data_buffer[o + 3] = (uint8_t)((f.right >> 8) & 0xFF);
  }

  xfer->device_handle = audioDev;
  xfer->bEndpointAddress = audioStream.endpointAddress;
  xfer->callback = isoTransferCallback;
  xfer->context = NULL;
  xfer->num_bytes = bytes;
  xfer->timeout_ms = 0;
  xfer->isoc_packet_desc[0].num_bytes = bytes;

  esp_err_t err = usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    Serial.printf("[USB] iso submit failed: %s\n", esp_err_to_name(err));
    usbStreaming = false;
  }
}

static void freeIsoTransfers()
{
  for (int i = 0; i < ISO_TRANSFER_COUNT; ++i) {
    if (isoTransfers[i]) {
      // If a device disappeared, transfers should return with NO_DEVICE.
      // Free only after streaming has been stopped.
      usb_host_transfer_free(isoTransfers[i]);
      isoTransfers[i] = NULL;
    }
  }
}

static void disconnectAudioDevice()
{
  usbStreaming = false;
  delay(20);

  freeIsoTransfers();

  if (audioDev != NULL) {
    if (audioStream.valid) {
      usb_host_interface_release(
          usbClient, audioDev, audioStream.interfaceNumber);
    }
    usb_host_device_close(usbClient, audioDev);
  }

  audioDev = NULL;
  memset(&audioStream, 0, sizeof(audioStream));
  resetResampler();
  pcmClear();

  Serial.println("[USB] audio device disconnected");
}

static bool connectAudioDevice(uint8_t addr)
{
  if (audioDev != NULL) {
    return false;
  }

  esp_err_t err = usb_host_device_open(usbClient, addr, &audioDev);
  if (err != ESP_OK) {
    Serial.printf("[USB] device open failed: %s\n", esp_err_to_name(err));
    audioDev = NULL;
    return false;
  }

  const usb_device_desc_t *devDesc = NULL;
  if (usb_host_get_device_descriptor(audioDev, &devDesc) == ESP_OK && devDesc) {
    Serial.printf("[USB] device VID=%04X PID=%04X\n",
                  devDesc->idVendor, devDesc->idProduct);
  }

  const usb_config_desc_t *cfg = NULL;
  err = usb_host_get_active_config_descriptor(audioDev, &cfg);
  if (err != ESP_OK || cfg == NULL) {
    Serial.println("[USB] no active configuration descriptor");
    usb_host_device_close(usbClient, audioDev);
    audioDev = NULL;
    return false;
  }

  AudioStreamInfo found = {};
  if (!parseUac1Output(cfg, found)) {
    Serial.println("[USB] no supported UAC1 stereo 16-bit 48k/44.1k output stream");
    usb_host_device_close(usbClient, audioDev);
    audioDev = NULL;
    return false;
  }

  Serial.printf("[USB] UAC1 OUT: iface=%u alt=%u ep=0x%02X mps=%u rate=%lu\n",
                found.interfaceNumber,
                found.alternateSetting,
                found.endpointAddress,
                found.maxPacketSize,
                (unsigned long)found.sampleRate);

  err = usb_host_interface_claim(
      usbClient,
      audioDev,
      found.interfaceNumber,
      found.alternateSetting);

  if (err != ESP_OK) {
    Serial.printf("[USB] interface claim failed: %s\n", esp_err_to_name(err));
    usb_host_device_close(usbClient, audioDev);
    audioDev = NULL;
    return false;
  }

  audioStream = found;

  // Many UAC1 devices accept endpoint SET_CUR for sample rate. If the NRG
  // has a fixed rate and stalls this request, continue anyway.
  if (setUac1SampleRate(found.endpointAddress, found.sampleRate)) {
    Serial.println("[USB] sample rate SET_CUR accepted");
  } else {
    Serial.println("[USB] sample rate SET_CUR not accepted; trying stream anyway");
  }

  resetResampler();
  pcmClear();

  for (int i = 0; i < ISO_TRANSFER_COUNT; ++i) {
    err = usb_host_transfer_alloc(found.maxPacketSize, 1, &isoTransfers[i]);
    if (err != ESP_OK) {
      Serial.printf("[USB] isoch alloc %d failed: %s\n",
                    i, esp_err_to_name(err));
      usbStreaming = false;
      freeIsoTransfers();
      usb_host_interface_release(
          usbClient, audioDev, found.interfaceNumber);
      usb_host_device_close(usbClient, audioDev);
      audioDev = NULL;
      memset(&audioStream, 0, sizeof(audioStream));
      return false;
    }
  }

  // Give ADC ring a little data before the first USB packet.
  const uint32_t waitStart = millis();
  while (pcmAvailable() < 400 && millis() - waitStart < 100) {
    delay(1);
  }

  usbStreaming = true;

  for (int i = 0; i < ISO_TRANSFER_COUNT; ++i) {
    prepareAndSubmitIso(isoTransfers[i]);
    if (!usbStreaming) {
      break;
    }
  }

  if (!usbStreaming) {
    disconnectAudioDevice();
    return false;
  }

  Serial.println("[USB] audio streaming started");
  return true;
}

static void usbClientEvent(
    const usb_host_client_event_msg_t *eventMsg,
    void *arg)
{
  (void)arg;

  if (eventMsg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (audioDev == NULL) {
      pendingDeviceAddress = eventMsg->new_dev.address;
    }
  }
  else if (eventMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (audioDev != NULL &&
        eventMsg->dev_gone.dev_hdl == audioDev) {
      pendingDisconnect = true;
      usbStreaming = false;
    }
  }
}

static void usbLibraryTask(void *arg)
{
  SemaphoreHandle_t ready = (SemaphoreHandle_t)arg;

  usb_host_config_t cfg = {};
  cfg.skip_phy_setup = false;
  cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;

  esp_err_t err = usb_host_install(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[USB] usb_host_install failed: %s\n", esp_err_to_name(err));
    xSemaphoreGive(ready);
    vTaskDelete(NULL);
    return;
  }

  Serial.println("[USB] host library installed");
  xSemaphoreGive(ready);

  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
  }
}

static void usbClientTask(void *arg)
{
  (void)arg;

  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = usbClientEvent;
  cfg.async.callback_arg = NULL;

  esp_err_t err = usb_host_client_register(&cfg, &usbClient);
  if (err != ESP_OK) {
    Serial.printf("[USB] client register failed: %s\n", esp_err_to_name(err));
    vTaskDelete(NULL);
    return;
  }

  Serial.println("[USB] client registered; waiting for NRG");

  while (true) {
    usb_host_client_handle_events(usbClient, pdMS_TO_TICKS(10));

    if (pendingDisconnect) {
      pendingDisconnect = false;
      disconnectAudioDevice();
    }

    const uint8_t addr = pendingDeviceAddress;
    if (addr != 0 && audioDev == NULL) {
      pendingDeviceAddress = 0;
      connectAudioDevice(addr);
    }
  }
}

static bool startUsbHost()
{
  SemaphoreHandle_t ready = xSemaphoreCreateBinary();
  if (!ready) {
    return false;
  }

  BaseType_t ok = xTaskCreatePinnedToCore(
      usbLibraryTask, "usb_lib", 4096, ready, 5, NULL, 0);

  if (ok != pdPASS) {
    vSemaphoreDelete(ready);
    return false;
  }

  if (xSemaphoreTake(ready, pdMS_TO_TICKS(3000)) != pdTRUE) {
    Serial.println("[USB] host install timeout");
    vSemaphoreDelete(ready);
    return false;
  }

  vSemaphoreDelete(ready);

  // If install failed, registering the client will fail and print the reason.
  ok = xTaskCreatePinnedToCore(
      usbClientTask, "usb_client", 6144, NULL, 6, NULL, 0);

  return ok == pdPASS;
}

// -----------------------------------------------------------------------------
// ARDUINO
// -----------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("============================================");
  Serial.println(" Esp32s3DACBT - Arduino-ESP32 core 2.0.17");
  Serial.println(" APB8202 -> ADC -> ESP32-S3 USB Host -> NRG");
  Serial.println("============================================");

  if (!startAdc()) {
    Serial.println("[FATAL] ADC setup failed");
    while (true) delay(1000);
  }

  if (!startUsbHost()) {
    Serial.println("[FATAL] USB host setup failed");
    while (true) delay(1000);
  }
}

void loop()
{
  static uint32_t last = 0;

  if (millis() - last >= 1000) {
    last = millis();

    uint32_t drops, starves;
    portENTER_CRITICAL(&pcmMux);
    drops = adcDropped;
    starves = usbStarved;
    portEXIT_CRITICAL(&pcmMux);

    Serial.printf(
        "[STAT] ring=%lu/%lu adc_drop=%lu usb_starve=%lu usb=%s rate=%lu\n",
        (unsigned long)pcmAvailable(),
        (unsigned long)(PCM_RING_FRAMES - 1),
        (unsigned long)drops,
        (unsigned long)starves,
        usbStreaming ? "streaming" : "idle",
        (unsigned long)audioStream.sampleRate);
  }

  delay(10);
}
