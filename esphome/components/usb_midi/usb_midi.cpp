#include "usb_midi.h"

#include <algorithm>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace usb_midi {

static const char *const TAG = "usb_midi";

// USB-MIDI 1.0: the Code Index Number (low nibble of each 4-byte event
// packet) gives how many of the 3 MIDI bytes after it are used.
static const uint8_t CIN_LENGTH[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};

// Length of a MIDI message from its status byte (SysEx aside).
static size_t message_length(uint8_t status) {
  if (status < 0xF0)
    return (status & 0xE0) == 0xC0 ? 2 : 3;  // program change, channel pressure: 2
  switch (status) {
    case 0xF1:
    case 0xF3:
      return 2;
    case 0xF2:
      return 3;
    default:
      return 1;  // F4-F7 and the real-time messages F8-FF
  }
}

void UsbMidi::setup() {
  this->tx_lock_ = xSemaphoreCreateMutex();
  usb_host_client_config_t config{};
  config.is_synchronous = false;
  config.max_num_event_msg = 5;
  config.async.client_event_callback = &UsbMidi::client_event_cb_;
  config.async.callback_arg = this;
  const esp_err_t err = usb_host_client_register(&config, &this->client_);
  if (err != ESP_OK || this->tx_lock_ == nullptr) {
    ESP_LOGE(TAG, "Could not register a USB host client: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  if (xTaskCreatePinnedToCore(&UsbMidi::client_task_, "usb_midi", 4096, this, 5, &this->task_, tskNO_AFFINITY) !=
      pdPASS) {
    ESP_LOGE(TAG, "Could not start the USB MIDI task");
    this->mark_failed();
  }
}

// All USB work happens here: client events, transfer callbacks, opening and
// closing the device.
void UsbMidi::client_task_(void *arg) {
  auto *self = static_cast<UsbMidi *>(arg);
  // A device plugged in before this client registered sends no NEW_DEV.
  uint8_t addresses[8];
  int count = 0;
  if (usb_host_device_addr_list_fill(sizeof(addresses), addresses, &count) == ESP_OK) {
    for (int i = 0; i < count && self->device_ == nullptr; i++)
      self->open_device_(addresses[i]);
  }
  for (;;)
    usb_host_client_handle_events(self->client_, portMAX_DELAY);
}

void UsbMidi::client_event_cb_(const usb_host_client_event_msg_t *msg, void *arg) {
  auto *self = static_cast<UsbMidi *>(arg);
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (self->device_ == nullptr)
      self->open_device_(msg->new_dev.address);
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (self->device_ != nullptr && msg->dev_gone.dev_hdl == self->device_) {
      self->gone_ = true;
      self->connected_.store(false);
      ESP_LOGI(TAG, "USB MIDI device %04x:%04x removed", self->vid_, self->pid_);
      self->close_if_idle_();
    }
  }
}

