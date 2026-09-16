#!/usr/bin/env python3
"""Call the olimpia_b0736 exploration services from the console.

Connects over the ESPHome native API, subscribes to the log stream, fires the
service and prints every REG line that comes back. No Home Assistant needed.

The api: block on these nodes has no encryption key and no password, so a
plain connection to port 6053 is enough.

Needs aioesphomeapi, which ships inside the esphome install - see tools/README.md.

  osreg.py -n master read 117        # one register
  osreg.py -n guest  scan 110 16     # sweep a range
  osreg.py -n first  -q scan 0 20    # -q: only REG lines
  osreg.py -n master watch 60        # tail register traffic, send nothing
  osreg.py -n master write 117 0     # asks before writing
  osreg.py -n master bit 7 1         # control-word bit (7 = standby)
"""
import argparse
import asyncio
import re
import sys
from datetime import datetime

from aioesphomeapi import APIClient, LogLevel

# Mirrors the static_ip of each olimpia-s3*.yaml.
NODES = {
    "first": "192.168.0.121",   # olimpia-s3        "First Floor FCU"
    "master": "192.168.0.122",  # olimpia-s3-master "Master Bedroom FCU"
    "guest": "192.168.0.124",   # olimpia-s3-guest  "Guest Room FCU"
}

REG_LINE = re.compile(rb"REG (\d+) \(0x[0-9A-Fa-f]{4}\) = (\d+)")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def signed(v: int) -> int:
    """Registers are 16-bit; some (117) carry a signed temperature."""
    return v - 65536 if v > 32767 else v


async def run(host: str, service: str | None, data: dict, secs: float, quiet: bool) -> int:
    cli = APIClient(host, 6053, None)
    await cli.connect(login=True)

    seen: dict[int, int] = {}

    def on_log(msg) -> None:
        if b"olimpia_b0736" not in msg.message:
            return
        text = ANSI.sub("", msg.message.decode("utf-8", "replace")).rstrip()
        m = REG_LINE.search(msg.message)
        if m:
            seen[int(m.group(1))] = int(m.group(2))
        if not quiet or m:
            print(f"{datetime.now():%H:%M:%S} {text}", flush=True)

    cli.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_INFO)
    await asyncio.sleep(0.5)

    if service:
        _, services = await cli.list_entities_services()
        by_name = {s.name: s for s in services}
        if service not in by_name:
            print(f"error: {host} has no service {service!r}; has {sorted(by_name)}", file=sys.stderr)
            await cli.disconnect()
            return 2
        await cli.execute_service(by_name[service], data)
        print(f"-> {service}({data}) sent to {host}", flush=True)

    await asyncio.sleep(secs)
    await cli.disconnect()

    if not seen:
        print("\n(no REG lines - register never answered, or the link is down)")
        return 1

    print("\n=== registers answered ===")
    for reg, val in sorted(seen.items()):
        s = signed(val)
        extra = f"  signed {s:>6}  {s / 10:>6.1f} degC" if s < 0 else ""
        print(f"  {reg:5d} (0x{reg:04X}) = {val:5d}  0x{val:04X}  {val >> 8:08b} {val & 0xFF:08b}{extra}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("-n", "--node", default="master", help="node name or IP (default: master)")
    p.add_argument("-t", "--secs", type=float, default=0, help="override seconds to listen")
    p.add_argument("-q", "--quiet", action="store_true", help="only show REG lines")
    p.add_argument("cmd", choices=["read", "scan", "write", "bit", "watch"])
    p.add_argument("args", nargs="*", type=int)
    a = p.parse_args()

    host = NODES.get(a.node, a.node)

    if a.cmd == "read":
        svc, data, secs = "read_register", {"reg": a.args[0]}, a.secs or 4
    elif a.cmd == "scan":
        start = a.args[0]
        count = a.args[1] if len(a.args) > 1 else 16
        # The component caps a scan at 64 registers per call.
        svc = "scan_registers"
        data = {"start": start, "count": count}
        secs = a.secs or max(6.0, count * 0.7)
    elif a.cmd == "write":
        reg, val = a.args[0], a.args[1]
        # Registers >= 200 are EEPROM with limited write endurance.
        warn = "  (EEPROM - limited write endurance!)" if reg >= 200 else ""
        if input(f"write register {reg} = {val} on {host}?{warn} [y/N] ").strip().lower() != "y":
            print("aborted")
            return 1
        svc, data, secs = "write_register", {"reg": reg, "value": val}, a.secs or 5
    elif a.cmd == "bit":
        svc, data, secs = "set_control_bit", {"bit": a.args[0], "value": bool(a.args[1])}, a.secs or 5
    else:
        svc, data = None, {}
        secs = a.secs or (a.args[0] if a.args else 30)

    return asyncio.run(run(host, svc, data, secs, a.quiet))


if __name__ == "__main__":
    sys.exit(main())
