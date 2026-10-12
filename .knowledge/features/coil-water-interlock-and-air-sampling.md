# Fan coil water interlock, "air sampling", and what we can control

**Status:** researched 2026-10-08 on the loft coil (`olimpia-lcd-loft.yaml`). Nothing
built yet. This is groundwork for gating the coil on water availability.

## The question

Driven directly (no B0736), a coil that is switched on or given a new setting **blows
for a few minutes even when the water is unusable**, and only then holds its fan and
blinks its water icon. Can that be controlled, or avoided?

## What the coil does (observed)

Loft coil, 26.0 degC water, cooling limit 20.0 degC (reg 219):

| Event | Fan held after | Speed while blowing |
| --- | --- | --- |
| Cool selected, 13:29 | about 5 min | Auto: 956 rpm (reg 212, Td = max) |
| Fan Min selected, 13:38 | still blowing at 678 rpm until the restart | Min |
| Power cycle, ~13:41:30 | 1 min 45 s (held 13:43:17) | Max: 1414 rpm |

- Valves EV1 and EV2 stay **open** throughout, both while blowing and while held.
- The hold shows on the bus as **reg 9 bit 15 clearing** (`0xC800` -> `0x4800`), with
  fan target/actual (regs 15/16) at 0. `olimpia_fancoil` publishes it as `fan_running:`
  and reports action IDLE while it is clear. See `.knowledge/gotchas.md`, "A fan coil
  will not run the fan if the water is the wrong temperature".
- The chiller-demand bit (reg 9 b12) was **never** set on this coil. Water sat at 26.0
  for over 30 min with the valves open, so no chilled water was reaching it at all.
- After the hold, no fan restart was seen in the following 3 minutes. That is too short
  to rule out air sampling (below), whose interval can be up to 60 min.

## What Olimpia's documentation says

Source: the manual for the **B0828 / B0855** Bi2 control kit,
`https://www.olimpiasplendid.it/media/files/5147_279581D%20MANUALE%20B0828%20-%20B0855.pdf`
(read with `pdftotext -layout`; English is the "GB" section). **Caveat:** this is the
kit manual for the Bi2 family's control board. Our coils have the onboard control the
B0736 pairs with (B0872), so the behaviour matches but it is not that board's own
manual.

### Water interlock - not configurable

> If either of the two LEDs flashes, this means that the water demand (hot or cold) is
> not satisfied and so the fan shuts down until the water temperature reaches a
> suitable value to satisfy the demand.

That is the blinking water icon. The thresholds are settable (regs 218 heating minimum,
219 cooling maximum), but the manual documents **no switch to disable the check** and
**no setting for the initial run before the coil decides**. That initial run is not
documented at all.

### "Air sampling" - configurable, service-centre option

> Air sampling: in heating or cooling mode and with ambient temperature above or below
> the required value respectively, the fan is powered for 1 minute at minimum speed and
> at intervals that can be set to between 1 and 60 minutes. In this way, the system is
> able to control the temperature of the surrounding environment and reactivate more
> quickly if necessary.

> Both of the above functions [air sampling and ambient offset] can be activated at the
> factory or by an authorised service centre at prior customer request.

So a coil with air sampling enabled gives **periodic 1-minute blows at minimum speed**
while waiting. That is a different thing from the initial multi-minute run seen above.

**Which register holds it is unknown.** Candidates are the config registers that read 0
on the loft coil: 220-223, 225-230, 232, 234, 239. The ambient offset (+/-5 degC) is
plausibly among 235-238 (all 50, which would fit a +/-5.0 degC offset encoded around
50). **Do not write-probe these** - they are EEPROM settings, and
`.knowledge/gotchas.md` records what blind write-probing did to a B0736. Diff against a
coil known to have air sampling enabled first.

### Remote mode ("rE") - confirms reg 224 bit 2

> To enable communication with thermostat B0736, hold down keys - and [mode] for 10
> seconds. ... If the function is active, each time a key is pressed, the letters "re"
> appear for a few seconds on the display. To select a function, the "remote control"
> must be disabled.

This is reg 224 bit 2 (`0x84` remote on, `0x80` off). Turning it off over Modbus
(reg 224 = 0x80) is equivalent to the key combination, and survived a power cycle.

### Jumpers J1-J3 on the control board

They are not separate jumpers: **J1, J2 and J3 are the three positions of one 3-way
DIP switch** (printed 1 2 3 on the board), each a slider with an ON and an OFF side.
The manual's text says they are "indicated in fig. 6", but fig. 6 is the key panel.
The switch is actually drawn as a magnified inset in **fig. 5**, the wiring diagram:

- close-up: [`../assets/bi2-board-dip-switch.png`](../assets/bi2-board-dip-switch.png)
- both board variants with the switch's position:
  [`../assets/bi2-board-wiring-fig5.png`](../assets/bi2-board-wiring-fig5.png)

