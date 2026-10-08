# Loft: a touchscreen replacing the B0736 outright

**Status:** running on the loft coil since 2026-10-08.
**Config:** `olimpia-lcd-loft.yaml`, node `olimpia-lcd-loft`, "Loft FCU", 192.168.0.127.
**Board:** Waveshare ESP32-S3-Touch-LCD-4.3B (800x480 RGB, GT911 touch, CH422G expander).

The B0736 is removed entirely. The board talks to the fan coil directly with
`olimpia_fancoil` (see `b1130-direct-fancoil.md`), and an LVGL screen is a local front
end onto the same climate entity Home Assistant sees.

## Settled: a coil that had a B0736 speaks plain Modbus ASCII

**Confirmed 2026-10-08.** With the B0736 removed, the loft coil answers at address 1,
9600 7E1, with the same register map and encodings as the basement coil. No coil setting
had to change. First readings: room 24.1, water 26.0 (so reg 1 really is water here),
setpoint 22.0, mode 5 (cool), control 0x0080 (standby), status 0.

**The first hookup was completely silent because A and B were swapped.** The UART debug
dump showed valid requests going out and *zero* bytes coming back - not garbage, nothing.
Swapping the two wires fixed it immediately. Total silence with known-good requests means
polarity first; garbled bytes would point at baud/parity instead.

What follows is the original risk note, kept for context:

Every coil we've driven directly so far had never been behind a B0736. A B0736 drives
its coils over the internal A/B bus with a different, binary protocol. Whether this coil
answers Modbus ASCII at address 1 once the B0736 is gone is **unproven**. The first test
is "Fan coil link" coming on and `read_register(200)` returning 1. If the coil stays
silent, look for a wall-thermostat vs Modbus/BMS setting on the coil before suspecting
the wiring.

## Board facts (from Waveshare's 4.3B pin table, 2026-10-08)

| | |
| --- | --- |
| RS485 | RX `GPIO43`, TX `GPIO44`, **automatic direction** - no `flow_control_pin` |
| CAN | TX `GPIO15`, RX `GPIO16` |
| I2C (touch, CH422G) | SDA `GPIO8`, SCL `GPIO9`; touch INT `GPIO4`, touch RST = EXIO1 |
| Backlight | CH422G EXIO2 - on/off only, no PWM |
| USB-C | native USB (GPIO19/20) -> `logger: hardware_uart: USB_SERIAL_JTAG` |
| Power | 7-36 V DC terminal block, or 5 V USB-C |
| PSRAM | 8 MB octal, **required** for the frame buffer |
| Temperature sensor | none |

ESPHome's `mipi_rgb` preset `ESP32-S3-TOUCH-LCD-4.3` (made for the non-B board) has
RGB/DE/sync/PCLK pins identical to the 4.3B table, so it is used as-is.

GPIO17/18/21 are **LCD data lines** on this board - the RS485 pins of the
ESP32-S3-RS485-CAN. A `uart:` block copied from an `olimpia-s3*` config would fight the
display.

## Design choices in the config

- **Backlight shares EXIO2 with the display's `enable_pin`.** Both must declare
  `allow_other_uses: true`, which is why the config restates `enable_pin:` from the
  preset - the preset alone fails validation ("incorrectly sets allow_other_uses").
- **`resume_on_input: false`** on `lvgl:`. The touchscreen `on_release` wakes the
  screen instead; with LVGL's default, the waking tap would also press the button under
  the finger.
- **Setpoint taps are debounced 2 s** (`nudge_setpoint` -> `commit_setpoint`,
  `mode: restart`). Reg 231 is in the >=200 EEPROM block, so five taps must produce one
  write.
- **The screen holds no state of its own.** `on_state` on the climate entity runs
  `refresh_ui`. A mode/fan button only shows as checked once the coil has confirmed it.
  The one exception is a setpoint edit in progress (`editing_setpoint`), which isn't
  overwritten until it has been committed.

