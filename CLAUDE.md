# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An ESPHome project, not a general application. An ESP32 board replaces the Olimpia
Splendid **B1130 WiFi kit** and supervises a **B0736** wall thermostat over RS485
using **Modbus ASCII**. Two carrier boards are supported — a LilyGO T-CAN485
(`esp32dev`) and a Waveshare ESP32-S3-RS485-CAN (`esp32-s3-devkitc-1`), both on
ESP-IDF. The `olimpia_b0736` component is board-agnostic; all board differences
live in YAML. The B0736
stays master of the internal A/B fan coil bus (watchdog cycle, ambient sensing,
display); we only talk to its A'/B' supervisor port, where it acts as a Modbus slave.

The register map was reverse-engineered from captured B1130 ↔ B0736 traffic
(`capture.log`), not from a vendor document. Treat any register outside the known
set as unknown — that is what the exploration services exist for.

## Commands

```bash
esphome run olimpia.yaml        # compile + upload (USB first time, OTA afterwards)
esphome compile olimpia.yaml    # build only — the fastest way to check a C++ change
esphome config olimpia.yaml     # validate + resolve YAML only, seconds instead of minutes
esphome logs olimpia.yaml       # stream logs
esphome clean olimpia.yaml      # wipe build cache (needed after some component edits)
esphome run sniffer.yaml        # listen-only build that hex-dumps every RS485 frame
```

Swap in `olimpia-s3.yaml` / `sniffer-s3.yaml` for the Waveshare board. A component
change must be compiled against **both** boards — same command, other file.

There is no test suite and no linter. `esphome config` (schema only) and
`esphome compile` (schema + C++) are the whole verification loop. Hardware is
required to actually observe behaviour.

`secrets.yaml` (git-ignored) must exist with `wifi_ssid` / `wifi_password`.

## Layout

- `olimpia.yaml` / `olimpia-s3.yaml` — the real configs, one per board: UART, the
  climate platform, and the HA-callable exploration services.
- `sniffer.yaml` / `sniffer-s3.yaml` — listen-only debug builds (`dummy_receiver: true`).
- `components/olimpia_b0736/` — the external component. `climate.py` is the config
  schema + codegen; `olimpia_b0736.{h,cpp}` is the protocol client and climate entity.

The four YAML files duplicate a lot (wifi, api services, climate block). They are
not yet factored into a shared `packages:` include, so a change to the exploration
services or the climate block has to be applied to both `olimpia*.yaml`.

## Component architecture

`OlimpiaB0736` is simultaneously a `climate::Climate`, a `uart::UARTDevice` and a
`PollingComponent`. The whole protocol is a **single-outstanding-transaction state
machine** — understand this before changing anything in `loop()`:

- `update()` (every `update_interval`) pushes a batch of FC03 reads onto `queue_`.
- `loop()` drains RX byte-by-byte, framing on `:` … `\r\n`, then sends the next
  queued transaction only when `state_ == STATE_IDLE`. Never more than one frame in
  flight; replies are matched to `current_` positionally, not by content.
- Timeout → retry up to `max_retries_` → give up and `mark_offline_()`.

Session handling matters: after 3 consecutive dead transactions `session_ok_` drops,
and `update()` then stops queueing the full poll batch and sends only a FC03 read of
`REG_IDENT` (199) until it answers. This is deliberate — a full dead batch costs
`timeout × retries × 6` and would outrun `update_interval` and build a backlog.

`REG_FAULT` (105) is the **last register of the poll batch**, and its handler is what
calls `publish_all_()`. Adding registers after it, or reordering the batch, silently
breaks state publishing.

Writes to the control register (201) are always **read-modify-write** via
`ctrl_set_mask_` / `ctrl_clear_mask_`: only the fan bits (b0–b2) and standby (b7) are
understood, so everything else must be preserved. If `control_valid_` is false the
write is deferred behind a read (`control_pending_`).

### Register map (B0736 as slave)

| Reg | Access | Meaning |
| --- | --- | --- |
| 0 | R | Room temperature, ×0.1 °C |
| 9 | R | Status bits: b14 EV1, b13 boiler, b12 chiller, b11 EV2 |
| 105 | R | Fault, 0 = healthy |
| 199 | R | Identity constant (1200) — used as the session handshake |
| 201 | R/W | Control: b0–b2 fan (0 auto, 1 min, 2 night, 3 max), b7 standby |
| 231 | R/W | Setpoint, `(°C − 15.5) × 2`, clamped to raw 1–25 (16–28 °C) |
| 233 | R/W | Mode: 3 = heating, 5 = cooling |