void UsbMidi::open_device_(uint8_t address) {
  usb_device_handle_t device = nullptr;
  if (usb_host_device_open(this->client_, address, &device) != ESP_OK)
    return;

  // The first Audio class / MIDI Streaming interface, and its bulk (or
  // interrupt) IN and OUT endpoints.
  const usb_config_desc_t *config = nullptr;
  int interface = -1, alternate = 0;
  uint8_t ep_in = 0, ep_out = 0;
  uint16_t mps_in = 0, mps_out = 0;
  if (usb_host_get_active_config_descriptor(device, &config) == ESP_OK && config != nullptr) {
    const uint8_t *desc = reinterpret_cast<const uint8_t *>(config);
    const uint16_t total = config->wTotalLength;
    bool in_midi = false;
    for (uint16_t offset = 0; offset + 2 <= total;) {
      const uint8_t length = desc[offset], type = desc[offset + 1];
      if (length < 2 || offset + length > total)
        break;
      if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && length >= 9) {
        if (interface >= 0 && ep_in != 0)
          break;
        in_midi = desc[offset + 5] == 0x01 && desc[offset + 6] == 0x03;
        if (in_midi) {
          interface = desc[offset + 2];
          alternate = desc[offset + 3];
          ep_in = ep_out = 0;
        }
      } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && length >= 7 && in_midi) {
        const uint8_t endpoint = desc[offset + 2], kind = desc[offset + 3] & 0x03;
        const uint16_t mps = (desc[offset + 4] | (desc[offset + 5] << 8)) & 0x7FF;
        if (kind == 2 || kind == 3) {  // bulk or interrupt
          if ((endpoint & 0x80) != 0 && ep_in == 0) {
            ep_in = endpoint;
            mps_in = mps;
          } else if ((endpoint & 0x80) == 0 && ep_out == 0) {
            ep_out = endpoint;
            mps_out = mps;
          }
        }
      }
      offset += length;
    }
  }
  if (interface < 0 || ep_in == 0 || mps_in == 0) {
    usb_host_device_close(this->client_, device);  // not a MIDI device
    return;
  }

  const usb_device_desc_t *device_desc = nullptr;
  if (usb_host_get_device_descriptor(device, &device_desc) == ESP_OK && device_desc != nullptr) {
    this->vid_ = device_desc->idVendor;
    this->pid_ = device_desc->idProduct;
  }
  esp_err_t err = usb_host_interface_claim(this->client_, device, interface, alternate);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "USB MIDI device %04x:%04x: interface %d is in use (%s)", this->vid_, this->pid_, interface,
             esp_err_to_name(err));
    usb_host_device_close(this->client_, device);
    return;
  }
  // IN transfers are whole max-size packets; OUT ones carry whole 4-byte
  // event packets, at most one max-size packet.
  const size_t in_size = std::max<size_t>(mps_in, 64);
  const size_t out_size = ep_out != 0 ? std::max<size_t>(std::min<size_t>(mps_out, 64) & ~size_t{3}, 4) : 0;
  err = usb_host_transfer_alloc(in_size, 0, &this->in_transfer_);
  if (err == ESP_OK && out_size != 0)
    err = usb_host_transfer_alloc(out_size, 0, &this->out_transfer_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "No memory for USB MIDI transfers");
    if (this->in_transfer_ != nullptr)
      usb_host_transfer_free(this->in_transfer_);
    this->in_transfer_ = nullptr;
    usb_host_interface_release(this->client_, device, interface);
    usb_host_device_close(this->client_, device);
    return;
  }

  this->device_ = device;
  this->interface_ = interface;
  this->ep_in_ = ep_in;
  this->ep_out_ = ep_out;
  this->gone_ = false;
  this->rx_head_.store(0);
  this->rx_tail_.store(0);
  xSemaphoreTake(this->tx_lock_, portMAX_DELAY);
  this->tx_len_ = 0;
  this->out_busy_.store(false);
  xSemaphoreGive(this->tx_lock_);

  usb_transfer_t *in = this->in_transfer_;
  in->device_handle = device;
  in->bEndpointAddress = ep_in;
  in->callback = &UsbMidi::in_transfer_cb_;
  in->context = this;
  in->num_bytes = in_size;
  if (this->out_transfer_ != nullptr) {
    this->out_transfer_->device_handle = device;
    this->out_transfer_->bEndpointAddress = ep_out;
    this->out_transfer_->callback = &UsbMidi::out_transfer_cb_;
    this->out_transfer_->context = this;
  }
  this->in_busy_ = usb_host_transfer_submit(in) == ESP_OK;
  this->connected_.store(true);
  ESP_LOGI(TAG, "USB MIDI device %04x:%04x connected: interface %d, in 0x%02x%s", this->vid_, this->pid_, interface,
           ep_in, ep_out != 0 ? ", out too" : ", no out");
}

// Runs on the client task. Frees everything once the device is gone and no
// transfer is still with the host stack.
void UsbMidi::close_if_idle_() {
  if (!this->gone_ || this->in_busy_ || this->out_busy_.load() || this->device_ == nullptr)
    return;
  xSemaphoreTake(this->tx_lock_, portMAX_DELAY);
  if (this->out_transfer_ != nullptr)
    usb_host_transfer_free(this->out_transfer_);
  this->out_transfer_ = nullptr;
  this->tx_len_ = 0;
  xSemaphoreGive(this->tx_lock_);
  if (this->in_transfer_ != nullptr)
    usb_host_transfer_free(this->in_transfer_);
  this->in_transfer_ = nullptr;
  usb_host_interface_release(this->client_, this->device_, this->interface_);
  usb_host_device_close(this->client_, this->device_);
  this->device_ = nullptr;
  this->gone_ = false;
}

void UsbMidi::in_transfer_cb_(usb_transfer_t *transfer) {
  auto *self = static_cast<UsbMidi *>(transfer->context);
  self->in_busy_ = false;
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
    for (int i = 0; i + 4 <= transfer->actual_num_bytes; i += 4)
      self->push_packet_(transfer->data_buffer + i);
  }
  const bool stopped = transfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
                       transfer->status == USB_TRANSFER_STATUS_CANCELED;
  if (!self->gone_ && !stopped) {
    transfer->num_bytes = transfer->data_buffer_size;
    self->in_busy_ = usb_host_transfer_submit(transfer) == ESP_OK;
  }
  self->close_if_idle_();
}

// One USB-MIDI event packet into the byte ring. A full ring drops the
// message rather than splitting it.
void UsbMidi::push_packet_(const uint8_t *packet) {
  const size_t length = CIN_LENGTH[packet[0] & 0x0F];
  if (length == 0)
    return;  // reserved code or padding
  const size_t head = this->rx_head_.load(std::memory_order_relaxed);
  const size_t tail = this->rx_tail_.load(std::memory_order_acquire);
  if (RX_BUFFER_SIZE - 1 - (head - tail + RX_BUFFER_SIZE) % RX_BUFFER_SIZE < length) {
    this->rx_dropped_++;
    return;
  }
  for (size_t i = 0; i < length; i++)
    this->rx_[(head + i) % RX_BUFFER_SIZE] = packet[1 + i];
  this->rx_head_.store((head + length) % RX_BUFFER_SIZE, std::memory_order_release);
  if (packet[1] >= 0x80)
    this->messages_in_++;
  ESP_LOGV(TAG, "in: %02X %02X %02X (%u bytes)", packet[1], packet[2], packet[3], static_cast<unsigned>(length));
}

