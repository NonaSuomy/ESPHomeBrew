"""Class-compliant USB MIDI devices (USB-MIDI cables, keyboards) on the usb_host bus.

    usb_midi:
      id: usb_midi_port

The first device with a USB-MIDI (Audio class, MIDI Streaming) interface is
used; it is picked up when plugged in and released when pulled. Incoming
messages are kept as plain MIDI bytes until read. From lambdas:
id(usb_midi_port).read(buf, len), id(usb_midi_port).write(bytes, len) (whole
messages), id(usb_midi_port).is_connected(). The PAPP loader passes them to
apps.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["usb_host"]

usb_midi_ns = cg.esphome_ns.namespace("usb_midi")
UsbMidi = usb_midi_ns.class_("UsbMidi", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UsbMidi),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_USB_MIDI")
