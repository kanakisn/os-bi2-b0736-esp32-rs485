#!/usr/bin/env python3
"""Sample a set of registers across every node on a fixed cadence.

Holds one API connection per node for the whole run, so each round is cheap.
Built to tell a live derived value apart from a stored constant: watch a
register next to ambient (reg 0) and see whether it tracks or stays pinned.

  sample.py               # 14 rounds, 50s apart
  sample.py 30 60         # 30 rounds, 60s apart
  sample.py 10 30 117 9   # watch regs 117 and 9 (reg 0 is always included)
"""
import asyncio
import re
import sys
from datetime import datetime

from aioesphomeapi import APIClient, LogLevel

from osreg import NODES, signed

REG_LINE = re.compile(rb"REG (\d+) \(0x[0-9A-Fa-f]{4}\) = (\d+)")


async def main() -> None:
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 14
    period = float(sys.argv[2]) if len(sys.argv) > 2 else 50.0
    extra = [int(a) for a in sys.argv[3:]] or [117, 9]
    watch = [0] + [r for r in extra if r != 0]

    clients: dict[str, tuple] = {}
    latest: dict[str, dict[int, int]] = {}

    for name, host in NODES.items():
        cli = APIClient(host, 6053, None)
        await cli.connect(login=True)
        latest[name] = {}
        _, services = await cli.list_entities_services()
        svc = {s.name: s for s in services}

        def make_cb(node: str):
            def cb(msg) -> None:
                m = REG_LINE.search(msg.message)
                if m:
                    latest[node][int(m.group(1))] = int(m.group(2))
            return cb

        cli.subscribe_logs(make_cb(name), log_level=LogLevel.LOG_LEVEL_INFO)
        clients[name] = (cli, svc)

    cols = "".join(f"{('r' + str(r)):>7}" for r in watch)
    print(f"{'time':8} {'node':7}{cols}   notes", flush=True)

    try:
        for rnd in range(rounds):
            for cli, svc in clients.values():
                for reg in watch:
                    await cli.execute_service(svc["read_register"], {"reg": reg})
                    await asyncio.sleep(0.35)
            await asyncio.sleep(2.0)

            for name in clients:
                d = latest[name]
                vals = "".join(f"{d.get(r, '-')!s:>7}" for r in watch)
                notes = []
                for r in watch:
                    if r == 0 or r not in d:
                        continue
                    s = signed(d[r])
                    if s < 0:
                        notes.append(f"r{r}={s}")
                        if 0 in d:
                            notes.append(f"amb-r{r}={d[0] - s}")
                print(f"{datetime.now():%H:%M:%S} {name:7}{vals}   {' '.join(notes)}", flush=True)
            print("", flush=True)

            if rnd < rounds - 1:
                await asyncio.sleep(period)
    finally:
        for cli, _ in clients.values():
            await cli.disconnect()


asyncio.run(main())