### Exploration services

`olimpia.yaml` exposes `read_register`, `write_register`, `scan_registers` and
`set_control_bit` as HA actions that call straight into the component. They are how
unknown registers get mapped: results are logged at INFO with a full bit breakdown
(registers requested by hand are tracked in `manual_reads_`). `write_register`
refuses register 200 (slave address, in EEPROM). Registers ≥200 generally live in
EEPROM with limited write endurance — do not loop writes over them.

## Hardware constraints that bite

**The two boards share no pins.** `GPIO16`/`GPIO17`/`GPIO19` mean different things
on each, so a pin block copied across configs is actively destructive:

| | T-CAN485 | Waveshare ESP32-S3 |
| --- | --- | --- |
| UART TX | `GPIO22` | `GPIO17` |
| UART RX | `GPIO21` | `GPIO18` |
| RS485 direction | none — 3 always-on enables | `GPIO21`, `flow_control_pin` |
| `GPIO16` | EN 5V (drive HIGH) | CAN RX |
| `GPIO17` | RS485 EN (drive HIGH) | RS485 TX |
| `GPIO19` | RS485 SE (drive HIGH) | USB D− |

- **T-CAN485**: the three enables must be held HIGH via `restore_mode: ALWAYS_ON`
  GPIO switches, or nothing moves on the wire. The transceiver stays enabled.
- **Waveshare**: an SP3485EN behind a non-inverting digital isolator, `RE`/`DE`
  tied. `flow_control_pin: GPIO21` on the `uart:` block is the whole story — it
  puts the IDF driver into `UART_MODE_RS485_HALF_DUPLEX` and toggles direction in
  hardware. Do **not** add a GPIO switch for it, and do not add DE handling to
  `send_frame_()`.
- **Waveshare logging**: the USB-C port goes straight to the native USB pins with
  no bridge chip, so `logger: hardware_uart: USB_SERIAL_JTAG` is mandatory — on
  UART0 the logs go nowhere.
- UART is **7E1 at 9600, non-inverted** on both. Inverting makes the idle line read
  as a stream of `0x00`. `dump_config()` asserts this with `check_uart_settings`.
- `use_address` must be where the board *currently* is, not where `manual_ip` will
  put it, or the first OTA after a network change cannot find it.
- `olimpia.yaml` and `sniffer.yaml` share the same node name (`olimpia-sniffer` —
  a copy-paste leftover in `olimpia.yaml`) and the same static IP `192.168.0.110`.
  The S3 pair uses `olimpia-s3` / `olimpia-s3-sniffer` on `192.168.0.121`.

## ESPHome API drift

`traits()` is version-guarded: `set_supports_*` was replaced by
`add_feature_flags()` in 2025.11 and the shims were removed in 2026.5. Captures were
taken on 2026.7.3. Keep new trait code behind the same `ESPHOME_VERSION_CODE` check
rather than picking one side.

## Bringing up a new B0736 — hard-won lessons

When a fresh install would not talk (Waveshare S3 on a second B0736), the fix was
**not** any of the usual RS485 suspects. Recorded here so they are not re-chased:

- **The session only opens on the IDENT (reg 199) read.** `session_ok_` is set only
  in the `REG_IDENT` handler; `update()` sends that knock while the link is down. A
  diagnostic "address sweep" once *replaced* the IDENT knock with a reg-105 probe, so
  the board never sent the one request the B0736 answers and never connected. If a
  unit polls forever with no reply, confirm `update()` is still sending `REG_IDENT`.
- **Isolated 2-wire RS485 was a red herring.** The Waveshare's RS485 is isolated with
  only A+/B- brought out (no signal ground), and it still receives fine — proven by
  sniffing the live internal A/B bus. Do not waste time on grounding/common-mode.
- **The internal A/B fan-coil bus is a different, binary protocol** — not the ASCII
  supervisor framing. Sniffing it shows structured non-`:` frames; that is expected
  and only proves the receiver works, nothing more.
- **A silent listen on A'/B' is normal.** The supervisor port is slave-only, so a
  listen-only sniffer there sees nothing until a master polls. Silence there is not a
  fault. The B0736's supervisor ("WiFi") icon lighting up confirms you are on the
  right A'/B' terminals.
- **Debug order that works:** sniff the live internal A/B bus to prove RX; if RX is
  fine, the problem is the request or the far end, not the transceiver.
