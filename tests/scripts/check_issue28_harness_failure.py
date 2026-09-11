#!/usr/bin/env python3
"""Run the hidden issue #28 failure probe and require a clean Catch2 failure."""

from __future__ import annotations

import argparse
import subprocess
import sys


FAILING = "integration: issue28 hidden cleanup probe preserves assertion failure"
SIBLING = "integration: issue28 hidden sibling runs after failed cleanup probe"
MARKER = "issue28 intentional failure marker"
SIBLING_MARKER = "issue28 sibling executed after cleanup probe"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="path to obd_integration_tests")
    args = parser.parse_args()

    cmd = [args.binary, "[issue28-harness]", "-s"]
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=90,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        print(output, end="")
        print("issue28 harness probe timed out", file=sys.stderr)
        return 1

    print(proc.stdout, end="")
    if proc.returncode == 0:
        print("hidden issue28 probe unexpectedly passed", file=sys.stderr)
        return 1
    if proc.returncode < 0:
        print(
            f"hidden issue28 probe terminated by signal {-proc.returncode}",
            file=sys.stderr,
        )
        return 1
    if proc.returncode == 128 + 11:
        print("hidden issue28 probe segfaulted", file=sys.stderr)
        return 1

    required = [FAILING, SIBLING, MARKER, SIBLING_MARKER, "REQUIRE( false )"]
    missing = [needle for needle in required if needle not in proc.stdout]
    if missing:
        print(
            "hidden issue28 probe missed expected output: " + ", ".join(missing),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
