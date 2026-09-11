#!/usr/bin/env python3
"""Fail if any CTest entry lacks an explicit positive finite TIMEOUT."""

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys


def timeout_violations(payload):
    """Return (missing, invalid) TIMEOUT violations from CTest JSON data."""
    missing = []
    invalid = []
    for test in payload.get("tests", []):
        name = test.get("name", "<unnamed>")
        props = {prop.get("name"): prop.get("value")
                 for prop in test.get("properties", [])}
        if "TIMEOUT" not in props:
            missing.append(name)
            continue
        try:
            timeout = float(props["TIMEOUT"])
        except (TypeError, ValueError):
            invalid.append((name, props["TIMEOUT"]))
            continue
        if not math.isfinite(timeout) or timeout <= 0:
            invalid.append((name, props["TIMEOUT"]))
    return missing, invalid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path,
                        help="CMake build directory to inspect")
    parser.add_argument("--ctest", default="ctest",
                        help="ctest executable, defaults to PATH lookup")
    args = parser.parse_args()

    result = subprocess.run(
        [args.ctest, "--test-dir", str(args.build_dir), "--show-only=json-v1"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode

    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        print(f"failed to parse ctest JSON: {exc}", file=sys.stderr)
        sys.stdout.write(result.stdout)
        return 1

    missing, invalid = timeout_violations(payload)
    if missing or invalid:
        for name in missing:
            print(f"{name}: missing explicit TIMEOUT")
        for name, value in invalid:
            print(f"{name}: invalid TIMEOUT value {value!r}")
        return 1

    print("Checked {} CTest entries: every test has explicit positive finite "
          "TIMEOUT".format(len(payload.get("tests", []))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
