#!/usr/bin/env bash
# build-ext.sh — phpize-configure-make the extension against deps/current.
#
# The unix build shape every consumer (CI, contributors, PIE/PECL down
# the line) uses: fetch.sh must have run first. Prints nothing on
# success beyond the toolchain's own output; leaves the loadable module
# at ext/corvid/modules/corvid.so and echoes its path.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/ext/corvid"
DEPS="$ROOT/deps/current"

if [ ! -f "$DEPS/corvid.h" ]; then
    echo "build-ext: $DEPS/corvid.h missing — run ./fetch.sh first" >&2
    exit 1
fi
command -v phpize >/dev/null 2>&1 || {
    echo "build-ext: phpize not found (install the PHP dev headers, e.g. php8.4-dev / php-devel)" >&2
    exit 1
}

cd "$EXT"

# Re-running is fine: phpize/configure are idempotent enough, and a
# clean configure avoids stale flags after a pin bump.
if [ ! -f Makefile ]; then
    phpize >/dev/null
    ./configure --with-corvid="$DEPS" >/dev/null
fi

make -s -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" >/dev/null

[ -f modules/corvid.so ] || [ -f modules/corvid.dylib ] || {
    echo "build-ext: no module produced in $EXT/modules" >&2
    exit 1
}
echo "$EXT/modules/corvid.so"
