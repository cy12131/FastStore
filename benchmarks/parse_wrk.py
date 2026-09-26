#!/usr/bin/env python3
"""Parse one successful wrk run and append a consistent raw-results row."""

import argparse
import csv
from datetime import datetime, timezone
from pathlib import Path
import re


TRANSFER_UNITS = {
    "B": 1.0,
    "KB": 1024.0,
    "MB": 1024.0**2,
    "GB": 1024.0**3,
    "TB": 1024.0**4,
}


def marker(text: str, name: str) -> float:
    match = re.search(rf"^{re.escape(name)}=([0-9.]+)$", text, re.MULTILINE)
    if not match:
        raise ValueError(f"missing {name} marker")
    return float(match.group(1))


def parse_transfer(text: str) -> tuple[str, float]:
    match = re.search(r"^Transfer/sec:\s*([0-9.]+)([KMGTP]?B)$", text, re.MULTILINE)
    if not match:
        raise ValueError("missing Transfer/sec")
    raw = match.group(1) + match.group(2)
    return raw, float(match.group(1)) * TRANSFER_UNITS[match.group(2)]


def parse_samples(path: Path) -> tuple[float, float, int]:
    with path.open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError("resource sampler produced no rows")
    cpu = [float(row["cpu_percent"]) for row in rows]
    rss = [int(row["rss_kib"]) for row in rows]
    return sum(cpu) / len(cpu), sum(rss) / len(rss), max(rss)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wrk-output", type=Path, required=True)
    parser.add_argument("--samples", type=Path, required=True)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--workload", required=True)
    parser.add_argument("--concurrency", type=int, required=True)
    parser.add_argument("--repeat", type=int, required=True)
    parser.add_argument("--duration", type=int, required=True)
    args = parser.parse_args()

    text = args.wrk_output.read_text(encoding="utf-8", errors="replace")
    requests_match = re.search(
        r"^Requests/sec:\s*([0-9.]+)$", text, re.MULTILINE
    )
    if not requests_match:
        raise ValueError("missing Requests/sec")
    if re.search(r"Socket errors:.*(?:connect [1-9]|read [1-9]|write [1-9]|timeout [1-9])", text):
        raise ValueError("wrk reported socket errors")
    non_success = re.search(r"Non-2xx or 3xx responses:\s*([0-9]+)", text)
    if non_success and int(non_success.group(1)) != 0:
        raise ValueError("wrk reported non-success responses")

    transfer_raw, transfer_bytes = parse_transfer(text)
    average_cpu, average_rss, peak_rss = parse_samples(args.samples)
    row = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "workload": args.workload,
        "concurrency": args.concurrency,
        "repeat": args.repeat,
        "duration_seconds": args.duration,
        "requests_per_sec": float(requests_match.group(1)),
        "transfer_per_sec_raw": transfer_raw,
        "transfer_bytes_per_sec": transfer_bytes,
        # Transfer/sec is preserved for every wrk run. MiB/sec is reported
        # only for GET because PUT's value represents response-direction bytes.
        "mib_per_sec": (
            transfer_bytes / (1024.0**2)
            if args.workload.startswith("get-") else ""
        ),
        "p50_ms": marker(text, "FASTSTORE_P50_US") / 1000.0,
        "p95_ms": marker(text, "FASTSTORE_P95_US") / 1000.0,
        "p99_ms": marker(text, "FASTSTORE_P99_US") / 1000.0,
        "avg_cpu_percent": average_cpu,
        "avg_rss_kib": average_rss,
        "peak_rss_kib": peak_rss,
    }

    fieldnames = list(row.keys())
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    new_file = not args.csv.exists()
    with args.csv.open("a", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        if new_file:
            writer.writeheader()
        writer.writerow(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
