#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/build/contrib/plugins/libia64-hob-write-trace.so}"
cc="${CC:-cc}"

command -v "$cc" >/dev/null 2>&1 || {
    echo "error: host C compiler is unavailable: $cc" >&2
    exit 2
}
command -v pkg-config >/dev/null 2>&1 || {
    echo "error: pkg-config is unavailable" >&2
    exit 2
}
pkg-config --exists glib-2.0 || {
    echo "error: glib-2.0 development files are unavailable" >&2
    exit 2
}

mkdir -p "$(dirname "$out")"
"$cc" \
    -std=gnu11 \
    -fPIC \
    -shared \
    -Wall \
    -Wextra \
    -Werror \
    -I"$root/include" \
    $(pkg-config --cflags glib-2.0) \
    "$root/contrib/plugins/ia64-hob-write-trace.c" \
    -o "$out" \
    $(pkg-config --libs glib-2.0)

test -s "$out"
printf 'built %s\n' "$out"
