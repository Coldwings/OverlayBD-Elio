#!/usr/bin/env python3
"""Reject co_await tokens inside Catch runtime assertion arguments.

This is a lexical source check, not a C++ parser or preprocessor. See
docs/testing.md for the intentionally conservative scope.
"""

import argparse
from pathlib import Path
import re


ASSERTIONS = {
    prefix + suffix
    for prefix in ("REQUIRE", "CHECK")
    for suffix in ("", "_FALSE", "_THROWS", "_THROWS_AS", "_THROWS_WITH",
                   "_THROWS_MATCHES", "_NOTHROW", "_THAT")
}
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx", ".h"}

# Consume comments/literals whole, so their parentheses and keywords cannot
# affect argument nesting. Numeric separators (1'000) are not character quotes.
TOKENS = re.compile(
    r'(?P<ignored>//[^\n]*|/\*[\s\S]*?\*/|'
    r'(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)"|'
    r'(?:u8|u|U|L)?"(?:\\[\s\S]|[^"\\])*"|'
    r"(?:u8|u|U|L)?'(?:\\[\s\S]|[^'\\])*'|"
    r"(?:[0-9]|\.[0-9])[A-Za-z0-9_'.]*)|"
    r'(?P<identifier>[A-Za-z_][A-Za-z_0-9]*)|(?P<punctuation>[^\s])'
)


def violations(source):
    """Return (assertion name, starting line) once per offending invocation."""
    depth = 0
    active = []
    found = set()
    previous = ""
    previous_line = 1
    line = 1
    position = 0
    for token in TOKENS.finditer(source):
        line += source.count("\n", position, token.start())
        position = token.start()
        if token.lastgroup == "ignored":
            continue
        word = token.group()
        if word == "(":
            depth += 1
            if previous in ASSERTIONS:
                active.append((depth, previous, previous_line))
        elif word == ")":
            if active and active[-1][0] == depth:
                active.pop()
            depth -= 1
        elif word == "co_await":
            found.update((name, start) for _, name, start in active)
        previous, previous_line = word, line
    return sorted(found, key=lambda item: (item[1], item[0]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path,
                        help="C++ test files or test source directories")
    args = parser.parse_args()
    files = set()
    for path in args.paths:
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            files.add(path)
        elif path.is_dir():
            files.update(p for p in path.rglob("*")
                         if p.is_file() and p.suffix in SOURCE_SUFFIXES)
        else:
            parser.error("not a C++ source file or directory: {}".format(path))
    if not files:
        parser.error("no C++ test sources found")
    failed = False
    for path in sorted(files):
        for name, line in violations(path.read_text(encoding="utf-8")):
            print("{}:{}: co_await inside {}; await into a named local first"
                  .format(path, line, name))
            failed = True
    if not failed:
        print("Checked {} C++ test sources: no awaits in Catch assertions"
              .format(len(files)))
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
