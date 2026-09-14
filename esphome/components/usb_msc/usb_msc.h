#pragma once

#include <string>

#include "esphome/core/component.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

namespace esphome {
namespace usb_msc {

// A USB stick mounted as FAT at mount_point (default /usb0): mounted when it
// is plugged in, unmounted when it is pulled. The MSC driver's own task
// reports connect/disconnect; installing and mounting happen in loop().
class UsbMsc : public Component {
 public:
  void set_mount_point(const std::string &path) { this->mount_point_ = path; }
  void set_max_files(int files) { this->max_files_ = files; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  // After usb_host has installed the USB host library.
  float get_setup_priority() const override { return setup_priority::LATE; }

  bool is_mounted() const { return this->vfs_ != nullptr; }
  // Mounts a connected stick again after unmount(). False when none is plugged in.
  bool mount();
  // Unmounts so the stick can be pulled safely; it stays unmounted until
  // mount() or it is plugged in again.
  void unmount();

 protected:
  static void event_callback_(const msc_host_event_t *event, void *arg);
  bool mount_device_();

  std::string mount_point_{"/usb0"};
  int max_files_{8};
  volatile uint8_t connected_address_{0};  // set by the driver task, 0 = none
  volatile bool disconnected_{false};
  msc_host_device_handle_t device_{nullptr};
  msc_host_vfs_handle_t vfs_{nullptr};
  bool installed_{false};
};

}  // namespace usb_msc
}  // namespace esphome
