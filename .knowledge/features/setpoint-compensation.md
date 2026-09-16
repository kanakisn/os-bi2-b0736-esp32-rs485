# Setpoint compensation (guest node)

**Status:** built and deployed 2026-09-16, **switched off by default**.

## Why it exists

The B0736's own probe sits in the fan coil's cold output. It reads roughly the room
when idle (~0.15 degC error) but drifts far below it while cooling - measured at
**+2.5 degC after an hour**, probe flat at 25.5 while reg 0 fell to 23.0. The B0736
regulates on that reading, so the room settles well above its setpoint. With setpoint
22.0 the room sat at 25.5: a ~3.5 degC undershoot.

Feeding the B0736 the real temperature is impossible - every route was tested and
refused, see `.knowledge/decisions/2026-09-16-external-probe-cannot-steer-hardware.md`.
So compensate the one thing that *is* writable, the setpoint.

## How it works

```
error    = probe - reg0          how much warmer the room really is
write to = target - error        a deliberately wrong setpoint
```

The B0736 reaches its wrong setpoint exactly when the real room reaches the right
temperature. All of it lives in `olimpia-s3-guest.yaml` - a 60 s `interval:` lambda,
deliberately in YAML rather than the component so each step is visible and tunable.

## Entities

| Entity | What it is |
| ------ | ---------- |
| `External probe temperature` | the **real** room temperature (Xiaomi over BLE) |
| `Room temperature` | what the B0736 believes (reg 0) |
| `Temperature error` | `probe - reg0` - the whole problem, as one number |
| `Corrected setpoint` | what compensation *would* write, computed continuously |
| `Target room temperature` | number, 16-28 degC - the real room temperature wanted |
| `Setpoint compensation` | switch - off after every reboot, on purpose |

`Corrected setpoint` updates whether or not the switch is on, so the correction can be
watched before anything is allowed to act on it.

## Deliberate choices

- **The switch defaults off** (`RESTORE_DEFAULT_OFF`). It writes to the B0736
  unattended; it should only run because someone turned it on.
- **Writes only on change.** The computed raw is compared against the climate entity's
  current target and skipped if equal, so the bus stays quiet.
- **Skips on any NaN.** A stale BLE probe, or reg 0 reading 0x7FFF with the probe
  unplugged, makes it log and do nothing rather than compute from garbage.
- **The climate entity is left honest** - it still reports reg 0 and the raw setpoint,
  so it mirrors the hardware. The compensation is a visible layer on top, not a lie
  buried inside the entity.

## Limits

Reg 231 clamps to **16-28 degC** and quantises to **0.5 degC**. An error larger than
`target - 16` cannot be fully corrected - the lambda logs `[CLAMPED]` when it hits the
rail. Correction is coarse to 0.5 degC, which is fine against a 2.5 degC error.

## Before trusting a reading

The B0736's probe needs time to equilibrate after being reconnected, and the Xiaomi has
a **14.2 min** thermal time constant. Neither number means anything until both have
settled - see `.knowledge/gotchas.md`.
