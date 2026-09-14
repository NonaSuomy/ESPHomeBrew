"""USB mass storage (a USB stick) mounted as a FAT folder, next to usb_host devices.

    usb_msc:
      id: usb_storage
      mount_point: /usb0     # default

The stick is mounted when it is plugged in and unmounted when it is pulled.
From lambdas: id(usb_storage).mount() (again after an eject),
id(usb_storage).unmount() (before pulling it), id(usb_storage).is_mounted().
"""

import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["usb_host"]

CONF_MOUNT_POINT = "mount_point"
CONF_MAX_FILES = "max_files"

usb_msc_ns = cg.esphome_ns.namespace("usb_msc")
UsbMsc = usb_msc_ns.class_("UsbMsc", cg.Component)


def validate_mount_point(value):
    value = cv.string_strict(value).rstrip("/")
    if not value.startswith("/") or value.count("/") != 1 or len(value) < 2:
        raise cv.Invalid("mount_point is one folder at the root, such as /usb0")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UsbMsc),
        cv.Optional(CONF_MOUNT_POINT, default="/usb0"): validate_mount_point,
        cv.Optional(CONF_MAX_FILES, default=8): cv.int_range(min=1, max=32),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # The ESP-IDF USB mass storage class driver; it runs next to the other
    # usb_host clients (usb_hidx and friends) on the same host stack.
    esp32.add_idf_component(name="espressif/usb_host_msc", ref="^1.1.3")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mount_point(config[CONF_MOUNT_POINT]))
    cg.add(var.set_max_files(config[CONF_MAX_FILES]))
