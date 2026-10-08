import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, climate, sensor, uart
from esphome.const import (
    CONF_ADDRESS,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)

from . import olimpia_fancoil_ns

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor"]

CONF_ROOM_TEMPERATURE = "room_temperature"
CONF_WATER_TEMPERATURE = "water_temperature"
CONF_EV1 = "ev1"
CONF_EV2 = "ev2"
CONF_ONLINE = "online"
CONF_FAN_RUNNING = "fan_running"
CONF_REPLY_TIMEOUT = "reply_timeout"
CONF_MAX_RETRIES = "max_retries"

OlimpiaFancoil = olimpia_fancoil_ns.class_(
    "OlimpiaFancoil", climate.Climate, uart.UARTDevice, cg.PollingComponent
)

CONFIG_SCHEMA = (
    climate.climate_schema(OlimpiaFancoil)
    .extend(
        {
            cv.Optional(CONF_ADDRESS, default=1): cv.int_range(min=1, max=247),
            cv.Optional(
                CONF_REPLY_TIMEOUT, default="500ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MAX_RETRIES, default=2): cv.int_range(min=0, max=5),
            cv.Optional(CONF_ROOM_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_WATER_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_EV1): binary_sensor.binary_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_EV2): binary_sensor.binary_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_FAN_RUNNING): binary_sensor.binary_sensor_schema(
                icon="mdi:fan",
            ),
            cv.Optional(CONF_ONLINE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(cv.polling_component_schema("5s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_timeout(config[CONF_REPLY_TIMEOUT]))
    cg.add(var.set_max_retries(config[CONF_MAX_RETRIES]))

    for key, setter in (
        (CONF_ROOM_TEMPERATURE, var.set_room_temperature_sensor),
        (CONF_WATER_TEMPERATURE, var.set_water_temperature_sensor),
    ):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(setter(sens))

    for key, setter in (
        (CONF_EV1, var.set_ev1_binary_sensor),
        (CONF_EV2, var.set_ev2_binary_sensor),
        (CONF_FAN_RUNNING, var.set_fan_running_binary_sensor),
        (CONF_ONLINE, var.set_online_binary_sensor),
    ):
        if key in config:
            bs = await binary_sensor.new_binary_sensor(config[key])
            cg.add(setter(bs))
