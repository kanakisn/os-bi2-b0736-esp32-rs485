# 2026-09-16 — The B0736 cannot be made to regulate on an external probe

**Every protocol route is exhausted.** The fan coil regulates on the B0736's own
internal sensor, and nothing on the A'/B' supervisor port changes that.

| # | Route | Result |
| - | ----- | ------ |
| 1 | Trim reg 117 (calibration offset) | **Ruled out** - the error is not constant, it grows with cooling runtime (~0.15 degC idle, +2.5 degC after an hour). A fixed trim cannot track it. |
| 2 | Broadcast FC06 reg 103 (ambient) | **No effect.** 113 frames on the wire, 30.0 degC into an idle unit, zero change in reg 9 / 104 / 0 / action. |
| 3 | Broadcast triplet 101/102/103 | **No effect.** 45 frames, the B1130's own control word 0x4000 + real setpoint + 30.0 degC ambient, in the B1130's order. Zero change. |
| 4 | Write the B0736's own ambient (regs 0, 1, 2) | **Rejected** (FC06). See below - the first test of this was flawed and was redone. |

### Route 4, done properly

The first attempt read the register back 2 and 4 seconds after writing, which cannot
tell "rejected" from "accepted, then overwritten by the next measurement cycle" - reg 0
is a live sensor value. Redone using the read-back `write_register()` already queues
immediately after the write (~150 ms), with a control:

```
CONTROL  reg 231 <- 14   readback @0.12s = 14   ACCEPTED
CONTROL  reg 231 <- 13   readback @0.08s = 13   ACCEPTED  (restored)
SUBJECT  reg 0   <- 350  readback @0.18s = 225  NOT accepted
SUBJECT  reg 0   <- 99   readback @0.21s = 225  NOT accepted
SUBJECT  regs 1, 2       same, both values      NOT accepted
rapid-fire reg 0 <- 350, 15x at 250ms: only ever 225
```

The control proves the method detects an accepted write inside the same window. So
regs 0/1/2 genuinely reject FC06 - they are read-only, not merely undocumented.

**Caveat, untested:** only FC06 was tried. FC16 (write multiple) is not implemented in
this component and some devices accept only that. Weak counter-evidence: `capture.log`
contains only FC03 and FC06, and the B1130 never wrote the B0736's ambient by any
function - it injected ambient by broadcasting to the fan coils instead.

## Why broadcasts do nothing here

The B0736 is master of the internal A/B fan-coil bus and writes its own ambient there
every watchdog cycle. A broadcast on the supervisor port is either not relayed to the
fan coils at all, or relayed and immediately overwritten. The B1130's broadcasts in
`capture.log` clearly reached something; ours do not. Do not spend more time on this
without evidence the bus topology differs from what is assumed.

## What is left: compensate via the setpoint

Reg 231 **is** writable and the climate entity already drives it. Since the error is
measurable in real time - `delta = probe - reg0` - the loop can be closed in software:

```
setpoint_to_write = desired_room_temp - (probe - reg0)
```

The B0736 then reaches its (deliberately wrong) setpoint at exactly the moment the
real room reaches the desired temperature. Constraints: reg 231 clamps to 16-28 degC
(regs 245/246), and it quantises to 0.5 degC, so the compensation is coarse and needs
a deadband to avoid writing on every poll. Not yet implemented.
