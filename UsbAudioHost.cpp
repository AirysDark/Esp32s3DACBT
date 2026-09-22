#include "UsbAudioHost.h"

#include <Arduino.h>

#include "AudioBuffer.h"
#include "ProjectConfig.h"

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Esp32s3DACBT requires ESP32-S3"
#endif

namespace {

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

struct ControlWait {
  volatile bool done;
  volatile usb_transfer_status_t status;
};

static usb_host_client_handle_t usbClient = NULL;
static usb_device_handle_t audioDevice = NULL;

static AudioStreamInfo audioStream = {};

static volatile bool streaming = false;
static volatile uint8_t pendingDeviceAddress = 0;
static volatile bool pendingDisconnect = false;

static usb_transfer_t *
    isoTransfers[ProjectConfig::ISO_TRANSFER_COUNT] = {0};

static uint32_t usbPacketRateAccumulator = 0;

static uint32_t resamplePhase = 0;
static StereoFrame resampleCurrent = {0, 0};
static bool resampleHaveCurrent = false;

uint32_t read24(const uint8_t *data)
{
  return ((uint32_t)data[0]) |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16);
}

uint32_t chooseRate(const uint8_t *formatDescriptor, uint8_t length)
{
  // UAC1 FORMAT_TYPE_I:
  // 0 len
  // 1 descriptor type
  // 2 descriptor subtype
  // 3 format type
  // 4 channels
  // 5 subframe size
  // 6 bit resolution
  // 7 sample-frequency type
  // 8... sample frequencies/range

  if (length < 11) {
    return 0;
  }

  const uint8_t frequencyType = formatDescriptor[7];

  if (frequencyType == 0) {
    // Continuous frequency range:
    // tLowerSamFreq + tUpperSamFreq
    if (length < 14) {
      return 0;
    }

    const uint32_t low = read24(formatDescriptor + 8);
    const uint32_t high = read24(formatDescriptor + 11);

    if (ProjectConfig::USB_RATE_PREFERRED >= low &&
        ProjectConfig::USB_RATE_PREFERRED <= high) {
      return ProjectConfig::USB_RATE_PREFERRED;
    }

    if (ProjectConfig::USB_RATE_FALLBACK >= low &&
        ProjectConfig::USB_RATE_FALLBACK <= high) {
      return ProjectConfig::USB_RATE_FALLBACK;
    }

    return 0;
  }

  const uint8_t requiredLength =
      8 + (frequencyType * 3);

  if (length < requiredLength) {
    return 0;
  }

  bool hasFallback = false;

  for (uint8_t i = 0; i < frequencyType; ++i) {
    const uint32_t rate =
        read24(formatDescriptor + 8 + i * 3);

    if (rate == ProjectConfig::USB_RATE_PREFERRED) {
      return ProjectConfig::USB_RATE_PREFERRED;
    }

    if (rate == ProjectConfig::USB_RATE_FALLBACK) {
      hasFallback = true;
    }
  }

  return hasFallback
      ? ProjectConfig::USB_RATE_FALLBACK
      : 0;
}

bool candidateIsUsable(
    bool inCandidate,
    const AudioStreamInfo &candidate,
    bool formatGood,
    uint32_t selectedRate,
    AudioStreamInfo &output)
{
  if (!inCandidate ||
      candidate.endpointAddress == 0 ||
      candidate.maxPacketSize == 0 ||
      !formatGood ||
      selectedRate == 0) {
    return false;
  }

  output = candidate;
  output.sampleRate = selectedRate;
  output.valid = true;

  return true;
}

