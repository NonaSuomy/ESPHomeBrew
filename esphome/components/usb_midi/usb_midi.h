#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esphome/core/component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace esphome {
namespace usb_midi {

// Size of the incoming byte buffer and of the outgoing USB-MIDI packet queue.
static constexpr size_t RX_BUFFER_SIZE = 2048;
static constexpr size_t TX_BUFFER_SIZE = 512;

// A class-compliant USB MIDI device (USB-MIDI cable or keyboard) on the
// usb_host bus. Its own USB host client task opens the device, claims the
// MIDI Streaming interface and keeps a bulk IN transfer running; incoming
// USB-MIDI event packets become plain MIDI bytes in a single-producer,
// single-consumer ring that read() drains.
class UsbMidi : public Component {
 public:
  void setup() override;
  void dump_config() override;
  // After usb_host has installed the USB host library.
  float get_setup_priority() const override { return setup_priority::LATE; }

  bool is_connected() const { return this->connected_.load(); }
  // Copies up to len received MIDI bytes into buf and returns how many.
  // One reader at a time (the PAPP loader, or a lambda).
  size_t read(uint8_t *buf, size_t len);
  // Sends whole MIDI messages (status byte first; SysEx F0 ... F7 included).
  // False when no device is connected or the queue is full.
  bool write(const uint8_t *data, size_t len);

 protected:
  static void client_task_(void *arg);
  static void client_event_cb_(const usb_host_client_event_msg_t *msg, void *arg);
  static void in_transfer_cb_(usb_transfer_t *transfer);
  static void out_transfer_cb_(usb_transfer_t *transfer);

  void open_device_(uint8_t address);
  void close_if_idle_();
  void push_packet_(const uint8_t *packet);
  void queue_packet_(uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2);
  void submit_out_locked_();

  usb_host_client_handle_t client_{nullptr};
  TaskHandle_t task_{nullptr};
  SemaphoreHandle_t tx_lock_{nullptr};

  // Owned by the client task.
  usb_device_handle_t device_{nullptr};
  usb_transfer_t *in_transfer_{nullptr};
  usb_transfer_t *out_transfer_{nullptr};
  uint8_t interface_{0};
  uint8_t ep_in_{0};
  uint8_t ep_out_{0};
  bool in_busy_{false};
  bool gone_{false};

  std::atomic<bool> connected_{false};
  std::atomic<bool> out_busy_{false};  // guarded by tx_lock_ when set

  // Received MIDI bytes: written by the client task, read by read().
  uint8_t rx_[RX_BUFFER_SIZE];
  std::atomic<size_t> rx_head_{0};
  std::atomic<size_t> rx_tail_{0};
  uint32_t rx_dropped_{0};

  // Outgoing USB-MIDI packets, 4 bytes each; guarded by tx_lock_.
  uint8_t tx_[TX_BUFFER_SIZE];
  size_t tx_len_{0};

  uint16_t vid_{0};
  uint16_t pid_{0};
  uint32_t messages_in_{0};
};

}  // namespace usb_midi
}  // namespace esphome
