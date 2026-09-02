#!/usr/bin/env bash
# surface-gate.sh — the binding-surface manifest gate.
#
# The engine publishes its full public-surface list (one row per public
# construct, radar-enforced) as scripts/bindings/surface.tsv at every
# release tag. This binding resolves EVERY line in docs/SURFACE.tsv:
# the binding's public API for the construct plus the test that proves it
# (a golden fixture line reference counts), or N/A + a reason when the
# binding deliberately does not expose it. That is the mechanical answer
# to "how do we know a binding isn't missing engine surface?".
#
# This gate fails when:
#   (a) docs/SURFACE.tsv does not cover every line of the engine list at
#       the PINNED engine tag (fetched from the raw URL at the pin), or
#       carries rows the engine no longer has;
#   (b) any binding-api or test cell is empty, or the exposure column
#       disagrees with the cells (MAPPED rows must not say N/A; N/A rows
#       must carry a non-empty reason in the test column);
#   (c) the N/A count differs from NA_BASELINE below — a new N/A is a
#       decision: add the row's reason string and raise the baseline in
#       the same commit (a drop likewise lowers it).
#
# Local development: CORVID_SURFACE_TSV=<path> overrides the fetch (the
# engine checkout's scripts/bindings/surface.tsv) so the gate can run
# offline; CI always fetches at the pin.
#
# Requirements: bash 3.2+, curl, awk. shellcheck-clean.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SURFACE="$ROOT/docs/SURFACE.tsv"
ENGINE_REPO="corvid-db/corvid"

# The committed N/A baseline for THIS binding (see (c) above).
NA_BASELINE=148

