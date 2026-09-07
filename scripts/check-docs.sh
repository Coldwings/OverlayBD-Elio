#!/usr/bin/env bash
# check-docs.sh — documentation governance floor checks.
#
# Two-tier enforcement (canonical statement: docs/README.md, "Enforcement
# tiers"):
#   hard gate (exit 1):   check 1 index<->file sync, check 2 documented test
#                         names exist, check 5 ADR integrity, check 6
#                         CANONICAL-COPY lockstep;
#   advisory (exit 0):    check 4 required sections per document shape,
#                         check 7 ADR back-references from current law.
# WARN (never counted):   relative .md links that do not resolve.
#
# This is a floor check: it verifies existence and synchronization, never
# meaning or quality — meaning stays with human/agent review.
#
# Known limitations (stated, not hidden):
# - Test-name convention: Catch2 TEST_CASE names are "<area>: <behavior>"
#   with area in {common, format, source, image, ublk, supervisor, cli,
#   integration}. Check 2 only flags backticked tokens matching that prefix
#   convention; a misspelled area prefix escapes the check.
# - No exported-symbol coverage check (the DART original's check 3): there is
#   no cheap C++ analogue of `go doc -all`. Section completeness (check 4) is
#   the floor instead.
# - Evidence anchors ("path::symbol") are verified only when written
#   (check 8, hard); the script never demands an anchor.
# - Heading matching is substring-based on `##`+ headings; exotic markdown
#   (setext headings) is not understood.

set -u
cd "$(dirname "$0")/.." || exit 1

fail=0      # hard-gate findings; alone decide the exit code
advisory=0  # advisory findings; reported, never block

advise() { echo "ADVISORY: $*"; advisory=$((advisory+1)); }
hard()   { echo "FAILED: $*" >&2; fail=$((fail+1)); }

# Per-check status line. Usage: snapshot `fb=$fail ab=$advisory` before the
# check body, then `status N "label" $fb $ab` after it.
status() { # $1=check number  $2=label  $3=fail-before  $4=advisory-before
  local df=$((fail - $3)) da=$((advisory - $4))
  if [ "$df" -gt 0 ]; then
    echo "FAILED: check$1: $2 ($df hard-gate failure(s))" >&2
  elif [ "$da" -gt 0 ]; then
    echo "advisory: check$1: $2 ($da note(s), non-blocking)"
  else
    echo "ok: check$1: $2"
  fi
}

