# External BLE room probe as the reported temperature

**Status:** shipped and confirmed working end to end 2026-09-16; the
hardware-regulation half is not started.
**Node:** guest only (`olimpia-s3-guest.yaml`).

Confirmed live: 22 probe updates in 75 s, climate entity reporting the probe
(28.19 degC) instead of reg 0 (25.5 degC), RS485 poll clean - zero timeouts,
retries or offline events with BLE running.

## What it does

A Xiaomi LYWSD03MMC at `A4:C1:38:B1:C0:3A` is read over BLE by the ESP32-S3 and
published as its own entities: **External probe temperature / humidity / battery /
battery voltage**.

**The climate entity reports the B0736's own reg 0, not the probe.** Decided
2026-09-16: what Home Assistant shows for the fan coil should be what the hardware is
actually regulating on, otherwise the entity claims a temperature the unit is not
controlling to. The two are published side by side so the error between them is
visible - which is the point, given that error reaches 2.5 degC after an hour of
cooling.

The component's `external_temperature:` option still exists and still works (it
overrides `current_temperature`, with a staleness fallback to reg 0). It is
deliberately **left unwired** in `olimpia-s3-guest.yaml`.

## Where it lives

- `components/olimpia_b0736/climate.py` — `CONF_EXTERNAL_TEMPERATURE` /
  `CONF_EXTERNAL_TEMPERATURE_TIMEOUT` in `CONFIG_SCHEMA`, wired in `to_code()`.
- `components/olimpia_b0736/olimpia_b0736.h` — `set_external_temperature_sensor()`,
  `set_external_temperature_timeout()`, and the `external_temperature_*` members
  under `#ifdef USE_SENSOR`.
- `components/olimpia_b0736/olimpia_b0736.cpp` — `OlimpiaB0736::setup()` registers
  the state callback; `OlimpiaB0736::publish_all_()` chooses the source;
  `on_register_()` `case REG_ROOM_TEMP` now only caches `b0736_temperature_`.
- `olimpia-s3-guest.yaml` — `esp32_ble_tracker:`, the `pvvx_mithermometer` sensor,
  and `external_temperature:` on the climate block.

## Two design points worth keeping

**The source is chosen in `publish_all_()`, not in the `REG_ROOM_TEMP` handler.**
One place decides. Reg 0 is always cached in `b0736_temperature_` and always
published on the `room_temperature` sensor, so internal and external can be
compared — that delta is exactly what a reg 117 trim would have to cancel.

**The external value is latched with its arrival time.** A BLE probe that stops
advertising must not freeze the reported temperature forever. After
`external_temperature_timeout` (default 15 min) the component falls back to reg 0
and logs a warning; it logs again on recovery. A flat battery degrades instead of
silently lying.

## Radio sharing

BLE and WiFi share one radio on the ESP32-S3, and this node must still answer the
B0736 inside `reply_timeout` (500 ms). Scan parameters are deliberately left at the
ESPHome default (320 ms interval / 30 ms window, ~9% duty cycle). Measured over 100 s
with the probe at -75 dBm: no timeouts, no retries, no offline events. If the
"B0736 link" binary sensor starts flapping after a BLE change, widen the scan
interval before suspecting anything else.

## Not done yet

Making the *hardware* follow the external probe. Two candidates:

1. **Trim reg 117** — the per-unit signed temperature offset. Would bias the B0736's
   own reading to match the probe. Unconfirmed as writable, and it is almost
   certainly EEPROM, so writes must be rare and rate-limited (deadband, not every
   poll).
2. **Broadcast FC06 to fan-coil reg 103** — the ambient-temperature write the B1130
   performs at address 0 in `capture.log`, which the community map documents as
   "must write periodically for auto mode".

## Sensor firmware state

Hardware **B1.4**, flashed to **pvvx v5.9**. Because it is B1.4 and not B1.5/B1.6,
the two-stage `Original_OTA_` path in pvvx issue #602 never applied - it flashed
directly. Login did need the MiHome credentials (Device known id / Mi Token / Mi Bind
Key) from the Xiaomi cloud token extractor; `Do Activation` alone was not offered on
`2.1.1_0159`.

**The advertised name is the firmware tell:** `ATC_B1C03A` = pvvx (ATC_ plus the last
three bytes of the MAC), `LYWSD03MMC` = stock. No connection needed to check.

## Discovery scaffolding - removed, and how to get it back

The scan scaffolding has been taken out: logging is back to `INFO` with no `logs:`
block, the `on_ble_advertise:` trigger is gone, and scanning is `active: false` so the
node no longer transmits on BLE at all.

To rediscover a sensor later (new probe, unknown MAC): set `active: true` and add back
an `on_ble_advertise:` trigger that logs. The tracker logs nothing on its own no
matter the log level - see `.knowledge/gotchas.md`.
