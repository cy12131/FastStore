#!/usr/bin/env python3
"""Reject failed warm-up/client outputs before recording a measurement."""

import argparse
from pathlib import Path
import re


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", choices=("wrk", "hey"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    text = args.output.read_text(encoding="utf-8", errors="replace")

    if args.tool == "hey":
        if "Error distribution:" in text:
            raise ValueError("hey reported request errors")
        statuses = re.findall(
            r"^\s*\[([0-9]+)\]\s+([0-9]+) responses$",
            text,
            re.MULTILINE,
        )
        if not statuses or any(status != "200" for status, _ in statuses):
            raise ValueError("hey reported missing or non-200 responses")
        if sum(int(count) for _, count in statuses) == 0:
            raise ValueError("hey completed no responses")
        return 0

    socket_errors = re.search(
        r"Socket errors: connect ([0-9]+), read ([0-9]+), "
        r"write ([0-9]+), timeout ([0-9]+)",
        text,
    )
    if socket_errors and any(int(value) != 0 for value in socket_errors.groups()):
        raise ValueError("wrk reported socket errors")
    non_success = re.search(r"Non-2xx or 3xx responses:\s*([0-9]+)", text)
    if non_success and int(non_success.group(1)) != 0:
        raise ValueError("wrk reported non-success responses")
    completed = re.search(r"^FASTSTORE_REQUESTS=([0-9]+)$", text, re.MULTILINE)
    if not completed or int(completed.group(1)) == 0:
        raise ValueError("wrk completed no responses")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
