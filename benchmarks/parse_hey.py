#!/usr/bin/env python3
"""Parse one successful hey HEAD run into raw_results.csv."""

import argparse
import csv
from datetime import datetime, timezone
from pathlib import Path
import re


def required(text: str, pattern: str, description: str) -> re.Match[str]:
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        raise ValueError(f"missing {description}")
    return match


def percentile_ms(text: str, percentile: int) -> float:
    seconds = float(required(
        text,
        rf"^\s*{percentile}% in ([0-9.]+) secs$",
        f"P{percentile}",
    ).group(1))
    return seconds * 1000.0


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
    parser.add_argument("--hey-output", type=Path, required=True)
    parser.add_argument("--samples", type=Path, required=True)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--workload", required=True)
    parser.add_argument("--concurrency", type=int, required=True)
    parser.add_argument("--repeat", type=int, required=True)
    parser.add_argument("--duration", type=int, required=True)
    args = parser.parse_args()

    text = args.hey_output.read_text(encoding="utf-8", errors="replace")
    if "Error distribution:" in text:
        raise ValueError("hey reported request errors")
    statuses = re.findall(r"^\s*\[([0-9]+)\]\s+([0-9]+) responses$", text, re.MULTILINE)
    if not statuses or any(status != "200" for status, _ in statuses):
        raise ValueError("hey reported missing or non-200 responses")
    response_count = sum(int(count) for _, count in statuses)
    if response_count == 0:
        raise ValueError("hey completed no responses")

    requests_per_sec = float(required(
        text, r"^\s*Requests/sec:\s*([0-9.]+)$", "requests/sec"
    ).group(1))
    average_cpu, average_rss, peak_rss = parse_samples(args.samples)

    row = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "workload": args.workload,
        "concurrency": args.concurrency,
        "repeat": args.repeat,
        "duration_seconds": args.duration,
        "requests_per_sec": requests_per_sec,
        # hey counts HEAD Content-Length as data even though no body is sent,
        # so transfer fields are intentionally empty rather than misleading.
        "transfer_per_sec_raw": "",
        "transfer_bytes_per_sec": "",
        "mib_per_sec": "",
        "p50_ms": percentile_ms(text, 50),
        "p95_ms": percentile_ms(text, 95),
        "p99_ms": percentile_ms(text, 99),
        "avg_cpu_percent": average_cpu,
        "avg_rss_kib": average_rss,
        "peak_rss_kib": peak_rss,
    }

    fieldnames = list(row.keys())
    new_file = not args.csv.exists()
    with args.csv.open("a", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        if new_file:
            writer.writeheader()
        writer.writerow(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
