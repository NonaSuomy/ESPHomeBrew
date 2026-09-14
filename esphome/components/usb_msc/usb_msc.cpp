#include "usb_msc.h"

#include "esp_vfs_fat.h"
#include "esphome/core/log.h"

namespace esphome {
namespace usb_msc {

static const char *const TAG = "usb_msc";

void UsbMsc::setup() {
  msc_host_driver_config_t config{};
  config.create_backround_task = true;
  config.task_priority = 5;
  config.stack_size = 4096;
  config.core_id = tskNO_AFFINITY;
  config.callback = &UsbMsc::event_callback_;
  config.callback_arg = this;
  const esp_err_t err = msc_host_install(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not start the USB mass storage driver: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  this->installed_ = true;
}

// Runs on the MSC driver's task: only note what happened; loop() acts on it.
void UsbMsc::event_callback_(const msc_host_event_t *event, void *arg) {
  auto *self = static_cast<UsbMsc *>(arg);
  if (event->event == msc_host_event_t::MSC_DEVICE_CONNECTED) {
    self->connected_address_ = event->device.address;
  } else if (event->event == msc_host_event_t::MSC_DEVICE_DISCONNECTED) {
    self->disconnected_ = true;
  }
}

void UsbMsc::loop() {
  if (this->disconnected_) {
    this->disconnected_ = false;
    if (this->vfs_ != nullptr) {
      msc_host_vfs_unregister(this->vfs_);
      this->vfs_ = nullptr;
    }
    if (this->device_ != nullptr) {
      msc_host_uninstall_device(this->device_);
      this->device_ = nullptr;
    }
    ESP_LOGI(TAG, "USB stick removed; %s unmounted", this->mount_point_.c_str());
  }
  const uint8_t address = this->connected_address_;
  if (address != 0) {
    this->connected_address_ = 0;
    if (this->device_ == nullptr) {
      const esp_err_t err = msc_host_install_device(address, &this->device_);
      if (err != ESP_OK) {
        this->device_ = nullptr;
        ESP_LOGW(TAG, "USB stick at address %u could not be opened: %s", address, esp_err_to_name(err));
        return;
      }
      this->mount_device_();
    }
  }
}

bool UsbMsc::mount_device_() {
  if (this->device_ == nullptr)
    return false;
  if (this->vfs_ != nullptr)
    return true;
  esp_vfs_fat_mount_config_t mount{};
  mount.format_if_mount_failed = false;  // never wipe someone's stick
  mount.max_files = this->max_files_;
  mount.allocation_unit_size = 0;
  const esp_err_t err = msc_host_vfs_register(this->device_, this->mount_point_.c_str(), &mount, &this->vfs_);
  if (err != ESP_OK) {
    this->vfs_ = nullptr;
    ESP_LOGW(TAG, "USB stick could not be mounted at %s: %s (FAT/FAT32 formatted?)", this->mount_point_.c_str(),
             esp_err_to_name(err));
    return false;
  }
  msc_host_device_info_t info{};
  if (msc_host_get_device_info(this->device_, &info) == ESP_OK) {
    ESP_LOGI(TAG, "USB stick mounted at %s: %lu MB", this->mount_point_.c_str(),
             static_cast<unsigned long>(static_cast<uint64_t>(info.sector_count) * info.sector_size / (1024 * 1024)));
  } else {
    ESP_LOGI(TAG, "USB stick mounted at %s", this->mount_point_.c_str());
  }
  return true;
}

bool UsbMsc::mount() {
  if (this->device_ == nullptr) {
    ESP_LOGI(TAG, "No USB stick plugged in");
    return false;
  }
  return this->mount_device_();
}

void UsbMsc::unmount() {
  if (this->vfs_ == nullptr)
    return;
  msc_host_vfs_unregister(this->vfs_);
  this->vfs_ = nullptr;
  ESP_LOGI(TAG, "%s unmounted; the USB stick can be pulled", this->mount_point_.c_str());
}

void UsbMsc::dump_config() {
  ESP_LOGCONFIG(TAG, "USB mass storage:");
  ESP_LOGCONFIG(TAG, "  Mount point: %s", this->mount_point_.c_str());
  ESP_LOGCONFIG(TAG, "  Max open files: %d", this->max_files_);
  if (this->is_failed())
    ESP_LOGCONFIG(TAG, "  Driver failed to start");
}

}  // namespace usb_msc
}  // namespace esphome