# ---- locate the engine pin ------------------------------------------------
pin=""
for cand in "$ROOT/fetch.sh" "$ROOT/Cargo.toml"; do
    if [ -f "$cand" ]; then
        p="$(awk -F'"' '
            /^CORVID_VERSION="/ { print $2; exit }
            /^corvid = \{ git = / && match($0, /tag = "v[0-9]+\.[0-9]+\.[0-9]+"/) {
                s = substr($0, RSTART, RLENGTH); sub(/^tag = "/, "", s); sub(/"$/, "", s); print s; exit
            }' "$cand")"
        if [ -n "$p" ]; then pin="$p"; break; fi
    fi
done
[ -n "$pin" ] || { echo "surface-gate: cannot find the engine pin (CORVID_VERSION in fetch.sh or tag = in Cargo.toml)" >&2; exit 1; }

# fetch.sh and fetch.ps1 must agree on the pin (same rule as bump.sh).
if [ -f "$ROOT/fetch.sh" ] && [ -f "$ROOT/fetch.ps1" ]; then
    ps1_pin="$(awk -F'"' '/\$CorvidVersion = "v[0-9]+\.[0-9]+\.[0-9]+"/ { print $2; exit }' "$ROOT/fetch.ps1")"
    if [ -n "$ps1_pin" ] && [ "$ps1_pin" != "$pin" ]; then
        echo "surface-gate: engine pins disagree: fetch.sh=$pin fetch.ps1=$ps1_pin" >&2
        exit 1
    fi
fi

# ---- fetch the engine list at the pin --------------------------------------
ENGINE_TSV="${CORVID_SURFACE_TSV:-}"
if [ -z "$ENGINE_TSV" ]; then
    ENGINE_TSV="$(mktemp "${TMPDIR:-/tmp}/surface-engine.tsv.XXXXXX")"
    trap 'rm -f "$ENGINE_TSV"' EXIT
    url="https://raw.githubusercontent.com/${ENGINE_REPO}/${pin}/scripts/bindings/surface.tsv"
    echo "surface-gate: engine pin $pin -> $url"
    curl -fsSL -o "$ENGINE_TSV" "$url" || {
        echo "surface-gate: cannot fetch the engine surface list at pin $pin" >&2
        echo "  (a tag predating scripts/bindings/surface.tsv 404s — bump the engine pin)" >&2
        exit 1
    }
fi

[ -f "$SURFACE" ] || { echo "surface-gate: $SURFACE is missing" >&2; exit 1; }

status=0
fail() { echo "surface-gate: FAIL: $*" >&2; status=1; }

# ---- (a) coverage: every engine line resolved, no stale rows ---------------
engine_items="$(awk -F'\t' 'NF==3 { print $1 }' "$ENGINE_TSV" | sort)"
ours_items="$(awk -F'\t' 'NF==5 { print $1 }' "$SURFACE" | sort)"
missing="$(comm -23 <(echo "$engine_items") <(echo "$ours_items"))"
stale="$(comm -13 <(echo "$engine_items") <(echo "$ours_items"))"
dupes="$(awk -F'\t' 'NF==5 { print $1 }' "$SURFACE" | sort | uniq -d)"

list_lines() { while IFS= read -r l; do printf '      %s\n' "$l"; done; }
[ -n "$missing" ] && {
    fail "engine constructs with NO row in docs/SURFACE.tsv (map them or record N/A + reason):"
    echo "$missing" | list_lines >&2
}
[ -n "$stale" ] && {
    fail "rows in docs/SURFACE.tsv the pinned engine list no longer has:"
    echo "$stale" | list_lines >&2
}
[ -n "$dupes" ] && fail "duplicate rows: $(echo "$dupes" | tr '\n' ' ')"

# class column must match the engine row verbatim (a reclassified construct
# is a signal to re-examine the mapping).
join -t "$(printf '\t')" \
    <(awk -F'\t' 'NF==3 { print $1 "\t" $2 }' "$ENGINE_TSV" | sort) \
    <(awk -F'\t' 'NF==5 { print $1 "\t" $2 }' "$SURFACE" | sort) |
    awk -F'\t' '$2 != $3 { print "      " $1 " (engine class: " $2 ", SURFACE.tsv: " $3 ")" }' > /tmp/sg-class.$$
[ -s /tmp/sg-class.$$ ] && { fail "class column drifted from the engine list:"; cat /tmp/sg-class.$$ >&2; }
rm -f /tmp/sg-class.$$

# ---- (b) no empty cells; exposure consistent --------------------------------
awk -F'\t' '
    NF != 5 && NF > 0 { print "      line " NR ": " NF " columns (need 5)"; bad = 1 }
    NF == 5 {
        if ($3 != "MAPPED" && $3 != "N/A") { print "      line " NR ": exposure must be MAPPED or N/A (got \"" $3 "\")"; bad = 1 }
        if ($4 == "") { print "      line " NR ": empty binding-api cell (" $1 ")"; bad = 1 }
        if ($5 == "") { print "      line " NR ": empty test cell (" $1 ")"; bad = 1 }
        if ($3 == "N/A" && $4 != "N/A") { print "      line " NR ": N/A row whose binding-api is not N/A (" $1 ")"; bad = 1 }
        if ($3 == "MAPPED" && ($4 == "N/A" || $4 == "")) { print "      line " NR ": MAPPED row with no binding api (" $1 ")"; bad = 1 }
        if ($3 == "N/A" && length($5) < 10) { print "      line " NR ": N/A reason too thin to be a reason (" $1 ")"; bad = 1 }
    }
    END { exit bad ? 1 : 0 }
' "$SURFACE" || fail "malformed rows in docs/SURFACE.tsv (see above)"

# ---- (c) N/A baseline --------------------------------------------------------
na_count="$(awk -F'\t' '$3 == "N/A"' "$SURFACE" | wc -l | tr -d ' ')"
if [ "$na_count" -ne "$NA_BASELINE" ]; then
    fail "N/A count $na_count != baseline $NA_BASELINE."
    echo "  A new N/A is a decision: give the row a reason string and raise" >&2
    echo "  NA_BASELINE in scripts/surface-gate.sh in the same commit (and" >&2
    echo "  vice versa when an N/A becomes a mapping). Engine constructs at" >&2
    echo "  this pin: $(echo "$engine_items" | wc -l | tr -d ' ')." >&2
fi

# ---- verdict -----------------------------------------------------------------
if [ "$status" -ne 0 ]; then
    echo "surface-gate: FAILED (see above)" >&2
    exit 1
fi
mapped="$(awk -F'\t' '$3 == "MAPPED"' "$SURFACE" | wc -l | tr -d ' ')"
echo "surface-gate: ok — pin $pin: $(echo "$engine_items" | wc -l | tr -d ' ') engine constructs, $mapped mapped, $na_count N/A (baseline $NA_BASELINE)"
