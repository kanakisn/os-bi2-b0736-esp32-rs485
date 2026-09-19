# The B1130 driving a fan coil directly (no B0736)

**Status:** captured and decoded 2026-09-19. Replacement component not built yet.
**Capture:** `capture-b1130.log` (1719 decoded frames).
**Sniffer:** `sniffer-s3-b1130.yaml`, node `olimpia-s3-b1130-sniffer`, 192.168.0.125.

A second installation has a B1130 WiFi kit wired straight to a Bi2 fan coil, with no
B0736 thermostat and the coil not set to remote (RE).

## What it actually does

**Ordinary unicast Modbus ASCII polling of address 1.** Nothing exotic:

- poll cycle **2.82 s** mean over 94 cycles (2.73-3.48)
- 8 registers read per cycle (233 is read twice)
- **zero broadcasts**, zero writes unless the user acts
- writes are FC06 and are **echoed back** by the coil, so it is a proper slave
- no reg 199 IDENT handshake - it simply starts polling

## Corrects the earlier prediction

`.knowledge/features/b1130-direct-fancoil.md` previously predicted broadcast-only
operation with the master supplying ambient via reg 103, extrapolating from the first
20 minutes of `capture.log`. **That is wrong for this installation.** There are no
broadcasts at all here, and the B1130 never writes an ambient temperature - the fan
coil uses its own sensor (reg 0).

So the hope that "an external probe can drive the hardware here" is **not established**.
Whether this coil would accept a write to reg 0 is untested - the sniffer cannot
transmit. That is the first experiment to run once a board can talk.

## Register map, as observed

| Reg | Observed | Meaning |
| --- | -------- | ------- |
| 0 | 240, 241 | ambient, **x0.1 degC** |
| 9 | 0x4800 running; 0x0000 standby; 0xC800 on resume | status bits |
| 201 | 0,1,2,3 and 128 | control word - **bits 0-2 fan** (0 auto, 1 min, 2 night, 3 max), **bit 7 standby** |
| 202 | 150 | minimum setpoint, 15.0 degC |
| 203 | 300 | maximum setpoint, 30.0 degC |
| 224 | 128 | constant, unknown |
| 231 | 210, 220, 240 | setpoint, **x0.1 degC** |
| 233 | 5 | mode, 5 = cooling |

Confirmed by writes seen on the wire:

```
WRITE reg=201 = 128   standby on        WRITE reg=231 = 240   setpoint 24.0
WRITE reg=201 = 0     standby off       WRITE reg=231 = 210   setpoint 21.0
WRITE reg=201 = 1/2/3 fan min/night/max
```

## The trap: reg 231 is encoded differently here

On the **B0736** supervisor port, reg 231 is `(degC - 15.5) * 2` - raw 13 means 22.0.
On the **fan coil**, reg 231 is plain **x0.1** - 220 means 22.0, written directly.

Same register number, same nominal purpose, incompatible encoding. Reusing the B0736
encoder here would silently command wildly wrong temperatures. Min/max setpoint also
move: 202/203 here, 245/246 on the B0736.

## What the replacement needs

Close to `olimpia_b0736` in shape - a polling request/response client, same wiring and
UART (9600 7E1 non-inverted, GPIO17/18, `flow_control_pin: GPIO21`) - but a **separate
platform** with this register map and encodings. The control word layout (fan bits 0-2,
standby bit 7) is identical to the B0736's reg 201, so that logic transfers directly.

`update_interval` can match the B1130's 2.82 s, or be slower; nothing here suggests a
watchdog, since the coil holds its state between polls and is only written on demand.
