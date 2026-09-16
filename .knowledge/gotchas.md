# Gotchas

Traps that cost real time on this project. Protocol facts live in `CLAUDE.md`;
this file is only for things that actively mislead.

## A single register scan cannot tell a stored constant from a derived value

Found while mapping reg 117 (`components/olimpia_b0736/olimpia_b0736.h`, register
map block at the top).

One scan of all three units had them satisfying `ambient - reg117 = 265` exactly.
That reads as a live deviation term, and the community register map reinforces it —
regs 204-213 keep referring to a "Td" deviation variable. It was a coincidence: two
of the three units happened to share *both* ambient and reg 117, so there were only
two independent points. The invariant broke the moment ambient drifted, with reg 117
sitting still at -15 while reg 0 fell 250 -> 245.

**Sample over time against reg 0 before believing any relationship between
registers.** That is what `tools/sample.py` exists for; `tools/osreg.py` alone will
happily hand you a convincing false lead.

## Silence from the B0736 means "no such register", not "bus fault"

The B0736 never returns a Modbus exception — not once in the 18k-line `capture.log`,
and not for any unimplemented register in a full 0-255 sweep. It simply does not
reply. Exactly 40 registers are implemented (list in `CLAUDE.md`).

So a timeout during exploration is a normal, informative answer. Do not go chasing
the transceiver, the grounding or the framing because a scan came back partly empty.

## Absence from capture.log does not mean a register does not exist

Regs 8, 117, 245 and 246 are absent from `capture.log` purely because the B1130 never
asked for them. The B0736 answers all four. `CLAUDE.md` says to treat anything outside
the captured set as unknown — "unknown", not "absent".

## esp32_ble_tracker logs nothing unless something is listening

Enabling `esp32_ble_tracker` and raising the log level produces no device list. The
scanner runs (`Scanner State: RUNNING`) but advertisements are only logged when a
listener exists. An explicit `on_ble_advertise:` trigger is what makes a discovery
scan visible — see the block in `olimpia-s3-guest.yaml`.

Related: a per-component entry under `logger: logs:` may not be *more* verbose than
the global `level:`. Setting `esp32_ble_tracker: DEBUG` under a global `INFO` is a
config error, not a silent no-op.

## Stock Xiaomi firmware looks identical to a BLE range problem

`pvvx_mithermometer` reads only the pvvx "Custom" advertising format, unencrypted.
Stock LYWSD03MMC firmware advertises an *encrypted* payload, and the pvvx platform
simply hears nothing — which is indistinguishable from the sensor being out of range
or the wrong MAC.

If a flashed sensor reports nothing, check the advertising *type* in the pvvx flasher
before moving the sensor or touching the MAC. `atc1441` and Mi-like formats produce
the same silence. Stock firmware would need `xiaomi_lywsd03mmc` plus a bindkey.
