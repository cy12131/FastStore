#!/usr/bin/env python3
"""Sample FastStore process CPU and RSS from /proc during one wrk window."""

import argparse
import csv
import os
from pathlib import Path
import time


def read_process_ticks(pid: int) -> int:
    fields = Path(f"/proc/{pid}/stat").read_text(encoding="ascii").split()
    return int(fields[13]) + int(fields[14])


def read_rss_kib(pid: int) -> int:
    for line in Path(f"/proc/{pid}/status").read_text(
        encoding="ascii"
    ).splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    raise RuntimeError("VmRSS is missing")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--duration", type=float, required=True)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if args.duration <= 0 or args.interval <= 0:
        parser.error("duration and interval must be positive")

    clock_ticks = os.sysconf("SC_CLK_TCK")
    deadline = time.monotonic() + args.duration
    previous_time = time.monotonic()
    previous_ticks = read_process_ticks(args.pid)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["timestamp", "cpu_percent", "rss_kib"])

        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(args.interval, remaining))

            now = time.monotonic()
            ticks = read_process_ticks(args.pid)
            elapsed = now - previous_time
            cpu_percent = ((ticks - previous_ticks) / clock_ticks) / elapsed * 100.0
            writer.writerow([
                f"{time.time():.6f}",
                f"{cpu_percent:.3f}",
                read_rss_kib(args.pid),
            ])
            output.flush()
            previous_time = now
            previous_ticks = ticks

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
