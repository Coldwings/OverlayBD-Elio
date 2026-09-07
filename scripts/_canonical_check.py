#!/usr/bin/env python3
"""Canonical-copy lockstep checker (check 6 of scripts/check-docs.sh).

Scans the markdown files given on argv for `<!-- CANONICAL id="X" -->` blocks
and `<!-- CANONICAL-COPY source="path" id="X" -->` blocks. A copy must be
byte-identical to its source block modulo leading indentation on each line.

Output: one `MISMATCH:<file>:<reason>` line per finding; exit status is
always 0 (the calling shell script counts findings).
"""
import os
import re
import sys

CANON_RE = re.compile(
    r'<!-- CANONICAL id="([^"]+)" -->(.*?)<!-- /CANONICAL id="\1" -->', re.S)
COPY_RE = re.compile(
    r'<!-- CANONICAL-COPY source="([^"]+)" id="([^"]+)" -->'
    r'(.*?)'
    r'<!-- /CANONICAL-COPY source="\1" id="\2" -->', re.S)


def strip_indent(body: str) -> str:
    return "".join(line.lstrip() for line in body.splitlines(keepends=True))


def main() -> int:
    sources = {}   # (path, id) -> stripped body
    copies = []    # (file, source, id, stripped body)
    for path in sys.argv[1:]:
        try:
            text = open(path, encoding="utf-8").read()
        except OSError:
            continue
        for m in CANON_RE.finditer(text):
            sources[(path, m.group(1))] = strip_indent(m.group(2))
        for m in COPY_RE.finditer(text):
            copies.append((path, m.group(1), m.group(2), strip_indent(m.group(3))))
    for path, source, cid, body in copies:
        src_norm = os.path.normpath(os.path.join(os.path.dirname(path), source))
        key = (src_norm, cid)
        if key not in sources:
            key = (source, cid)
        if key not in sources:
            print(f"MISMATCH:{path}: canonical source block "
                  f"'{source}::{cid}' not found")
        elif sources[key] != body:
            print(f"MISMATCH:{path}: CANONICAL-COPY of '{source}::{cid}' "
                  f"differs from its source (edit both or neither)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
