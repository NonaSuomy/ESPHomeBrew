"""Drop conflicting zero entity-count defines before defines.h is written.

Some ESPHome setups (seen with ESPHome 2026.6.0-dev plus locally forked
components) emit an entity count twice, e.g.

    #define ESPHOME_ENTITY_BUTTON_COUNT 0
    #define ESPHOME_ENTITY_BUTTON_COUNT 10

which fails every C++ file with "redefined [-Werror]". Core ESPHome writes each
count once, from the real number of entities, at CoroPriority.FINAL. This
component runs after that and removes a zero-valued ESPHOME_ENTITY_*_COUNT
define when a non-zero one with the same name exists. Nothing else changes.

    external_components:
      - source: github://NonaSuomy/papp-conversions@main
        components: [define_fixup]
    define_fixup:
"""

import logging
import re

import esphome.config_validation as cv
from esphome.core import CORE
from esphome.coroutine import coroutine_with_priority

_LOGGER = logging.getLogger(__name__)
_COUNT_DEFINE = re.compile(r"^ESPHOME_ENTITY_[A-Z0-9_]+_COUNT$")

CONFIG_SCHEMA = cv.Schema({})


def _as_int(value):
    try:
        return int(str(value))
    except (TypeError, ValueError):
        return None


# Lower than CoroPriority.FINAL (-1000), where core adds the real counts, so this
# sees every define before the writer turns CORE.defines into defines.h.
@coroutine_with_priority(-10000.0)
async def _drop_conflicting_zero_counts():
    by_name = {}
    for define in CORE.defines:
        if _COUNT_DEFINE.match(str(define.name)):
            by_name.setdefault(define.name, []).append(define)
    for name, defines in by_name.items():
        if len(defines) < 2:
            continue
        values = [_as_int(d.value) for d in defines]
        if any(v not in (None, 0) for v in values):
            for define, value in zip(defines, values):
                if value == 0:
                    CORE.defines.discard(define)
                    _LOGGER.info("define_fixup: dropped conflicting '#define %s 0'", name)


async def to_code(config):
    CORE.add_job(_drop_conflicting_zero_counts)
