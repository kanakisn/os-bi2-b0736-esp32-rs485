import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, climate, sensor, uart
from esphome.const import (
    CONF_ADDRESS,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)

from . import olimpia_b0736_ns

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "binary_sensor"]

CONF_ROOM_TEMPERATURE = "room_temperature"
CONF_FAULT = "fault"
CONF_CHILLER = "chiller"
CONF_BOILER = "boiler"
CONF_ONLINE = "online"
CONF_REPLY_TIMEOUT = "reply_timeout"
CONF_MAX_RETRIES = "max_retries"
CONF_EXTERNAL_TEMPERATURE = "external_temperature"
CONF_EXTERNAL_TEMPERATURE_TIMEOUT = "external_temperature_timeout"

OlimpiaB0736 = olimpia_b0736_ns.class_(
    "OlimpiaB0736", climate.Climate, uart.UARTDevice, cg.PollingComponent
)

CONFIG_SCHEMA = (
    climate.climate_schema(OlimpiaB0736)
    .extend(
        {
            cv.Optional(CONF_ADDRESS, default=1): cv.int_range(min=1, max=247),
            cv.Optional(CONF_REPLY_TIMEOUT, default="500ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MAX_RETRIES, default=2): cv.int_range(min=0, max=5),
            # An existing sensor (e.g. a BLE room probe) to report as the
            # climate entity's current temperature in place of reg 0.
            cv.Optional(CONF_EXTERNAL_TEMPERATURE): cv.use_id(sensor.Sensor),
            cv.Optional(
                CONF_EXTERNAL_TEMPERATURE_TIMEOUT, default="15min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ROOM_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_FAULT): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_CHILLER): binary_sensor.binary_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BOILER): binary_sensor.binary_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
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

    if CONF_EXTERNAL_TEMPERATURE in config:
        ext = await cg.get_variable(config[CONF_EXTERNAL_TEMPERATURE])
        cg.add(var.set_external_temperature_sensor(ext))
        cg.add(
            var.set_external_temperature_timeout(
                config[CONF_EXTERNAL_TEMPERATURE_TIMEOUT]
            )
        )

    if CONF_ROOM_TEMPERATURE in config:
        sens = await sensor.new_sensor(config[CONF_ROOM_TEMPERATURE])
        cg.add(var.set_room_temperature_sensor(sens))

    for key, setter in (
        (CONF_FAULT, var.set_fault_binary_sensor),
        (CONF_CHILLER, var.set_chiller_binary_sensor),
        (CONF_BOILER, var.set_boiler_binary_sensor),
        (CONF_ONLINE, var.set_online_binary_sensor),
    ):
        if key in config:
            bs = await binary_sensor.new_binary_sensor(config[key])
            cg.add(setter(bs))
