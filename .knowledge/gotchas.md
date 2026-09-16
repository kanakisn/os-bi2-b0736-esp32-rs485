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

For a **read** of an unimplemented register the B0736 does not reply at all - no
Modbus exception, just silence. Not once in the 18k-line `capture.log`, and not for
any unimplemented register in a full 0-255 sweep. Exactly 40 registers are
implemented (list in `CLAUDE.md`).

So a timeout during read exploration is a normal, informative answer. Do not go
chasing the transceiver, the grounding or the framing because a scan came back partly
empty.

**Correction (2026-09-16):** "the B0736 never returns exceptions" was recorded here as
a blanket rule and that is wrong. An **FC16 write** to regs 0/1/2 returns exception
**0x04 (slave device failure)**. The rule holds only for reads. A write can and does
get a real exception back.

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

## The B0736's own sensor does not measure the room while the unit is running

Measured 2026-09-16 on the guest unit, with a settled external probe for reference.
The fan coil cooled continuously for an hour (640 samples, `Action: COOLING`
throughout, no idle period). Over that hour:

| time  | probe (real room) | reg 0 (B0736) | gap |
| ----- | ----------------- | ------------- | ---- |
| 16:09 | -                 | 25.5          | -    |
| 16:39 | 25.87             | 24.6          | +1.3 |
| 16:51 | 25.56             | 23.5          | +2.1 |
| 17:07 | 25.49 (flat 15m)  | 23.0          | +2.5 |

The probe was **flat** at 25.5 for the last fifteen minutes while reg 0 kept falling.
They are not measuring the same air. The B0736's internal sensor is being pulled down
by the unit's own cold output - it reads its supply air, not the room.

**Consequences:**

1. **Reg 117 cannot fix this.** The error is not a constant sensor offset, it grows
   with cooling runtime (+0.15 degC when settled and idle, +2.5 degC after an hour of
   cooling). A fixed trim would be wrong in every state except the one it was measured
   in. This rules out the reg 117 plan recorded in
   `.knowledge/features/external-ble-room-probe.md`.
2. **The zone undershoots its setpoint badly.** Setpoint 22.0, B0736 reads 23.0 and
   falling, so it will stop cooling shortly - while the room is actually 25.5. That is
   a ~3.5 degC comfort error, and it is a property of the installation, not a bug in
   this project.
3. **Any single-point comparison of probe vs reg 0 is meaningless** unless you also
   record what the unit was doing. Note the `Action:` and reg 9 state alongside.

**Do not conclude anything from a probe reading until it has settled.** The Xiaomi
LYWSD03MMC has a measured thermal time constant of **14.2 min** (exponential fit,
R^2 = 0.994), so it needs 45-70 minutes after being moved. An earlier reading taken
25 minutes in suggested a +0.15 degC offset and "no trim needed"; that was the
transient, and it was wrong in both directions.

## Write-probing registers 125/126 latches fault 4 on the B0736

**2026-09-16: this happened. Do not repeat it.** A write-probe sweep (write
`current+1`, restore immediately) over the unexplained registers latched the guest
unit into **fault 4** (`reg 105 = 0x0004`) immediately after probing **reg 126**. The
value was restored to 0 within about a second and every other register came back to
its baseline - but the fault is latched and does not follow the value back.

It is not a system-wide condition: at the same moment the other two units read
`reg 105 = 0` and `reg 104 = 1` (running), while the guest read `105 = 4`, `104 = 0`
(idle, refusing to operate).

**None of these clear it:**

- writing 0 to reg 105 (it is a status register, not a control)
- standby on -> off via control bit 7
- mode cycle cooling -> heating -> cooling

The remaining fix is a **physical power cycle of the B0736**.

Circumstantial reading of the cause: reg 126 may select the temperature source
(internal vs a remote air probe). Writing 1 would then select a probe that is not
fitted, and the unit latches an "air probe missing" fault. Supporting evidence:
**reg 117, the calibration offset, changed by itself** at the same moment
(65526 -> 65521) without ever being written - exactly what a sensor-source change
would do. Unproven, and not worth proving on a live system.