bool parseUac1Output(
    const usb_config_desc_t *config,
    AudioStreamInfo &output)
{
  memset(&output, 0, sizeof(output));

  const uint8_t *base =
      reinterpret_cast<const uint8_t *>(config);

  const uint16_t totalLength = config->wTotalLength;

  bool inCandidate = false;
  AudioStreamInfo candidate = {};
  bool formatGood = false;
  uint32_t selectedRate = 0;

  uint16_t offset = 0;

  while (offset + 2 <= totalLength) {
    const uint8_t *descriptor = base + offset;
    const uint8_t length = descriptor[0];
    const uint8_t type = descriptor[1];

    if (length < 2 || offset + length > totalLength) {
      break;
    }

    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE &&
        length >= USB_INTF_DESC_SIZE) {

      if (candidateIsUsable(
              inCandidate,
              candidate,
              formatGood,
              selectedRate,
              output)) {
        return true;
      }

      const usb_intf_desc_t *interfaceDescriptor =
          reinterpret_cast<const usb_intf_desc_t *>(descriptor);

      inCandidate =
          interfaceDescriptor->bInterfaceClass ==
              USB_CLASS_AUDIO_CODE &&
          interfaceDescriptor->bInterfaceSubClass ==
              USB_SUBCLASS_AUDIOSTREAMING &&
          interfaceDescriptor->bAlternateSetting != 0 &&
          interfaceDescriptor->bNumEndpoints > 0;

      memset(&candidate, 0, sizeof(candidate));
      formatGood = false;
      selectedRate = 0;

      if (inCandidate) {
        candidate.interfaceNumber =
            interfaceDescriptor->bInterfaceNumber;
        candidate.alternateSetting =
            interfaceDescriptor->bAlternateSetting;
      }
    }
    else if (inCandidate &&
             type == USB_DESC_CS_INTERFACE &&
             length >= 8 &&
             descriptor[2] == UAC_AS_FORMAT_TYPE &&
             descriptor[3] == UAC_FORMAT_TYPE_I) {

      const uint8_t channels = descriptor[4];
      const uint8_t subframeSize = descriptor[5];
      const uint8_t bitsPerSample = descriptor[6];

      if (channels == 2 &&
          subframeSize == 2 &&
          bitsPerSample == 16) {

        selectedRate =
            chooseRate(descriptor, length);

        formatGood = selectedRate != 0;
      }
    }
    else if (inCandidate &&
             type == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
             length >= USB_EP_DESC_SIZE) {

      const usb_ep_desc_t *endpoint =
          reinterpret_cast<const usb_ep_desc_t *>(descriptor);

      const bool isOut =
          (endpoint->bEndpointAddress &
           USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) == 0;

      const bool isIsochronous =
          (endpoint->bmAttributes &
           USB_BM_ATTRIBUTES_XFERTYPE_MASK) ==
          USB_BM_ATTRIBUTES_XFER_ISOC;

      if (isOut && isIsochronous) {
        candidate.endpointAddress =
            endpoint->bEndpointAddress;

        candidate.maxPacketSize =
            endpoint->wMaxPacketSize & 0x07FF;

        candidate.interval =
            endpoint->bInterval;
      }
    }

    offset += length;
  }

  return candidateIsUsable(
      inCandidate,
      candidate,
      formatGood,
      selectedRate,
      output);
}

void controlTransferCallback(usb_transfer_t *transfer)
{
  ControlWait *wait =
      static_cast<ControlWait *>(transfer->context);

  wait->status = transfer->status;
  wait->done = true;
}

bool setUac1SampleRate(uint8_t endpoint, uint32_t rate)
{
  usb_transfer_t *transfer = NULL;

  if (usb_host_transfer_alloc(
          USB_SETUP_PACKET_SIZE + 3,
          0,
          &transfer) != ESP_OK) {
    return false;
  }

  ControlWait wait = {};
  wait.done = false;
  wait.status = USB_TRANSFER_STATUS_ERROR;

  usb_setup_packet_t *setup =
      reinterpret_cast<usb_setup_packet_t *>(
          transfer->data_buffer);

  setup->bmRequestType =
      USB_BM_REQUEST_TYPE_DIR_OUT |
      USB_BM_REQUEST_TYPE_TYPE_CLASS |
      USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;

  setup->bRequest = UAC_SET_CUR;

  setup->wValue =
      (uint16_t)UAC_EP_SAMPLING_FREQ_CONTROL << 8;

  setup->wIndex = endpoint;
  setup->wLength = 3;

  transfer->data_buffer[USB_SETUP_PACKET_SIZE + 0] =
      rate & 0xFF;

  transfer->data_buffer[USB_SETUP_PACKET_SIZE + 1] =
      (rate >> 8) & 0xFF;

  transfer->data_buffer[USB_SETUP_PACKET_SIZE + 2] =
      (rate >> 16) & 0xFF;

  transfer->device_handle = audioDevice;
  transfer->bEndpointAddress = 0;
  transfer->callback = controlTransferCallback;
  transfer->context = &wait;
  transfer->num_bytes = USB_SETUP_PACKET_SIZE + 3;
  transfer->timeout_ms = 1000;

  const esp_err_t err =
      usb_host_transfer_submit_control(
          usbClient,
          transfer);

  if (err != ESP_OK) {
    usb_host_transfer_free(transfer);
    return false;
  }

  const uint32_t start = millis();

  while (!wait.done &&
         millis() - start < 1500) {
    usb_host_client_handle_events(
        usbClient,
        pdMS_TO_TICKS(10));
  }

  const bool ok =
      wait.done &&
      wait.status ==
          USB_TRANSFER_STATUS_COMPLETED;

  usb_host_transfer_free(transfer);

  return ok;
}

