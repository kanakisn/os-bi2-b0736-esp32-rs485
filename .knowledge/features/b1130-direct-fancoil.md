# The B1130 driving a fan coil directly (no B0736)

**Status:** sniffer built and committed, capture not yet taken.
**Config:** `sniffer-s3-b1130.yaml`. **Node:** `olimpia-s3-b1130-sniffer`, 192.168.0.125.

A second installation has a B1130 WiFi kit wired straight to a Bi2 fan coil, with **no
B0736 thermostat** and the coil **not** set to remote (RE). The goal is to replace the
B1130 with a Waveshare, as was done for the three B0736 units.

## How it controls the coil, from evidence already in capture.log

`capture.log` was taken on the *other* installation, but its first twenty minutes
(15:14-15:35) show the B1130 before any B0736 polling begins. In that window it does
**nothing but broadcast writes** - it never reads, and never addresses a slave:

```
15:28:25  addr=0  W reg=101 = 16384    control word 0x4000
15:28:26  addr=0  W reg=102 = 230      setpoint 23.0 degC (x0.1)
15:28:27  addr=0  W reg=103 = 241      ambient  24.1 degC (x0.1)
15:30:24  addr=0  W reg=101 = 16384    ... repeats on a ~60s timer
```

The first non-broadcast frame in the whole capture is at 15:35:56, `addr=1 R reg=199`.

So the B1130 is **write-only, fire-and-forget**: it broadcasts the full command set on a
timer and again immediately on a user change (at 15:28:52 reg 101 briefly became
16386 = 0x4002, fan speed 2/night, then back to 0x4000).

**Two consequences:**

1. **No RE jumper is needed** because remote control is asserted *in the protocol* -
   bit 14 of the control word (reg 101 = 0x4000) - not by a physical selector.
2. **The master supplies the room temperature**, via reg 103. The coil regulates on
   whatever it is told. That is exactly what the B0736 refuses to accept
   (see `2026-09-16-external-probe-cannot-steer-hardware.md`), so in *this*
   installation an external probe such as the Xiaomi genuinely can drive the hardware.

## Why the replacement is a different component

`olimpia_b0736` is a request/response client polling a slave. This installation needs a
**periodic broadcaster** over the fan-coil map (101/102/103), possibly with reads if the
coil answers at its own address. Wiring and UART are identical: 9600 7E1 non-inverted,
GPIO17/18, `flow_control_pin: GPIO21`.

## What the capture must answer before building anything

1. Broadcast only, or does it also unicast to a fan-coil address?
2. The repeat interval. Reg 103 reportedly must be rewritten periodically or the coil
   reverts - that watchdog period sets `update_interval`.
3. Does the fan coil **answer reads**? If so, status, water temperature and fault are
   available, and reg 200 gives its address.
4. The control-word bits for fan speed, standby and mode on this unit.

Change setpoint, fan speed and on/off in the Olimpia app while capturing - the deltas
are what identify the bits.

## The sniffer decodes inline

`sniffer-s3-b1130.yaml` prints decoded Modbus ASCII as it arrives - address, function,
register, value, broadcasts flagged, LRC checked - instead of only hex-dumping the way
the older sniffers do. Every analysis in this repo so far has meant dumping hex and
parsing it offline in Python; this removes that step. Raw hex still goes out at DEBUG
in case the framing turns out not to be Modbus ASCII.

It never transmits, so it can be tapped across the live A/B pair safely.
