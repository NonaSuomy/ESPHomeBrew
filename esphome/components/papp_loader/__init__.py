from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, display, esp32, lvgl, sensor, speaker, touchscreen

from esphome.const import CONF_ID, CONF_PATH

DEPENDENCIES = ["network"]
AUTO_LOAD = ["binary_sensor", "sensor", "speaker", "touchscreen"]


CONF_AUTOSTART = "autostart"
CONF_CATALOG_URL = "catalog_url"
CONF_DISPLAY_ID = "display_id"
CONF_TOUCHSCREEN_ID = "touchscreen_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_TOGGLE_BUTTON = "toggle_button"
CONF_LAUNCH_BUTTON = "launch_button"
CONF_USB_HIDX_ID = "usb_hidx_id"
CONF_LVGL_ID = "lvgl_id"
CONF_FIRE_BUTTON = "fire_button"
CONF_TOUCH_BUTTON = "touch_button"
CONF_ADC_BUTTON_SENSOR = "adc_button_sensor"
CONF_LEFT_STICK_X_SENSOR = "left_stick_x_sensor"
CONF_LEFT_STICK_Y_SENSOR = "left_stick_y_sensor"
CONF_RIGHT_STICK_X_SENSOR = "right_stick_x_sensor"
CONF_RIGHT_STICK_Y_SENSOR = "right_stick_y_sensor"

papp_loader_ns = cg.esphome_ns.namespace("papp_loader")
PappLoader = papp_loader_ns.class_("PappLoader", cg.Component)
LaunchUrlAction = papp_loader_ns.class_("LaunchUrlAction", automation.Action)
USBHIDXComponent = cg.esphome_ns.namespace("usb_hidx").class_("USBHIDXComponent")
LvglComponent = cg.esphome_ns.namespace("lvgl").class_("LvglComponent")

BUTTON_FIELDS = {
    "up": 0,
    "right": 1,
    "down": 2,
    "left": 3,
    "select": 4,
    "start": 5,
    "a": 6,
    "b": 7,
    "x": 8,
    "y": 9,
    "l": 10,
    "r": 11,
    "menu": 12,
    "volume": 13,
}

BUTTON_SCHEMAS = {
    cv.Optional(f"button_{name}"): cv.use_id(binary_sensor.BinarySensor)
    for name in BUTTON_FIELDS
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PappLoader),
        cv.Required(CONF_PATH): cv.string,
        cv.Optional(CONF_AUTOSTART, default=False): cv.boolean,
        cv.Optional(CONF_CATALOG_URL): cv.string,
        cv.Required(CONF_DISPLAY_ID): cv.use_id(display.Display),
        cv.Optional(CONF_TOUCHSCREEN_ID): cv.use_id(touchscreen.Touchscreen),
        cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_LVGL_ID): cv.use_id(LvglComponent),
        cv.Optional(CONF_TOGGLE_BUTTON): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_LAUNCH_BUTTON): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_FIRE_BUTTON): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_TOUCH_BUTTON): cv.use_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_ADC_BUTTON_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_LEFT_STICK_X_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_LEFT_STICK_Y_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_RIGHT_STICK_X_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_RIGHT_STICK_Y_SENSOR): cv.use_id(sensor.Sensor),
        # Optional because network loading and the PAPP runtime can be used
        # without USB HIDX. When present, bind to the existing USBHIDX ID.
        cv.Optional(CONF_USB_HIDX_ID): cv.use_id(USBHIDXComponent),
        **BUTTON_SCHEMAS,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # esp_http_client is intentionally omitted from ESPHome's default P4
    # component set. Pull it in here because network PAPP loading uses it
    # directly, and enable the IDF TLS component for HTTPS URLs.
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.include_builtin_idf_component("esp-tls")
    # ESPHome renamed this helper between releases. Either variant enables
    # the IDF CA bundle used by the HTTPS path; HTTP remains available too.
    if hasattr(esp32, "require_certificate_bundle"):
        esp32.require_certificate_bundle()
    elif hasattr(esp32, "require_full_certificate_bundle"):
        esp32.require_full_certificate_bundle()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_path(config[CONF_PATH]))
    cg.add(var.set_autostart(config[CONF_AUTOSTART]))
    if catalog_url := config.get(CONF_CATALOG_URL):
        cg.add(var.set_catalog_url(catalog_url))

    display_var = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(display_var))

    if touchscreen_id := config.get(CONF_TOUCHSCREEN_ID):
        touchscreen_var = await cg.get_variable(touchscreen_id)
        cg.add(var.set_touchscreen(touchscreen_var))

    if speaker_id := config.get(CONF_SPEAKER_ID):
        speaker_var = await cg.get_variable(speaker_id)
        cg.add(var.set_speaker(speaker_var))

    if lvgl_id := config.get(CONF_LVGL_ID):
        cg.add_define("PAPP_LOADER_USE_LVGL")
        lvgl_var = await cg.get_variable(lvgl_id)
        cg.add(var.set_lvgl(lvgl_var))

    if toggle_button_id := config.get(CONF_TOGGLE_BUTTON):
        toggle_button_var = await cg.get_variable(toggle_button_id)
        cg.add(var.set_toggle_button(toggle_button_var))

    if launch_button_id := config.get(CONF_LAUNCH_BUTTON):
        launch_button_var = await cg.get_variable(launch_button_id)
        cg.add(var.set_launch_button(launch_button_var))

    if fire_button_id := config.get(CONF_FIRE_BUTTON):
        fire_button_var = await cg.get_variable(fire_button_id)
        cg.add(var.set_fire_button(fire_button_var))

    if touch_button_id := config.get(CONF_TOUCH_BUTTON):
        touch_button_var = await cg.get_variable(touch_button_id)
        cg.add(var.set_touch_button(touch_button_var))

    for key, setter in (
        (CONF_ADC_BUTTON_SENSOR, "set_adc_button_sensor"),
        (CONF_LEFT_STICK_X_SENSOR, "set_left_stick_x_sensor"),
        (CONF_LEFT_STICK_Y_SENSOR, "set_left_stick_y_sensor"),
        (CONF_RIGHT_STICK_X_SENSOR, "set_right_stick_x_sensor"),
        (CONF_RIGHT_STICK_Y_SENSOR, "set_right_stick_y_sensor"),
    ):
        if sensor_id := config.get(key):
            sensor_var = await cg.get_variable(sensor_id)
            cg.add(getattr(var, setter)(sensor_var))

    if usb_hidx_id := config.get(CONF_USB_HIDX_ID):
        cg.add_define("PAPP_LOADER_USE_USB_HIDX")
        usb_hidx_var = await cg.get_variable(usb_hidx_id)
        cg.add(var.set_usb_hidx(usb_hidx_var))

    for name, index in BUTTON_FIELDS.items():
        key = f"button_{name}"
        if sensor_id := config.get(key):
            sensor_var = await cg.get_variable(sensor_id)
            cg.add(var.set_button(index, sensor_var))


@automation.register_action(
    "papp_loader.launch_url",
    LaunchUrlAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(PappLoader),
            cv.Required("url"): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def papp_loader_launch_url_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    action = cg.new_Pvariable(action_id, template_arg, parent)
    url = await cg.templatable(config["url"], args, cg.std_string)
    cg.add(action.set_url(url))
    return action