## Next

- Room temperature is the coil's own reg 0. As the coil's master we can now try
  writing an ambient to regs 101-103, the triplet the B1130 broadcast. That never
  worked from behind a B0736 (`.knowledge/decisions/2026-09-16-reg103-broadcast-does-not-steer.md`)
  but has never been tried directly against a coil. If it works, a BLE probe could
  drive regulation here.

## Screen layout (2026-10-08)

A shared header (room name, Athens clock from SNTP, status chip) sits above an LVGL
`tileview` with two tiles, swiped horizontally:

1. **Control** - setpoint dial (draggable `arc` plus -/+), mode tiles, fan tiles.
   Dragging the arc does not swipe the page: LVGL 9 clears `SCROLL_CHAIN` on arcs.
2. **Status & history** - status table, a 6 h room/setpoint chart, an activity log.

The chart and the log live in `loft_ui.h` (included via `esphome: includes:`). It's
display bookkeeping only and is lost on reboot; HA keeps the real history. ESPHome
2026.7 has no LVGL chart widget, so the chart is two `line` widgets whose points are set
from C++ (`lv_line_set_points`, `lv_value_precise_t` is an integer here). Log entries
come from diffing the climate state in `refresh_ui`. A change seen within 15 s of a tap
on the screen is tagged "(screen)"; anything else came from HA or the coil.

Lambda traps hit while building it:

- `id(x)` in a lambda is a **pointer**. `id(x).foo` works only because ESPHome rewrites
  it to `x->foo`. `auto &c = id(fancoil)` binds a reference to the pointer and every
  `c.foo` then fails; use `auto *c = id(fancoil)`.
- LVGL **label** ids are raw `lv_obj_t *` (pass `id(lbl)` to `lv_label_set_text`), but
  **line** ids are `LvLineType *` and need `id(line)->obj`. `lv_obj_t` is opaque in
  LVGL 9, so getting it wrong is an "incomplete type" error.
- `id: clock` is reserved by ESPHome. The time source is `athens_time`.
- `layout:` is not allowed in `style_definitions`, so the tile layout is a YAML anchor
  (`&tile_layout`) on each tile button.

The "on/off" slide switch on the 4.3B is the **battery** switch. It connects a Li cell,
it isn't a power switch, and it does nothing on USB or DC power.

## Loft coil configuration snapshot (2026-10-08, after reg 224 -> 0x80)

For diffing against the basement coil, whose fan *is* blocked by warm water in cooling
(see gotchas). Read with FC03, regs 200-239. Anything not listed read 0.

| Reg | Value | Community map |
| --- | --- | --- |
| 200 | 1 | address |
| 202/203 | 150/300 | setpoint min/max |
| 204/205 | 10/10 | Td hysteresis off/on |
| 206/207 | 20/20 | Td of AUTO-MAX, cooling/heating |
| 210/211 | 680/680 | fan rpm AUTO Td=0 / night / min |
| 212/213 | 950/950 | fan rpm AUTO Td=max |
| 215 | 680 | fan rpm night? |
| 216 | 1400 | fan rpm max? |
| 218 | 300 | min water temp while heating (30.0) |
| 219 | 200 | max water temp while cooling (20.0) |
| 224 | 0x80 | status bits (bit 2 remote - was 0x84) |
| 235-238 | 50 each | "temperature offset?" |

**Observed the same day, open question:** in Cool with setpoint 20.0, room 25.2 and
water 26.0 (above reg 219's 20.0), this coil ran its fan at 956 rpm (= reg 212, full
AUTO speed) with both valves open. Reg 9 = `0xC800` (EV1, EV2, unknown bit 15; chiller
demand bit 12 **off**). The basement coil holds its fan at 0 in the same situation.
Which setting gates the fan on water temperature is unknown - no register in the
community map describes it. A 200-239 diff against the basement coil is the next step.