void resetResampler()
{
  usbPacketRateAccumulator = 0;

  resamplePhase = 0;
  resampleCurrent.left = 0;
  resampleCurrent.right = 0;
  resampleHaveCurrent = false;
}

StereoFrame nextUsbFrame(uint32_t outputRate)
{
  if (!resampleHaveCurrent) {
    if (!AudioBuffer::pop(resampleCurrent)) {
      StereoFrame silence = {0, 0};
      AudioBuffer::incrementStarved();
      return silence;
    }

    resampleHaveCurrent = true;
  }

  StereoFrame output = resampleCurrent;

  resamplePhase +=
      ProjectConfig::ADC_PER_CHANNEL_RATE;

  while (resamplePhase >= outputRate) {
    StereoFrame next;

    if (AudioBuffer::pop(next)) {
      resampleCurrent = next;
    } else {
      resampleCurrent.left = 0;
      resampleCurrent.right = 0;
      AudioBuffer::incrementStarved();
    }

    resamplePhase -= outputRate;
  }

  return output;
}

void prepareAndSubmitIso(usb_transfer_t *transfer);

void isoTransferCallback(usb_transfer_t *transfer)
{
  if (!streaming || audioDevice == NULL) {
    return;
  }

  if (transfer->status !=
          USB_TRANSFER_STATUS_COMPLETED &&
      transfer->status !=
          USB_TRANSFER_STATUS_SKIPPED) {

    Serial.printf(
        "[USB] isoch transfer status=%d\n",
        (int)transfer->status);
  }

  prepareAndSubmitIso(transfer);
}

void prepareAndSubmitIso(usb_transfer_t *transfer)
{
  if (!streaming ||
      audioDevice == NULL ||
      !audioStream.valid) {
    return;
  }

  usbPacketRateAccumulator +=
      audioStream.sampleRate;

  uint32_t frames =
      usbPacketRateAccumulator / 1000;

  usbPacketRateAccumulator %= 1000;

  if (frames == 0) {
    frames = 1;
  }

  const uint32_t bytes = frames * 4;

  if (bytes > transfer->data_buffer_size ||
      bytes > audioStream.maxPacketSize) {

    Serial.printf(
        "[USB] packet too large: %lu > buffer=%u mps=%u\n",
        (unsigned long)bytes,
        (unsigned)transfer->data_buffer_size,
        (unsigned)audioStream.maxPacketSize);

    streaming = false;
    return;
  }

  for (uint32_t i = 0; i < frames; ++i) {
    const StereoFrame frame =
        nextUsbFrame(audioStream.sampleRate);

    const uint32_t offset = i * 4;

    transfer->data_buffer[offset + 0] =
        (uint8_t)(frame.left & 0xFF);

    transfer->data_buffer[offset + 1] =
        (uint8_t)((frame.left >> 8) & 0xFF);

    transfer->data_buffer[offset + 2] =
        (uint8_t)(frame.right & 0xFF);

    transfer->data_buffer[offset + 3] =
        (uint8_t)((frame.right >> 8) & 0xFF);
  }

  transfer->device_handle = audioDevice;
  transfer->bEndpointAddress =
      audioStream.endpointAddress;

  transfer->callback = isoTransferCallback;
  transfer->context = NULL;
  transfer->num_bytes = bytes;
  transfer->timeout_ms = 0;

  transfer->isoc_packet_desc[0].num_bytes =
      bytes;

  const esp_err_t err =
      usb_host_transfer_submit(transfer);

  if (err != ESP_OK) {
    Serial.printf(
        "[USB] iso submit failed: %s\n",
        esp_err_to_name(err));

    streaming = false;
  }
}