**Confirmed reproducible 2026-09-16 (second occurrence):** writing `reg 126 = 1`
again, this time with the probe disconnected, added fault bit 2 (8 -> 12) *and* reset
reg 117 from 65526 to 65521 - the same coupling as the first time. Writing 126 back to
0 clears neither. So reg 126 = 1 reliably latches a fault and resets the calibration
offset to what looks like a factory default (-1.5 degC, the value the other two units
carry). It is not a temperature-source selector in any useful sense.

**If a write-probe sweep is ever run again:** exclude 125 and 126, keep the
snapshot-and-restore discipline (it worked - everything except the latched fault came
back), and do it on a unit nobody is relying on.

Registers 127, 128 and 129 - which the first sweep aborted before reaching - were
probed later and all three accept a write and restore cleanly, with no fault.

### What the sweep did establish

| | registers |
| --- | --- |
| Accepted a write, restored cleanly | 100, 106, 107, 112, 113, 118, 119, 120, 121, 125, 126 |
| Rejected the write (read-only) | 0, 1, 2, 108, 111 |

**Reg 117 is writable** - confirmed when it was restored from 65521 back to 65526.
That settles an open question, even though trimming it was already ruled out because
the B0736's error is not a constant offset.

## The B0736 will not accept an external ambient temperature. At all.

Exhaustively tested 2026-09-16. Every route fails:

| Route | Result |
| ----- | ------ |
| FC06 write to regs 0/1/2, probe fitted | silently rejected, value unchanged at ~150 ms |
| FC06 write to regs 0/1/2, **probe physically disconnected** | still rejected - stays 0x7FFF |
| FC16 write to regs 0/1/2 | **exception 0x04** (slave device failure) |
| FC16 write to reg 231 (known FC06-writable) | silently ignored - FC16 does not write here at all |
| Broadcast FC06 reg 103 (fan-coil ambient) | no effect, 113 frames |
| Broadcast triplet 101/102/103 | no effect, 45 frames |
| Unicast to a fan coil | impossible - see below |
| Any other writable register carrying 251 (25.1 degC) | none is an ambient input |
| FC06/FC16 to reg 8 (the setpoint x10 mirror) | **exception 0x04** - read-only, so no 0.1 degC setpoint resolution either |
| Reads of registers 256-319 | nothing answers - the map really does end at 255 |
| reg 126 = 1 then writing ambient | still refused, and it latches a fault |

**The fan coils are not on the supervisor bus.** An address sweep of 1-31 on A'/B'
found exactly one responder: address 1, the B0736 itself. The fan coils live behind it
on the internal A/B bus and cannot be addressed from here at all. That kills the
unicast route and explains why the broadcasts do nothing.

Removing the probe was worth trying - with no sensor generating reg 0 there is nothing
to overwrite - and it still refused. Regs 0/1/2 are not "overwritten", they are
genuinely not writable.

**Consequence:** the only remaining way to make the room reach its target is to
compensate the *setpoint* (reg 231, which is writable), using the measured error
`probe - reg0`. That needs reg 0, so the B0736's own probe must stay connected.

## Reg 0 = 0x7FFF (32767) means "no probe", not 3276.7 degC

With the B0736's air probe disconnected, regs 0, 1 and 2 all read 32767 and reg 105
latches fault **8**. Anything consuming reg 0 must treat 0x7FFF as invalid - publishing
it as a temperature gives Home Assistant 3276.7 degC.

Reg 105 is a **bitfield**: 8 (bit 3) = air probe missing; the 4 (bit 2) latched by
write-probing reg 126 is a different fault entirely.

### Still untested, if anyone wants to keep going

- **The >=200 EEPROM block**: 207, 209, 218, 219, 221, 222, 224, 225, 247 were never
  write-probed, deliberately. A "temperature source" parameter is most plausibly here.
  The risk is different in kind from the volatile range: these persist across a power
  cycle, so a bad value cannot be cleared by power-cycling the way a latched fault can.
  Baseline values for all of them are in the snapshot, so a restore is possible.
- **FC04 (read input registers)**: every read so far has used FC03. FC04 addresses a
  separate space in Modbus and might expose registers FC03 does not. Read-only, so it
  cannot solve the write problem, but it could reveal what else exists.
- **Reads above 319**: only 256-319 was sampled.
