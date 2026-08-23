# Olimpia Splendid Bi2 — ESP32 / RS485 supervisor

ESPHome integration that supervises an **Olimpia Splendid Bi2** fan coil
through its **B0736** wall thermostat, over RS485 using Modbus ASCII. An
ESP32 takes the place of the original **B1130 WiFi kit**, exposing the fan
coil to Home Assistant as a native `climate` entity plus diagnostic sensors.

The B0736 stays master of the internal A/B fan coil bus — it keeps handling
the mandatory watchdog cycle, ambient temperature reading and the local
display. This project only *supervises* it through the B0736's secondary
A'/B' supervisor port, exactly as the B1130 kit would.

---

## Devices

| Device | Role |
| --- | --- |
| **Olimpia Splendid Bi2** fan coil unit(s) | The heating/cooling terminal units. Driven by the B0736 over its internal A/B bus. |
| **Olimpia Splendid B0736** wall thermostat | Master of the fan coil bus and the device we talk to. Acts as a Modbus **slave** on its A'/B' supervisor port. |
| **LilyGO T-CAN485** (ESP32) | Our controller. Replaces the B1130 WiFi kit. Runs ESPHome and speaks Modbus ASCII to the B0736 over RS485. |
| **Waveshare ESP32-S3-RS485-CAN** | Alternative controller, same role. Isolated RS485, ESP32-S3-WROOM-1 (16 MB flash / 8 MB PSRAM). |

Either controller works — pick one and flash the matching config. The T-CAN485
is an `esp32dev` board; the Waveshare is `esp32-s3-devkitc-1`. Both are built
with the **ESP-IDF** framework and both run the same `olimpia_b0736`
component, which is board-agnostic.

---

## Connections

### RS485 bus

The T-CAN485's RS485 A/B pair connects to the B0736 **A'/B' supervisor
port** (the same terminals the B1130 kit used — *not* the internal A/B fan
coil bus).

### UART / serial line

Modbus ASCII per the RDE109 supervisor spec — **9600 baud, 7 data bits,
even parity, 1 stop bit (7E1)**, non-inverted. Same on both boards; only the
pins and the transceiver control differ.

> **The two boards share no pins.** GPIO16/17/19 mean completely different
> things on each. Never copy a pin block from one config to the other.

#### LilyGO T-CAN485 (`olimpia.yaml`, `sniffer.yaml`)

| Signal | GPIO |
| --- | --- |
| UART RX | `GPIO21` |
| UART TX | `GPIO22` |
| EN 5V (5 V boost) | `GPIO16` |
| RS485 EN (driver enable) | `GPIO17` |
| RS485 SE (slew/enable) | `GPIO19` |

The three enable lines are held **HIGH** for the whole session, as always-on
GPIO switches in ESPHome. The transceiver is left permanently enabled.

#### Waveshare ESP32-S3-RS485-CAN (`olimpia-s3.yaml`, `sniffer-s3.yaml`)

| Signal | GPIO |
| --- | --- |
| UART TX (`TXD1`) | `GPIO17` |
| UART RX (`RXD1`) | `GPIO18` |
| RS485 direction (`RS485_EN`) | `GPIO21` |

No always-on switches here. The RS485 side is an **SP3485EN** behind a
non-inverting six-channel digital isolator, with `RE` and `DE` tied together —
so `GPIO21` is a genuine direction pin: HIGH drives, LOW listens. It is
declared as the UART's `flow_control_pin`, which ESPHome drives HIGH around each
transmission (in software) and holds LOW to listen. Nothing in the component has
to know about it.

The board also carries a CAN transceiver on `GPIO15`/`GPIO16` (unused here)
and a jumper-selectable 120 Ω RS485 termination resistor — check the jumper
against your bus before blaming the firmware.

Its USB-C port is wired straight to the ESP32-S3 native USB pins
(`GPIO19` D−, `GPIO20` D+); there is **no USB-UART bridge chip**, which is why
the config sets `logger: hardware_uart: USB_SERIAL_JTAG`. Logging on UART0
would go nowhere.

### Network

Static IP is used so there are no DHCP lease changes and no mDNS dependency
for OTA. Defaults (edit in the YAML to match your LAN):

- Static IP: `192.168.0.110`
- Gateway / DNS: `192.168.0.1`
- Subnet: `255.255.255.0`

> Pick an address **outside** your router's DHCP pool.

---

## Repository layout

