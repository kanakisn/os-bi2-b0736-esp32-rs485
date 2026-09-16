# Console tools

Register exploration from a terminal instead of Home Assistant. Both scripts
talk to the node over the ESPHome **native API** (port 6053) and call the
`read_register` / `write_register` / `scan_registers` / `set_control_bit`
services defined in the `api:` block of each `olimpia-s3*.yaml`.

The `api:` block on these nodes has no encryption key and no password, so no
credentials are needed.

## Interpreter

The scripts need `aioesphomeapi`, which is already bundled inside the esphome
install — there is nothing to `pip install`. Derive the path rather than
hardcoding it, so it survives an esphome upgrade:

```bash
ESPY=$(head -1 "$(which esphome)" | sed 's|^#!||')
cd tools
$ESPY osreg.py -n master read 117
```

A plain `python3` only works if you installed `aioesphomeapi` into it yourself.

## osreg.py

One-shot service call, then prints the registers that answered.

```bash
$ESPY osreg.py -n master read 117        # one register
$ESPY osreg.py -n guest  scan 110 16     # sweep a range (max 64 per call)
$ESPY osreg.py -n first  -q scan 0 20    # -q: only REG lines
$ESPY osreg.py -n master watch 60        # tail traffic, send nothing
$ESPY osreg.py -n master write 117 0     # asks before writing
$ESPY osreg.py -n master bit 7 1         # control-word bit 7 = standby
```

`-n` takes `first` / `master` / `guest` (mapped in `NODES`, mirroring the
`static_ip` of each config) or a bare IP.

Values with the high bit set are also printed as a signed number and as °C,
since at least reg 117 carries a signed temperature.

## sample.py

Holds one connection per node and samples on a cadence — this is what tells a
live derived value apart from a stored constant. A register that tracks
ambient is computed; one that stays pinned while ambient drifts is stored.

```bash
$ESPY sample.py               # 14 rounds, 50s apart, regs 0/117/9
$ESPY sample.py 30 60         # 30 rounds, 60s apart
$ESPY sample.py 10 30 113 247 # watch regs 113 and 247 instead
```

Reg 0 (ambient) is always included as the reference column.

## Caution

- Registers **≥ 200 live in EEPROM** with limited write endurance. Never loop
  writes over them. `write_register` refuses only reg 200 (slave address); the
  rest is on you.
- Read first, across several states (idle / heating / cooling / standby),
  before writing anything. A register that reads 0 once has told you nothing.
