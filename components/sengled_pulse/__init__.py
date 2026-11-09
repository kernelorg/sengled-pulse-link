"""Sengled Pulse Link (C01-BR30) gateway on its internal UART.

One block creates every entity; `lamp_slots` sets how many lamps are shown.
Each entity key below takes the usual entity options (name, icon, internal,
disabled_by_default, ...) to override the defaults.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    button,
    light,
    media_player,
    number,
    select,
    switch,
    text,
    text_sensor,
    uart,
)
from esphome.const import (
    CONF_DELAY,
    CONF_ID,
    CONF_MEDIA_PLAYER,
    CONF_NAME,
    CONF_OPTIONS,
    CONF_OUTPUT_ID,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

CODEOWNERS = ["@kernelorg"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["button", "light", "number", "select", "switch", "text", "text_sensor"]
MULTI_CONF = False

CONF_LAMP_SLOTS = "lamp_slots"
CONF_SLOTS = "slots"
CONF_AUTO_MUTE = "auto_mute"

ns = cg.esphome_ns.namespace("sengled_pulse")
SengledPulse = ns.class_("SengledPulse", cg.PollingComponent, uart.UARTDevice)
SengledLight = ns.class_("SengledLight", light.LightOutput)
SengledNumber = ns.class_("SengledNumber", number.Number)
SengledSwitch = ns.class_("SengledSwitch", switch.Switch)
SengledSelect = ns.class_("SengledSelect", select.Select)
SengledButton = ns.class_("SengledButton", button.Button)
SengledText = ns.class_("SengledText", text.Text, cg.Component)
Kind = ns.enum("Kind", is_class=True)

MAX_SLOTS = 8  # the gateway supports up to 8 lamps
EQ_OPTIONS = ["normal", "pop", "jazz", "classic", "rock", "movie"]
CHANNEL_OPTIONS = ["right", "left", "left+right"]
SOURCE_OPTIONS = ["3.5mm", "bluetooth"]


def _options(defaults):
    """options of a select: protocol value = position, labels may be renamed"""
    return cv.All(cv.ensure_list(cv.string_strict), cv.Length(min=len(defaults), max=len(defaults)))


def _light_schema():
    # transitions would flood the UART with brightness frames; the lamps
    # apply the level themselves, so no gamma either
    return light.light_schema(SengledLight, light.LightType.BRIGHTNESS_ONLY).extend(
        {
            cv.Optional("gamma_correct", default=1.0): cv.positive_float,
            cv.Optional("default_transition_length", default="0s"): cv.positive_time_period_milliseconds,
        }
    )


def _volume_schema(icon="mdi:volume-high"):
    return number.number_schema(SengledNumber, icon=icon, unit_of_measurement="%")


def _mute_schema():
    # no command at boot, the state is read back
    return switch.switch_schema(SengledSwitch, icon="mdi:volume-off", default_restore_mode="DISABLED")


def _select_schema(defaults, icon=cv.UNDEFINED):
    return select.select_schema(SengledSelect, icon=icon).extend(
        {cv.Optional(CONF_OPTIONS, default=defaults): _options(defaults)}
    )


def _entity(schema, name):
    """optional entity with a default name"""
    return schema, name


ENTITIES = {
    # key: (schema, default name)
    "all_lamps": _entity(_light_schema(), "All lamps"),
    "volume": _entity(_volume_schema(), "Volume"),
    "mute": _entity(_mute_schema(), "Mute"),
    "equalizer": _entity(_select_schema(EQ_OPTIONS), "Equalizer"),
    "channel": _entity(_select_schema(CHANNEL_OPTIONS, "mdi:speaker"), "Channel"),
    "gateway_mute": _entity(_mute_schema(), "Gateway mute"),
    "gateway_equalizer": _entity(_select_schema(EQ_OPTIONS), "Gateway equalizer"),
    "gateway_channel": _entity(_select_schema(CHANNEL_OPTIONS, "mdi:speaker"), "Gateway channel"),
    "audio_source": _entity(_select_schema(SOURCE_OPTIONS), "Audio source"),
    "light_memory": _entity(
        switch.switch_schema(SengledSwitch, entity_category=ENTITY_CATEGORY_CONFIG, default_restore_mode="DISABLED"),
        "Light memory",
    ),
    # lamp address for add / remove: "00020056" or the label/QR text "...;AD:00020056"
    "lamp_address": _entity(
        text.text_schema(SengledText, icon="mdi:identifier", entity_category=ENTITY_CATEGORY_CONFIG, mode="TEXT"),
        "Lamp address",
    ),
    "add_lamp": _entity(
        button.button_schema(SengledButton, icon="mdi:link-plus", entity_category=ENTITY_CATEGORY_CONFIG), "Add lamp"
    ),
    "remove_lamp": _entity(
        button.button_schema(SengledButton, icon="mdi:link-off", entity_category=ENTITY_CATEGORY_CONFIG),
        "Remove lamp",
    ),
    "refresh_nodes": _entity(
        button.button_schema(SengledButton, icon="mdi:refresh", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "Refresh node list",
    ),
    "pairing_status": _entity(
        text_sensor.text_sensor_schema(icon="mdi:link-variant", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "Pairing status",
    ),
    "gateway_type": _entity(
        text_sensor.text_sensor_schema(icon="mdi:information-outline", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "Gateway type",
    ),
    "gateway_nodes": _entity(
        text_sensor.text_sensor_schema(icon="mdi:lightbulb-group", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "Gateway nodes",
    ),
}

# per lamp slot; "{n}" = slot number from 1
SLOT_ENTITIES = {
    "light": (_light_schema(), "Lamp {n}"),
    "volume": (_volume_schema(), "Lamp {n} volume"),
    "mute": (_mute_schema(), "Lamp {n} mute"),
    "channel": (_select_schema(CHANNEL_OPTIONS, "mdi:speaker"), "Lamp {n} channel"),
    "address": (
        text_sensor.text_sensor_schema(icon="mdi:identifier", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "Lamp {n} address",
    ),
    "identify": (
        button.button_schema(SengledButton, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "Identify lamp {n}",
    ),
}

SLOT_SCHEMA = cv.Schema({cv.Optional(key): schema for key, (schema, _) in SLOT_ENTITIES.items()})


def _fill_defaults(config):
    """Default names for every entity, and one `slots` entry per lamp slot."""
    config = dict(config)
    for key, (_, name) in ENTITIES.items():
        config[key] = {CONF_NAME: name, **(config.get(key) or {})}
    count = config.get(CONF_LAMP_SLOTS, 8)
    slots = list(config.get(CONF_SLOTS) or [])
    if len(slots) > count:
        raise cv.Invalid(f"{len(slots)} entries under slots, but lamp_slots is {count}")
    slots += [{}] * (count - len(slots))
    filled = []
    for i, slot in enumerate(slots, start=1):
        slot = dict(slot or {})
        for key, (_, name) in SLOT_ENTITIES.items():
            slot[key] = {CONF_NAME: name.format(n=i), **(slot.get(key) or {})}
        filled.append(slot)
    config[CONF_SLOTS] = filled
    return config


CONFIG_SCHEMA = cv.All(
    _fill_defaults,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SengledPulse),
            # how many lamps this node shows; lamps are bound to slots at run
            # time from the gateway's node list, so this only has to be >= the
            # number of paired lamps
            cv.Optional(CONF_LAMP_SLOTS, default=8): cv.int_range(min=1, max=MAX_SLOTS),
            # per-slot overrides, e.g. names: slots: [{light: {name: Kitchen}}]
            cv.Optional(CONF_SLOTS): cv.ensure_list(SLOT_SCHEMA),
            # mute all lamps and the gateway while this media player plays
            # nothing; speakers muted by hand stay muted
            cv.Optional(CONF_AUTO_MUTE): cv.Schema(
                {
                    cv.Required(CONF_MEDIA_PLAYER): cv.use_id(media_player.MediaPlayer),
                    cv.Optional(CONF_DELAY, default="3s"): cv.positive_time_period_milliseconds,
                }
            ),
            **{cv.Optional(key): schema for key, (schema, _) in ENTITIES.items()},
        }
    )
    # how often the gateway and every lamp are read back
    .extend(cv.polling_component_schema("30s"))
    .extend(uart.UART_DEVICE_SCHEMA),
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema("sengled_pulse", baud_rate=115200)


async def _light(hub, conf, slot):
    out = cg.new_Pvariable(conf[CONF_OUTPUT_ID])
    cg.add(out.set_parent(hub))
    cg.add(out.set_slot(slot))
    await light.register_light(out, conf)
    return out


async def _control(hub, var, kind, slot=-1):
    cg.add(var.set_parent(hub))
    cg.add(var.set_kind(getattr(Kind, kind), slot))
    return var


async def _number(hub, conf, kind, slot=-1):
    var = await number.new_number(conf, min_value=0, max_value=100, step=1)
    return await _control(hub, var, kind, slot)


async def _switch(hub, conf, kind, slot=-1):
    return await _control(hub, await switch.new_switch(conf), kind, slot)


async def _select(hub, conf, kind, slot=-1):
    var = await select.new_select(conf, options=conf[CONF_OPTIONS])
    return await _control(hub, var, kind, slot)


async def _button(hub, conf, kind, slot=-1):
    return await _control(hub, await button.new_button(conf), kind, slot)


async def to_code(config):
    hub = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(hub, config)
    await uart.register_uart_device(hub, config)
    cg.add(hub.set_slot_count(config[CONF_LAMP_SLOTS]))

    cg.add(hub.set_all_lamps(await _light(hub, config["all_lamps"], -1)))
    cg.add(hub.set_volume(await _number(hub, config["volume"], "VOLUME")))
    cg.add(hub.set_mute(await _switch(hub, config["mute"], "MUTE")))
    cg.add(hub.set_equalizer(await _select(hub, config["equalizer"], "EQUALIZER")))
    cg.add(hub.set_channel(await _select(hub, config["channel"], "CHANNEL")))
    cg.add(hub.set_gateway_mute(await _switch(hub, config["gateway_mute"], "GATEWAY_MUTE")))
    cg.add(hub.set_gateway_equalizer(await _select(hub, config["gateway_equalizer"], "GATEWAY_EQUALIZER")))
    cg.add(hub.set_gateway_channel(await _select(hub, config["gateway_channel"], "GATEWAY_CHANNEL")))
    cg.add(hub.set_audio_source(await _select(hub, config["audio_source"], "AUDIO_SOURCE")))
    cg.add(hub.set_light_memory(await _switch(hub, config["light_memory"], "LIGHT_MEMORY")))

    addr = await text.new_text(config["lamp_address"], max_length=64)
    await cg.register_component(addr, config["lamp_address"])
    cg.add(hub.set_lamp_address(addr))
    await _button(hub, config["add_lamp"], "ADD_LAMP")
    await _button(hub, config["remove_lamp"], "REMOVE_LAMP")
    await _button(hub, config["refresh_nodes"], "REFRESH_NODES")
    cg.add(hub.set_pairing_status(await text_sensor.new_text_sensor(config["pairing_status"])))
    cg.add(hub.set_gateway_type(await text_sensor.new_text_sensor(config["gateway_type"])))
    cg.add(hub.set_gateway_nodes(await text_sensor.new_text_sensor(config["gateway_nodes"])))

    for i, slot in enumerate(config[CONF_SLOTS]):
        cg.add(hub.set_slot_light(i, await _light(hub, slot["light"], i)))
        cg.add(hub.set_slot_volume(i, await _number(hub, slot["volume"], "SLOT_VOLUME", i)))
        cg.add(hub.set_slot_mute(i, await _switch(hub, slot["mute"], "SLOT_MUTE", i)))
        cg.add(hub.set_slot_channel(i, await _select(hub, slot["channel"], "SLOT_CHANNEL", i)))
        cg.add(hub.set_slot_address(i, await text_sensor.new_text_sensor(slot["address"])))
        await _button(hub, slot["identify"], "SLOT_IDENTIFY", i)

    if (auto := config.get(CONF_AUTO_MUTE)) is not None:
        player = await cg.get_variable(auto[CONF_MEDIA_PLAYER])
        cg.add(hub.set_auto_mute(player, auto[CONF_DELAY]))