| Path | Purpose |
| --- | --- |
| `olimpia.yaml` | **Main config, T-CAN485.** Full climate integration exposed to Home Assistant. |
| `olimpia-s3.yaml` | **Main config, Waveshare ESP32-S3-RS485-CAN.** Same integration, different board. |
| `sniffer.yaml` | Listen-only UART debug config used to reverse-engineer the bus (logs raw hex frames). T-CAN485. |
| `sniffer-s3.yaml` | Same listen-only debug build for the Waveshare board. |
| `components/olimpia_b0736/` | The custom ESPHome external component (the Modbus ASCII client + climate platform). |
| `secrets.yaml` | Wi-Fi credentials. **Git-ignored** — you create this locally (see below). |
| `capture.log` | Reference UART capture from bringup. |

---

## What the climate entity exposes

From the `olimpia_b0736` component:

- **Modes:** Off, Cool, Heat
- **Fan modes:** Auto, Low, Quiet (night), High
- **Setpoint:** 16–28 °C, 0.5 °C steps
- **Current temperature:** reported by the B0736 (0.1 °C resolution)
- **HVAC action:** heating / cooling / idle / off

Diagnostic sensors (all optional, configured in `olimpia.yaml`):

| Sensor | Meaning |
| --- | --- |
| Room temperature | Ambient temperature read from the B0736 |
| Fan coil fault | Problem flag (register 105, non-zero = fault) |
| Chiller demand | Chiller call-for-cooling status bit |
| Boiler demand | Boiler call-for-heat status bit |
| B0736 link | Connectivity — true while the Modbus session is alive |

### Register map (B0736 as Modbus slave)

Derived by capturing B1130 ↔ B0736 traffic:

| Reg | Access | Meaning |
| --- | --- | --- |
| 0 | R | Room temperature, ×0.1 °C |
| 9 | R | Status bits: b14 EV1, b13 boiler, b12 chiller, b11 EV2 |
| 105 | R | Fault (0 = healthy) |
| 199 | R | Identity constant (1200), read once per session |
| 201 | R/W | Control: b0–b2 fan, b7 standby |
| 231 | R/W | Setpoint, `(°C − 15.5) × 2` |
| 233 | R/W | Mode: 3 = heating, 5 = cooling |

---

## Build & deploy

### Prerequisites

- [ESPHome](https://esphome.io/guides/installation.html) installed
  (`pip install esphome`, or the Home Assistant add-on / Docker image).
- The board connected by USB for the **first** flash.

Everything below uses `olimpia.yaml` (T-CAN485). For the Waveshare board,
substitute `olimpia-s3.yaml` in every command — the steps are identical.

### 1. Create `secrets.yaml`

This file is git-ignored. Create it next to the YAML configs:

```yaml
wifi_ssid: "your-network"
wifi_password: "your-password"
```

### 2. (Optional) Adjust the network settings

In `olimpia.yaml`, set `manual_ip.static_ip` and `use_address` to the
address the board should have. On the very first flash `use_address` must be
the address the board is **on right now** (or use USB), otherwise OTA can't
find it. After the first flash with a new `manual_ip`, set `use_address` to
match `static_ip`.

### 3. First flash (over USB)

```bash
esphome run olimpia.yaml       # or: esphome run olimpia-s3.yaml
```

Select the serial port when prompted. The Waveshare board enumerates through
the ESP32-S3's built-in USB-Serial-JTAG, so no bridge driver is needed; if it
does not show up, hold BOOT while plugging it in to force the ROM bootloader. This compiles the firmware (including
the local `components/olimpia_b0736` external component) and uploads it.

### 4. Subsequent updates (OTA)

Once the board is on the network at its static IP, `esphome run` uploads
wirelessly — no USB needed:

```bash
esphome run olimpia.yaml
```

### Useful sub-commands

```bash
esphome compile olimpia.yaml   # build only
esphome logs olimpia.yaml      # stream logs (OTA/serial)
esphome clean olimpia.yaml     # wipe build cache
```

---

## Debugging the bus

If the link isn't working, `sniffer.yaml` is a listen-only build that dumps
every RS485 frame as hex + ASCII to the ESPHome log — handy for confirming
framing and watching the B0736's traffic:

```bash
esphome run sniffer.yaml       # or: esphome run sniffer-s3.yaml
esphome logs sniffer.yaml
```

`olimpia.yaml` also has a UART `debug:` block (commented guidance inline)
that prints wire-level TX/RX once `logger` level is `DEBUG`.

---

## Notes

- Inverting the UART makes the idle line read as continuous `0x00` — keep it
  **non-inverted**.
- `power_save_mode: none` keeps the ESP32 radio awake so the Home Assistant
  API connection stays stable.
- The T-CAN485 config uses `192.168.0.110` and the Waveshare config
  `192.168.0.121`, so both boards can sit on the bench at once. Change them to
  suit your LAN — just keep them apart.
