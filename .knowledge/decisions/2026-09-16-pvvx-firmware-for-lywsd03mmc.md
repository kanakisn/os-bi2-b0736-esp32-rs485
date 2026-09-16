# 2026-09-16 — pvvx custom firmware on the LYWSD03MMC, not stock + bindkey

**Decision:** reflash the Xiaomi LYWSD03MMC with pvvx custom firmware and read it
with ESPHome's `pvvx_mithermometer`, rather than keeping stock firmware and
extracting a bindkey for `xiaomi_lywsd03mmc`.

## Why

A 100-second discovery scan from the guest node measured the sensor at
`A4:C1:38:B1:C0:3A`, -75 dBm, with **one advertisement in 100 seconds**, encrypted.

- One advert per 100 s cannot drive a control loop, and stock firmware gives no way
  to change the advertising interval. pvvx makes it configurable (set to a few
  seconds).
- Stock advertisements are encrypted, so `xiaomi_lywsd03mmc` needs a bindkey
  extracted from the Xiaomi cloud account — a fiddly step requiring the user's
  Xiaomi credentials, and it does not fix the advertising rate.
- pvvx additionally exposes battery level and voltage.

## Cost accepted

The sensor stops working in the Xiaomi Home app. Reversible — stock firmware can be
reflashed from the same tool.

## Required configuration

pvvx flasher settings, after flashing: **Advertising type = `Custom`**, encryption
**off**, interval a few seconds. `atc1441` and Mi-like formats are not readable by
`pvvx_mithermometer` and fail silently — see `.knowledge/gotchas.md`.

## Alternative if this is ever revisited

Keeping stock + bindkey remains viable for a sensor that only needs occasional
reporting rather than control, and preserves Mi app functionality.
