# 2026-09-16 — The B0736 cannot be made to regulate on an external probe

**Every protocol route is exhausted.** The fan coil regulates on the B0736's own
internal sensor, and nothing on the A'/B' supervisor port changes that.

| # | Route | Result |
| - | ----- | ------ |
| 1 | Trim reg 117 (calibration offset) | **Ruled out** - the error is not constant, it grows with cooling runtime (~0.15 degC idle, +2.5 degC after an hour). A fixed trim cannot track it. |
| 2 | Broadcast FC06 reg 103 (ambient) | **No effect.** 113 frames on the wire, 30.0 degC into an idle unit, zero change in reg 9 / 104 / 0 / action. |
| 3 | Broadcast triplet 101/102/103 | **No effect.** 45 frames, the B1130's own control word 0x4000 + real setpoint + 30.0 degC ambient, in the B1130's order. Zero change. |
| 4 | Write the B0736's own ambient (regs 0, 1, 2) | **Rejected.** All three read 225 before, immediately after an FC06 write of 300, and 4s later. Silently ignored - no exception, value unchanged. |

Route 4 also settles a question the register map left open: regs 0/1/2 are genuinely
read-only, not merely undocumented-for-write.

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
