"""Test-only stub: emits the stray zero entity count seen on some builds, so CI
can prove define_fixup removes it. Never use this in a device config."""
import esphome.codegen as cg
import esphome.config_validation as cv

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    cg.add_define("ESPHOME_ENTITY_BUTTON_COUNT", 0)