# ---------------------------------------------------------------------------
# check 1 (hard): docs/README.md index <-> docs/*.md file sync
# ---------------------------------------------------------------------------
fb=$fail; ab=$advisory
index_links=$(sed -n 's/^|[^|]*](\.\?\/\?\([A-Za-z0-9_.\/-]*\.md\)).*$/\1/p' docs/README.md | sort -u)
for f in docs/*.md; do
  base=$(basename "$f")
  [ "$base" = "README.md" ] && continue
  rel="./$base"
  if ! echo "$index_links" | grep -qxF "$base"; then
    hard "check1: $f has no row in the docs/README.md index"
  fi
done
while IFS= read -r link; do
  [ -z "$link" ] && continue
  case "$link" in
    adr/*) target="docs/$link" ;;
    *)     target="docs/$link" ;;
  esac
  if [ ! -f "$target" ]; then
    hard "check1: index row points at missing file: $target"
  fi
done <<< "$index_links"
status 1 "docs index <-> file sync" $fb $ab

# ---------------------------------------------------------------------------
# check 2 (hard): test names cited in backticks in docs exist in tests/
# Convention: TEST_CASE("<area>: <behavior>"), area in the fixed list below.
# ---------------------------------------------------------------------------
fb=$fail; ab=$advisory
areas='common|format|source|image|ublk|supervisor|cli|integration'
test_names_file=$(mktemp)
grep -rhoE 'TEST_CASE\("[^"]+"' tests/ 2>/dev/null | sed 's/^TEST_CASE("//' | sort -u > "$test_names_file"
cited_file=$(mktemp)
grep -rhoE '`[^`]+`' docs/*.md docs/adr/*.md AGENTS.md README.md CONTRIBUTING.md 2>/dev/null \
  | sed 's/^`//; s/`$//' \
  | grep -E "^($areas): " | sort -u > "$cited_file"
while IFS= read -r name; do
  [ -z "$name" ] && continue
  if ! grep -qxF "$name" "$test_names_file"; then
    hard "check2: docs cite test name \`$name\` which does not exist in tests/"
  fi
done < "$cited_file"
rm -f "$test_names_file" "$cited_file"
status 2 "documented test names exist" $fb $ab

# ---------------------------------------------------------------------------
# check 4 (advisory): every docs/*.md carries the required sections of its shape
# The DOC-SHAPES block in docs/README.md is the machine-readable source.
# ---------------------------------------------------------------------------
fb=$fail; ab=$advisory
shapes_block=$(sed -n '/<!-- DOC-SHAPES/,/DOC-SHAPES -->/p' docs/README.md | grep -v '<!--')
get_shape_def() { # $1=shape name -> "; "-separated sections
  echo "$shapes_block" | sed -n "s/^$1: //p"
}
assignments=$(echo "$shapes_block" | sed -n 's/^assignments: //p')
aliases=$(echo "$shapes_block" | sed -n 's/^heading-aliases: //p')
shape_of() { # $1=docs path (docs/x.md)
  echo "$assignments" | tr ';' '\n' | sed -n "s|^[[:space:]]*$1=\([a-z]*\)[[:space:]]*$|\1|p" | head -1
}
# alias lookup: "A <=> B" means heading A is satisfied by B and vice versa
alias_of() { # $1=section name -> newline-separated alternatives
  echo "$aliases" | tr ';' '\n' | while IFS= read -r pair; do
    a=$(echo "$pair" | sed 's/[[:space:]]*<=>.*//; s/^[[:space:]]*//; s/[[:space:]]*$//')
    b=$(echo "$pair" | sed 's/.*<=>[[:space:]]*//; s/[[:space:]]*$//')
    [ -z "$a" ] && continue
    if [ "$a" = "$1" ]; then echo "$b"; fi
    if [ "$b" = "$1" ]; then echo "$a"; fi
  done
}
for f in docs/*.md; do
  base=$(basename "$f")
  [ "$base" = "README.md" ] && continue
  shape=$(shape_of "docs/$base")
  [ -z "$shape" ] && shape="module"
  [ "$shape" = "policy" ] && continue
  def=$(get_shape_def "$shape")
  [ -z "$def" ] && { hard "check4: shape '$shape' (docs/$base) has no definition in DOC-SHAPES"; continue; }
  headings=$(grep -E '^#{2,} ' "$f" | sed 's/^#*[[:space:]]*//')
  echo "$def" | tr ';' '\n' | while IFS= read -r sec; do
    sec=$(echo "$sec" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    [ -z "$sec" ] && continue
    case "$sec" in
      per-module*) continue ;;  # not machine-checkable by design
    esac
    found=0
    echo "$headings" | grep -qiF "$sec" && found=1
    if [ "$found" -eq 0 ]; then
      while IFS= read -r alt; do
        [ -z "$alt" ] && continue
        echo "$headings" | grep -qiF "$alt" && { found=1; break; }
      done <<< "$(alias_of "$sec")"
    fi
    [ "$found" -eq 0 ] && echo "$f|$sec"
  done
done > /tmp/checkdocs_sec.$$ 2>/dev/null
while IFS='|' read -r f sec; do
  [ -z "$f" ] && continue
  advise "check4: $f is missing required section '$sec' for its shape"
done < /tmp/checkdocs_sec.$$
rm -f /tmp/checkdocs_sec.$$
status 4 "required sections per shape" $fb $ab