(Both cropped from page 23 of the B0828/B0855 manual linked above.)

**Where it is on the board.** Upper left, just below the two relays (**X1**) and the
fuse **F1**, near the 230 V mains terminals (L, N, earth). It is mounted turned 90
degrees, so read the ON marking on the switch body rather than assuming "up". On the
B0828 board it sits directly above the fan motor connector **X10**; on the B0855 board
it is left of the large valve/motor connector strip. Landmarks: the **A/B** RS485
terminals are at the lower left, and **X4** (H2, water probe) and **X3** (AIR probe)
are along the bottom edge. The drawing shows all three sliders identically, so it says
**nothing about factory settings**. Our coils' boards may differ in layout from this
kit, so the switch's own printed labels win over this description.

The manual says the board "must be configured according to the model of Bi2 ... on
which it is installed", and that the three settings are independent of each other:

- **J1** (radiant plate, heating at night). ON: "for appliances with radiant plate,
  such as SLR or SLR+" - at night, near setpoint, heat by radiation and natural
  convection with the fan inhibited. OFF: "for appliances without radiant plate, such
  as SL, SL+ or SLN" - always forced convection, fan active at night at reduced speed.
  Must match the model. Heating-only; unrelated to the warm-water blowing.
- **J2** (fan at setpoint, cooling only). ON: "in cooling mode the fan remains powered
  even when the required ambient temperature is reached". OFF: "the fan deactivates on
  reaching the set temperature". A comfort choice. It does **not** override the water
  interlock - wrong water stops the fan either way.
- **J3** (plumbing). ON: "2-pipe systems: ... a single water valve for summer
  (cooling) and winter (heating) mode", also used on Full Flat models with movable
  suction panels. OFF: "4-pipe systems: ... 2 water valves, one for summer and one for
  winter". Must match the plumbing.

**Possible link to our reg 9 valve bits.** The loft coil reports *both* EV1 (b14) and
EV2 (b11) set. Fig. 5 shows two outputs, **Y1** (hot water valve) and **Y2/FF** -
Y2 doubles as the movable-panel drive on Full Flat models. So two "valve" bits on what
may be a 2-pipe system could mean J3 = OFF, or that the second output drives
something other than a valve. Counting the valves actually wired would settle it.

**Practicalities.** None of the three is known to be readable or settable over
Modbus - it takes someone at the coil. The switch sits next to the 230 V side (the
valves are 230 V / 50 Hz), so **isolate the coil first**. The board most likely reads
the switch at power-up: change it powered off, then power on.

**Not yet recorded:** the actual J1-J3 positions on any of our coils. A photo of the
switch on the loft coil would fill that in.

### Presence contact input (terminals A/B of the presence input, not the RS485 pair)

Closing a free (unpowered) presence-sensor contact moves the setpoint by **2.5 degC**
(up in cooling, down in heating). If that offset is set to 0 degC, closing the contact
puts the coil in **standby** instead, which is useful as a window contact. The offset
is "changed in the factory or by an authorised service centre". The input cannot be
paralleled across boards.

### Other signals worth knowing

- Red/blue LEDs on: heating/cooling. Both off: ventilation (fan-only, valves not
  driven). Red and blue alternating: automatic cool/heat changeover active.
- Error codes: **E2** ambient probe fault, **E3** water probe fault, **E4** fan motor or
  speed sensor fault, **FI** filter maintenance due.
- Night mode moves the setpoint by 1 degC after one hour and another after two
  (down in heating, up in cooling).

## What *we* can control

The coil only blows warm air when it is switched on or re-commanded. So the lever is
**when the screen or HA switches the coil on**, not the coil's internal logic:

1. **Gate on the heat pump (preferred).** If HA exposes whether the heat pump runs and
   in which mode (or its flow temperature), hold the loft in standby until the right
   water is available. The coil then never blows warm air. Blocked on knowing which HA
   entity that is.
2. **Gate on another coil's water temperature.** The basement coil (`olimpia_fancoil`,
   reg 1) reports water temperature. This only works if it is on the same circuit, and
   that coil also needs flow to read anything but stagnant water.
3. **Back off after a failed check.** After `fan_running` clears with the valves open,
   go to standby and retry every 30-60 min. Each retry still costs a few minutes of
   warm air.

**Do not gate on this coil's own reg 1 while it is in standby.** With the valves shut
the probe reads stagnant water and would never see the water become ready - a deadlock.

## Open questions

- Is air sampling enabled on the loft coil, and at what interval? Watch `fan_running`
  for an hour while the water icon blinks: periodic 1-minute ON pulses at about 680
  rpm would answer it.
- Which register (if any) holds the air-sampling interval and the ambient offset? Needs
  a diff against a coil with a known configuration.
- Why is the initial run so variable (5 min vs 1 min 45 s)? It may depend on the fan
  speed, on how far the water reading has to move, or on a timer started by the command.