void freeIsoTransfers()
{
  for (int i = 0;
       i < ProjectConfig::ISO_TRANSFER_COUNT;
       ++i) {

    if (isoTransfers[i] != NULL) {
      usb_host_transfer_free(isoTransfers[i]);
      isoTransfers[i] = NULL;
    }
  }
}

void disconnectAudioDevice()
{
  streaming = false;
  delay(20);

  freeIsoTransfers();

  if (audioDevice != NULL) {
    if (audioStream.valid) {
      usb_host_interface_release(
          usbClient,
          audioDevice,
          audioStream.interfaceNumber);
    }

    usb_host_device_close(
        usbClient,
        audioDevice);
  }

  audioDevice = NULL;
  memset(&audioStream, 0, sizeof(audioStream));

  resetResampler();
  AudioBuffer::clear();

  Serial.println("[USB] audio device disconnected");
}

bool connectAudioDevice(uint8_t address)
{
  if (audioDevice != NULL) {
    return false;
  }

  esp_err_t err =
      usb_host_device_open(
          usbClient,
          address,
          &audioDevice);

  if (err != ESP_OK) {
    Serial.printf(
        "[USB] device open failed: %s\n",
        esp_err_to_name(err));

    audioDevice = NULL;
    return false;
  }

  const usb_device_desc_t *deviceDescriptor = NULL;

  if (usb_host_get_device_descriptor(
          audioDevice,
          &deviceDescriptor) == ESP_OK &&
      deviceDescriptor != NULL) {

    Serial.printf(
        "[USB] device VID=%04X PID=%04X\n",
        deviceDescriptor->idVendor,
        deviceDescriptor->idProduct);
  }

  const usb_config_desc_t *configDescriptor = NULL;

  err = usb_host_get_active_config_descriptor(
      audioDevice,
      &configDescriptor);

  if (err != ESP_OK ||
      configDescriptor == NULL) {

    Serial.println(
        "[USB] no active configuration descriptor");

    usb_host_device_close(
        usbClient,
        audioDevice);

    audioDevice = NULL;

    return false;
  }

  AudioStreamInfo found = {};

  if (!parseUac1Output(
          configDescriptor,
          found)) {

    Serial.println(
        "[USB] no supported UAC1 stereo 16-bit 48k/44.1k output stream");

    usb_host_device_close(
        usbClient,
        audioDevice);

    audioDevice = NULL;

    return false;
  }

  Serial.printf(
      "[USB] UAC1 OUT: iface=%u alt=%u ep=0x%02X mps=%u rate=%lu\n",
      found.interfaceNumber,
      found.alternateSetting,
      found.endpointAddress,
      found.maxPacketSize,
      (unsigned long)found.sampleRate);

  err = usb_host_interface_claim(
      usbClient,
      audioDevice,
      found.interfaceNumber,
      found.alternateSetting);

  if (err != ESP_OK) {
    Serial.printf(
        "[USB] interface claim failed: %s\n",
        esp_err_to_name(err));

    usb_host_device_close(
        usbClient,
        audioDevice);

    audioDevice = NULL;

    return false;
  }

  audioStream = found;

  if (setUac1SampleRate(
          found.endpointAddress,
          found.sampleRate)) {

    Serial.println(
        "[USB] sample rate SET_CUR accepted");
  } else {
    Serial.println(
        "[USB] sample rate SET_CUR not accepted; trying stream anyway");
  }

  resetResampler();
  AudioBuffer::clear();

  for (int i = 0;
       i < ProjectConfig::ISO_TRANSFER_COUNT;
       ++i) {

    err = usb_host_transfer_alloc(
        found.maxPacketSize,
        1,
        &isoTransfers[i]);

    if (err != ESP_OK) {
      Serial.printf(
          "[USB] isoch alloc %d failed: %s\n",
          i,
          esp_err_to_name(err));

      streaming = false;
      freeIsoTransfers();

      usb_host_interface_release(
          usbClient,
          audioDevice,
          found.interfaceNumber);

      usb_host_device_close(
          usbClient,
          audioDevice);

      audioDevice = NULL;
      memset(
          &audioStream,
          0,
          sizeof(audioStream));

      return false;
    }
  }

  const uint32_t waitStart = millis();

  while (AudioBuffer::available() <
             ProjectConfig::USB_PREFILL_FRAMES &&
         millis() - waitStart < 100) {
    delay(1);
  }

  streaming = true;

  for (int i = 0;
       i < ProjectConfig::ISO_TRANSFER_COUNT;
       ++i) {

    prepareAndSubmitIso(isoTransfers[i]);

    if (!streaming) {
      break;
    }
  }

  if (!streaming) {
    disconnectAudioDevice();
    return false;
  }

  Serial.println("[USB] audio streaming started");

  return true;
}