size_t UsbMidi::read(uint8_t *buf, size_t len) {
  if (buf == nullptr)
    return 0;
  size_t tail = this->rx_tail_.load(std::memory_order_relaxed);
  const size_t head = this->rx_head_.load(std::memory_order_acquire);
  size_t count = 0;
  while (count < len && tail != head) {
    buf[count++] = this->rx_[tail];
    tail = (tail + 1) % RX_BUFFER_SIZE;
  }
  this->rx_tail_.store(tail, std::memory_order_release);
  return count;
}

void UsbMidi::queue_packet_(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2) {
  if (this->tx_len_ + 4 > TX_BUFFER_SIZE)
    return;
  uint8_t *packet = this->tx_ + this->tx_len_;
  packet[0] = cin & 0x0F;  // cable 0
  packet[1] = b0;
  packet[2] = b1;
  packet[3] = b2;
  this->tx_len_ += 4;
}

bool UsbMidi::write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0 || !this->connected_.load() || this->tx_lock_ == nullptr)
    return false;
  xSemaphoreTake(this->tx_lock_, portMAX_DELAY);
  if (this->out_transfer_ == nullptr || this->tx_len_ + (len + 2) / 3 * 4 > TX_BUFFER_SIZE) {
    xSemaphoreGive(this->tx_lock_);
    return false;
  }
  size_t i = 0;
  bool sysex = false;
  while (i < len) {
    const uint8_t status = data[i];
    if (status == 0xF0)
      sysex = true;
    if (sysex) {
      // SysEx: 3 bytes per packet; the one holding F7 says how many it has.
      uint8_t chunk[3] = {0, 0, 0};
      size_t n = 0;
      while (i < len && n < 3 && sysex) {
        chunk[n++] = data[i];
        sysex = data[i++] != 0xF7;
      }
      queue_packet_(sysex ? 0x4 : static_cast<uint8_t>(4 + n), chunk[0], chunk[1], chunk[2]);
    } else if (status >= 0x80) {
      const size_t n = message_length(status);
      if (i + n > len)
        break;  // incomplete message
      const uint8_t cin = status < 0xF0 ? static_cast<uint8_t>(status >> 4)
                          : status >= 0xF8 ? 0xF
                          : n == 1 ? 0x5
                          : n == 2 ? 0x2
                                   : 0x3;
      queue_packet_(cin, status, n > 1 ? data[i + 1] : 0, n > 2 ? data[i + 2] : 0);
      i += n;
    } else {
      i++;  // a data byte without a status (running status is not supported)
    }
  }
  this->submit_out_locked_();
  xSemaphoreGive(this->tx_lock_);
  return true;
}

// With tx_lock_ held: send the next queued packets if no OUT transfer is out.
void UsbMidi::submit_out_locked_() {
  usb_transfer_t *out = this->out_transfer_;
  if (out == nullptr || this->out_busy_.load() || this->tx_len_ == 0 || !this->connected_.load())
    return;
  const size_t n = std::min(this->tx_len_, out->data_buffer_size & ~size_t{3});
  std::memcpy(out->data_buffer, this->tx_, n);
  std::memmove(this->tx_, this->tx_ + n, this->tx_len_ - n);
  this->tx_len_ -= n;
  out->num_bytes = n;
  this->out_busy_.store(usb_host_transfer_submit(out) == ESP_OK);
}

void UsbMidi::out_transfer_cb_(usb_transfer_t *transfer) {
  auto *self = static_cast<UsbMidi *>(transfer->context);
  xSemaphoreTake(self->tx_lock_, portMAX_DELAY);
  self->out_busy_.store(false);
  if (!self->gone_)
    self->submit_out_locked_();
  xSemaphoreGive(self->tx_lock_);
  self->close_if_idle_();
}

void UsbMidi::dump_config() {
  ESP_LOGCONFIG(TAG, "USB MIDI:");
  if (this->is_failed()) {
    ESP_LOGCONFIG(TAG, "  Failed to start");
  } else if (this->is_connected()) {
    ESP_LOGCONFIG(TAG, "  Device %04x:%04x, %u messages in, %u bytes dropped", this->vid_, this->pid_,
                  static_cast<unsigned>(this->messages_in_), static_cast<unsigned>(this->rx_dropped_));
  } else {
    ESP_LOGCONFIG(TAG, "  No device connected");
  }
}

}  // namespace usb_midi
}  // namespace esphome