# ---------------------------------------------------------------------------
# check 5 (hard): ADR integrity
#   - filenames NNNN-kebab.md, numbers unique (implicit), index coverage
#   - metadata: Status / Date / Supersedes / Binds present and well-formed
#   - supersession symmetric
#   - Binds paths exist
# ---------------------------------------------------------------------------
fb=$fail; ab=$advisory
adr_index=$(sed -n 's/^|[^|]*](\.\?\/\?\([0-9][0-9][0-9][0-9]-[A-Za-z0-9_.-]*\.md\)).*$/\1/p' docs/adr/README.md | sort -u)
for f in docs/adr/[0-9][0-9][0-9][0-9]-*.md; do
  [ -f "$f" ] || continue
  base=$(basename "$f")
  num=${base%%-*}
  echo "$adr_index" | grep -qxF "$base" || hard "check5: $base missing from docs/adr/README.md index"

  st=$(sed -n 's/^- Status:[[:space:]]*//p' "$f" | head -1)
  case "$st" in
    proposed|accepted|rejected) ;;
    "superseded by ADR-"[0-9][0-9][0-9][0-9]) ;;
    "") hard "check5: $base has no '- Status:' metadata line" ;;
    *)  hard "check5: $base has invalid Status '$st'" ;;
  esac

  dt=$(sed -n 's/^- Date:[[:space:]]*//p' "$f" | head -1)
  echo "$dt" | grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}' || hard "check5: $base Date line missing or malformed ('$dt')"

  sup=$(sed -n 's/^- Supersedes:[[:space:]]*//p' "$f" | head -1)
  if [ -z "$sup" ]; then
    hard "check5: $base has no '- Supersedes:' metadata line"
  fi

  binds=$(sed -n 's/^- Binds:[[:space:]]*//p' "$f" | head -1)
  if [ -z "$binds" ]; then
    hard "check5: $base has no '- Binds:' metadata line"
  else
    echo "$binds" | tr ',;' '\n\n' | while IFS= read -r item; do
      tok=$(echo "$item" | awk '{print $1}' | sed 's/[§#].*$//')
      case "$tok" in
        docs/*|src/*|tools/*|tests/*|scripts/*|cmake/*|*.md)
          [ -e "$tok" ] || echo "MISSING:$base:$tok" ;;
      esac
    done
  fi > /tmp/checkdocs_binds.$$ 2>/dev/null
  while IFS= read -r line; do
    case "$line" in
      MISSING:*) hard "check5: ${line#MISSING:} — Binds path does not exist (split on ':')" ;;
    esac
  done < /tmp/checkdocs_binds.$$
  rm -f /tmp/checkdocs_binds.$$
done
while IFS= read -r base; do
  [ -z "$base" ] && continue
  [ -f "docs/adr/$base" ] || hard "check5: index row points at missing ADR: docs/adr/$base"
done <<< "$adr_index"

# supersession symmetry
for f in docs/adr/[0-9][0-9][0-9][0-9]-*.md; do
  [ -f "$f" ] || continue
  base=$(basename "$f"); num=${base%%-*}
  st=$(sed -n 's/^- Status:[[:space:]]*//p' "$f" | head -1)
  sup=$(sed -n 's/^- Supersedes:[[:space:]]*//p' "$f" | head -1)
  case "$st" in
    "superseded by ADR-"*)
      other=${st#superseded by ADR-}
      of=$(ls docs/adr/"$other"-*.md 2>/dev/null | head -1)
      if [ -z "$of" ]; then
        hard "check5: $base claims supersession by ADR-$other but that file does not exist"
      else
        osup=$(sed -n 's/^- Supersedes:[[:space:]]*//p' "$of" | head -1)
        echo "$osup" | grep -q "ADR-$num" || hard "check5: ADR-$other does not list ADR-$num in Supersedes (asymmetric)"
      fi ;;
  esac
  case "$sup" in
    ADR-*|*ADR-*)
      for other in $(echo "$sup" | grep -oE 'ADR-[0-9]{4}' | sed 's/ADR-//' | sort -u); do
        [ "$other" = "$num" ] && continue
        of=$(ls docs/adr/"$other"-*.md 2>/dev/null | head -1)
        if [ -z "$of" ]; then
          hard "check5: $base Supersedes ADR-$other but that file does not exist"
        else
          ost=$(sed -n 's/^- Status:[[:space:]]*//p' "$of" | head -1)
          [ "$ost" = "superseded by ADR-$num" ] || hard "check5: ADR-$other status is '$ost', expected 'superseded by ADR-$num' (asymmetric)"
        fi
      done ;;
  esac
done
status 5 "ADR integrity" $fb $ab