void usbClientEvent(
    const usb_host_client_event_msg_t *eventMessage,
    void *arg)
{
  (void)arg;

  if (eventMessage->event ==
      USB_HOST_CLIENT_EVENT_NEW_DEV) {

    if (audioDevice == NULL) {
      pendingDeviceAddress =
          eventMessage->new_dev.address;
    }
  }
  else if (eventMessage->event ==
           USB_HOST_CLIENT_EVENT_DEV_GONE) {

    if (audioDevice != NULL &&
        eventMessage->dev_gone.dev_hdl ==
            audioDevice) {

      pendingDisconnect = true;
      streaming = false;
    }
  }
}

void usbLibraryTask(void *arg)
{
  SemaphoreHandle_t ready =
      (SemaphoreHandle_t)arg;

  usb_host_config_t config = {};
  config.skip_phy_setup = false;
  config.intr_flags = ESP_INTR_FLAG_LEVEL1;

  const esp_err_t err =
      usb_host_install(&config);

  if (err != ESP_OK) {
    Serial.printf(
        "[USB] usb_host_install failed: %s\n",
        esp_err_to_name(err));

    xSemaphoreGive(ready);
    vTaskDelete(NULL);
    return;
  }

  Serial.println("[USB] host library installed");

  xSemaphoreGive(ready);

  while (true) {
    uint32_t eventFlags = 0;

    usb_host_lib_handle_events(
        portMAX_DELAY,
        &eventFlags);
  }
}

void usbClientTask(void *arg)
{
  (void)arg;

  usb_host_client_config_t config = {};
  config.is_synchronous = false;
  config.max_num_event_msg = 5;
  config.async.client_event_callback =
      usbClientEvent;
  config.async.callback_arg = NULL;

  const esp_err_t err =
      usb_host_client_register(
          &config,
          &usbClient);

  if (err != ESP_OK) {
    Serial.printf(
        "[USB] client register failed: %s\n",
        esp_err_to_name(err));

    vTaskDelete(NULL);
    return;
  }

  Serial.println(
      "[USB] client registered; waiting for NRG");

  while (true) {
    usb_host_client_handle_events(
        usbClient,
        pdMS_TO_TICKS(10));

    if (pendingDisconnect) {
      pendingDisconnect = false;
      disconnectAudioDevice();
    }

    const uint8_t address =
        pendingDeviceAddress;

    if (address != 0 &&
        audioDevice == NULL) {

      pendingDeviceAddress = 0;
      connectAudioDevice(address);
    }
  }
}

} // namespace

namespace UsbAudioHost {

bool begin()
{
  SemaphoreHandle_t ready =
      xSemaphoreCreateBinary();

  if (ready == NULL) {
    return false;
  }

  BaseType_t taskResult =
      xTaskCreatePinnedToCore(
          usbLibraryTask,
          "usb_lib",
          4096,
          ready,
          5,
          NULL,
          0);

  if (taskResult != pdPASS) {
    vSemaphoreDelete(ready);
    return false;
  }

  if (xSemaphoreTake(
          ready,
          pdMS_TO_TICKS(3000)) != pdTRUE) {

    Serial.println(
        "[USB] host install timeout");

    vSemaphoreDelete(ready);

    return false;
  }

  vSemaphoreDelete(ready);

  taskResult =
      xTaskCreatePinnedToCore(
          usbClientTask,
          "usb_client",
          6144,
          NULL,
          6,
          NULL,
          0);

  return taskResult == pdPASS;
}

bool isStreaming()
{
  return streaming;
}

uint32_t sampleRate()
{
  return audioStream.sampleRate;
}

} // namespace UsbAudioHost
