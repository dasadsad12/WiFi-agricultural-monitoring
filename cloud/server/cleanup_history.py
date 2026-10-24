#!/usr/bin/env python3
"""Bounded cleanup for this partition only.

Keep a bounded recent window instead of deleting the whole report history.
The relay itself keeps the latest 300 samples in memory; the on-disk window is
larger so a service restart can rebuild useful charts and statistics.
"""
import argparse
from collections import deque
from pathlib import Path


DEFAULT_MAX_LINES = 5000


def cleanup(data_dir, dry_run=False, max_lines=DEFAULT_MAX_LINES):
    root = Path(data_dir).resolve()
    target = root / "telemetry.jsonl"
    if not target.exists():
        return
    try:
        max_lines = max(1, int(max_lines))
    except (TypeError, ValueError):
        max_lines = DEFAULT_MAX_LINES
    try:
        with target.open("r", encoding="utf-8") as stream:
            recent = deque(stream, maxlen=max_lines)
    except OSError:
        return
    if dry_run:
        print("would retain {} lines in {}".format(len(recent), target))
        return
    temporary = target.with_suffix(".jsonl.tmp")
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            stream.writelines(recent)
        temporary.replace(target)
        rotated = root / "telemetry.jsonl.1"
        if rotated.exists():
            rotated.unlink()
        print("retained {} lines in {}".format(len(recent), target))
    except OSError:
        try:
            if temporary.exists():
                temporary.unlink()
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", required=True)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--max-lines", type=int, default=DEFAULT_MAX_LINES)
    args = parser.parse_args()
    cleanup(args.data_dir, args.dry_run, args.max_lines)


if __name__ == "__main__":
    main()