# ---------------------------------------------------------------------------
# check 6 (hard): CANONICAL-COPY blocks byte-identical to their source
# (modulo leading indentation)
# ---------------------------------------------------------------------------
fb=$fail; ab=$advisory
if command -v python3 >/dev/null 2>&1; then
  find . -name '*.md' -not -path './third_party/*' -not -path './build/*' -print0 \
  | xargs -0 python3 scripts/_canonical_check.py 2>/dev/null > /tmp/checkdocs_canon.$$
  while IFS= read -r line; do
    case "$line" in
      MISMATCH:*) hard "check6: ${line#MISMATCH:}" ;;
    esac
  done < /tmp/checkdocs_canon.$$
  rm -f /tmp/checkdocs_canon.$$
else
  echo "WARN: python3 not found; check6 (canonical-copy lockstep) skipped"
fi
status 6 "canonical-copy lockstep" $fb $ab

# ---------------------------------------------------------------------------
# check 7 (advisory): every non-superseded ADR is cited by number from
# current law (docs/*.md outside docs/adr/, or AGENTS.md)
# ---------------------------------------------------------------------------
fb=$fail; ab=$advisory
law_files=$(ls docs/*.md AGENTS.md 2>/dev/null)
for f in docs/adr/[0-9][0-9][0-9][0-9]-*.md; do
  [ -f "$f" ] || continue
  base=$(basename "$f"); num=${base%%-*}
  st=$(sed -n 's/^- Status:[[:space:]]*//p' "$f" | head -1)
  case "$st" in "superseded by ADR-"*) continue ;; esac
  if ! grep -qF "ADR-$num" $law_files 2>/dev/null; then
    advise "check7: ADR-$num ($base) is not cited from any current-law document"
  fi
done
status 7 "ADR back-references" $fb $ab

# ---------------------------------------------------------------------------
# check 8 (hard): written evidence anchors "path::symbol" resolve
# Anchors are opt-in; this only validates the ones that exist.
# ---------------------------------------------------------------------------
fb=$fail; ab=$advisory
grep -rhoE '`[A-Za-z0-9_./-]+\.(hpp|cpp|h|c|md|sh|py)::[A-Za-z_][A-Za-z0-9_]*`' docs/*.md AGENTS.md 2>/dev/null \
  | sed 's/^`//; s/`$//' | sort -u | while IFS= read -r anchor; do
  path=${anchor%%::*}; sym=${anchor##*::}
  if [ ! -f "$path" ]; then
    echo "ANCHOR:$anchor:file $path does not exist"
  elif ! grep -qE "(class|struct|enum|constexpr|auto|void|int|bool|task|std::|using|template|[A-Za-z0-9_])[ &*<]*$sym\b" "$path"; then
    echo "ANCHOR:$anchor:symbol $sym not found in $path"
  fi
done > /tmp/checkdocs_anchor.$$ 2>/dev/null
while IFS= read -r line; do
  case "$line" in
    ANCHOR:*) hard "check8: evidence anchor ${line#ANCHOR:}" ;;
  esac
done < /tmp/checkdocs_anchor.$$
rm -f /tmp/checkdocs_anchor.$$
status 8 "evidence anchors resolve" $fb $ab

# ---------------------------------------------------------------------------
# WARN (never fails): relative markdown links resolve
# ---------------------------------------------------------------------------
for f in README.md CONTRIBUTING.md AGENTS.md docs/*.md docs/adr/*.md; do
  [ -f "$f" ] || continue
  dir=$(dirname "$f")
  grep -oE '\]\(([^)#]+\.md)\)' "$f" 2>/dev/null | sed 's/^](//; s/)$//' | while IFS= read -r link; do
    case "$link" in
      http*|/*) continue ;;
    esac
    [ -f "$dir/$link" ] || echo "WARN: $f links to missing $link"
  done
done

# ---------------------------------------------------------------------------
if [ "$fail" -gt 0 ]; then
  echo "check-docs: $fail hard-gate failure(s)" >&2
  [ "$advisory" -gt 0 ] && echo "check-docs: plus $advisory advisory note(s)" >&2
  exit 1
fi
if [ "$advisory" -gt 0 ]; then
  echo "check-docs: floor checks passed; $advisory advisory note(s) — non-blocking, each needs a disposition in the PR thread"
else
  echo "check-docs: all floor checks passed"
fi
exit 0
