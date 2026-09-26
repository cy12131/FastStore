#!/usr/bin/env python3
"""Aggregate successful repeats while retaining raw_results.csv unchanged."""

import argparse
import csv
from collections import defaultdict
from pathlib import Path


NUMERIC_COLUMNS = [
    "requests_per_sec",
    "mib_per_sec",
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "avg_cpu_percent",
    "avg_rss_kib",
]


def mean(rows: list[dict[str, str]], name: str) -> float | None:
    values = [float(row[name]) for row in rows if row[name] != ""]
    return sum(values) / len(values) if values else None


def display(value: object, precision: int) -> str:
    if value is None:
        return "-"
    return f"{float(value):.{precision}f}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    args = parser.parse_args()

    with args.raw.open(newline="", encoding="utf-8") as source:
        raw_rows = list(csv.DictReader(source))
    if not raw_rows:
        raise ValueError("raw results contain no successful runs")

    groups: dict[tuple[str, int], list[dict[str, str]]] = defaultdict(list)
    for row in raw_rows:
        groups[(row["workload"], int(row["concurrency"]))].append(row)

    summary_rows = []
    for (workload, concurrency), rows in sorted(groups.items()):
        summary = {
            "workload": workload,
            "concurrency": concurrency,
            "successful_repeats": len(rows),
        }
        for name in NUMERIC_COLUMNS:
            summary["mean_" + name] = mean(rows, name)
        summary["max_peak_rss_kib"] = max(
            int(row["peak_rss_kib"]) for row in rows
        )
        summary_rows.append(summary)

    fieldnames = list(summary_rows[0].keys())
    with args.csv.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(summary_rows)

    with args.markdown.open("w", encoding="utf-8") as output:
        output.write(
            "| workload | c | repeats | req/s | MiB/s | P50 ms | P95 ms | "
            "P99 ms | CPU % | avg RSS KiB | peak RSS KiB |\n"
        )
        output.write("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for row in summary_rows:
            output.write(
                f"| {row['workload']} | {row['concurrency']} | "
                f"{row['successful_repeats']} | "
                f"{display(row['mean_requests_per_sec'], 2)} | "
                f"{display(row['mean_mib_per_sec'], 2)} | "
                f"{display(row['mean_p50_ms'], 3)} | "
                f"{display(row['mean_p95_ms'], 3)} | "
                f"{display(row['mean_p99_ms'], 3)} | "
                f"{display(row['mean_avg_cpu_percent'], 2)} | "
                f"{display(row['mean_avg_rss_kib'], 1)} | "
                f"{row['max_peak_rss_kib']} |\n"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
