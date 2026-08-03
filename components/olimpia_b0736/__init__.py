"""Olimpia Splendid B0736 supervisor-port client.

Speaks Modbus ASCII (9600 7E1) to the B0736 wall thermostat over its
A'/B' supervisor port, taking the place of the B1130 WiFi kit.

The B0736 remains master of the A/B fan coil bus, so it keeps handling
the mandatory 101/102/103 watchdog cycle, ambient temperature and the
local display. This component only supervises it.
"""

import esphome.codegen as cg

CODEOWNERS = ["@nikos"]
DEPENDENCIES = ["uart"]

olimpia_b0736_ns = cg.esphome_ns.namespace("olimpia_b0736")
