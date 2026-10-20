# Basement wall panel: ESP32-P4-86-Panel as an HA remote for every fan coil

**Status:** flashed and booting 2026-10-20 (WiFi up, touch responding). HA adoption,
the fan-coil rows and the relay click are **not yet confirmed**.
**Layout (2026-10-20):** header (title, clock) over a two-tile `tileview` - swipe
between **lights** (two tall tiles side by side: HA `light.basement_basement_lights_lights`
and relay 1 "Basement Hall lights") and **fan coils** (six rows: living room, master, guest, first floor, loft,
basement). Tapping a row opens `detail_page`. After 10 min idle it returns to the lights
tile and goes dark. The living room coil is `climate.olimpia_sniffer_fan_coils` - the
T-CAN485 node from `olimpia.yaml`, still named `olimpia-sniffer`.
**Config:** `basement-panel.yaml` + `panel_ui.h` + `panel/coil.yaml` + `panel/coil_row.yaml`.
Node `basement-panel`, "Basement Panel", 192.168.0.128.
**Board:** Waveshare ESP32-P4-86-Panel-ETH-2RO (720x720 MIPI-DSI, GT911, 2 relays).

## The design decision: through Home Assistant, not Modbus

This is the only node in the repo that talks to **no** fan coil. It mirrors each coil's
climate entity from HA (`homeassistant` text sensors and sensors, `internal: true`) and
sends commands back as `climate.set_hvac_mode` / `set_fan_mode` / `set_temperature`
actions.

Why: every coil already has exactly one Modbus master (its own node). A second master
on any of those buses would collide with the single-outstanding-transaction polling in
`olimpia_b0736` / `olimpia_fancoil`. Going through HA also gives one UI for both coil
types without caring that their registers differ.

Consequences:

- HA must **allow the device to perform actions** (ESPHome integration -> device ->
  Configure). Without it every tap is dropped silently.
- `panel_ui.h` hardcodes the five **entity ids**, which are ESPHome's defaults derived
  from `friendly_name` + climate `name` (e.g. "Master Bedroom FCU" + "Fan coils" ->
  `climate.master_bedroom_fcu_fan_coils`). Renaming an entity in HA breaks its row
  (shows "Offline").
- Fan-only is offered per coil (`Coil::fan_only` in `panel_ui.h`). `olimpia_b0736`
  doesn't support it, `olimpia_fancoil` does.
- Setpoint taps are debounced 2 s (`commit_setpoint`), because the B0736 keeps reg 231
  in EEPROM.

Structure: `panel/coil.yaml` is included five times under `packages:` with
`vars: {i, entity}`. It creates `c${i}_*` sensors and a `refresh_c${i}` script that
repaints row `i`. `panel/coil_row.yaml` is `!include`d inside the LVGL widget list.
The single detail page serves whichever coil is selected (`sel`) through
`panel::coils()`, which `on_boot` (priority 600) fills with the sensor pointers.

## Board facts and the traps hit bringing it up

| | |
| --- | --- |
| Flash port | the **"USB to UART"** Type-C (WCH CH343, `1a86:55d3`). The **OTG** Type-C shows nothing on the Mac at all - that was the first "not detected". |
| USB logs | silent on the UART port - the logger defaults to the P4's native USB. Use `esphome logs --device 192.168.0.128`. |
| Silicon | **ESP32-P4 rev v1.3** -> `engineering_sample: true`, max 360 MHz. Without it ESPHome assumes rev 3 silicon. Check with `esptool flash-id`. |
| Flash | chip has 32 MB, declared **16 MB**: OTA above 16 MB needs `enable_idf_experimental_features` (esphome/esphome#11775). |
| LDOs | `esp_ldo` is **required** by `mipi_dsi`: channel 3 = 2.5 V (DSI PHY). Channel 4 = 3.3 V for the GPIO46-48 domain (relay 2, RS485), but 3.3 V is not a valid LDO voltage (max 2.7) - it is `voltage: passthrough`. |
| WiFi | ESP32-C6 over SDIO, `esp32_hosted`: reset 54, cmd 19, clk 18, d0-d3 14-17, `active_high: true` (from #11775). Worked first time. |
| Display | `mipi_dsi` model `WAVESHARE-P4-86-PANEL` (built into ESPHome 2026.7). |
| Touch | GT911, I2C SDA 7 / SCL 8, reset GPIO23. On-boot 300 ms delay + `setup_priority: -200` per esphome/esphome#15019 (settle-after-reset bug). |
| Backlight | PWM GPIO26 (ledc) is **active low** -> `inverted: true`. Boost enable GPIO33 held HIGH (correct - it was briefly suspected and removed, wrongly). |
| Relays | GPIO32 (relay 1 -> `light` "Basement Hall lights"), GPIO46 (relay 2 -> `switch` "Relay 2"). |
| BOOT button | **GPIO35**, active low (`boot_button`, toggles the screen via `toggle_screen`). Espressif's P4 convention - the wiki doesn't name it, and lists GPIO35 as Ethernet TXD1 (unused, we're on WiFi). **Confirmed 2026-10-20**: presses logged and the screen toggled. Read only at runtime, so holding it at power-on still enters download mode. |
| Power | DC 6-30 V. |

The board also has RS485 (RX GPIO48, TX GPIO47), so it could drive a fan coil directly
like the loft screen does. It deliberately doesn't - see the design decision above.

Lambda trap, again: a C++ ternary with `" : "` in a one-line `!lambda` is parsed as a
YAML mapping ("mapping values are not allowed here"). Use a `|-` block.

## The black screen (2026-10-20)

After the first flash the screen was black while logs said `Backlight >> ON, 100%`.
Clues, in the order they arrived:

- Pressing the side **RESET** button lit the screen for an instant, then black again.
  During reset every pin floats, so the backlight lit by itself - the firmware was
  actively switching it *off*.
- 50% brightness (sent over the API) lit the screen; a touch, which sets 100%, blacked
  it. So the GPIO26 PWM is **active low**. Fix: `inverted: true` on the ledc output.
- A 10% test looked like it failed only because a touch reset it to 100% straight away.

**Red herring that cost a build:** GPIO33 (boost enable, held HIGH) was removed on
suspicion. The board then showed `OTA rollback detected ... reset before the boot was
marked successful` and was running the *old* build again. That wasn't a crash: the
side RESET was pressed twice inside the boot window, which ESPHome's safe mode treats
as a boot loop and rolls back. **After pressing RESET during testing, check the
`compiled on` time in the log before believing which build you are testing.**

## Colours

Mode accents are shared with the loft screen: cool `0x2F9BFF`, heat `0xFF7043`, fan
`0x26C6A6`. **Off is red `0xE53935`** (the Off tile when active, and the row strip),
because a switched-off coil must stand out. **Offline is neutral grey `0x6B7480`** - it
used to be red, which made "off" and "offline" look alike. Defined as
`panel::COLOR_OFF` / `COLOR_OFFLINE` in `panel_ui.h`.
