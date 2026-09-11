#!/usr/bin/env bash
set -euo pipefail

src="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$src/out}"
prefix="${IA64_CROSS_PREFIX:-ia64-linux-gnu-}"
cc="${CC:-${prefix}gcc}"
objcopy="${OBJCOPY:-${prefix}objcopy}"
readelf="${READELF:-${prefix}readelf}"

for tool in git python3 "$cc" "$objcopy" "$readelf"; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: required tool is unavailable: $tool" >&2
        exit 2
    }
done

rm -rf "$out"
mkdir -p "$out"

git clone --depth 1 https://github.com/PicoEFI/PicoEFI.git "$out/picoefi"
mkdir -p "$out/picoefi/inc/efi/ia64"
cp "$src/efibind.h" "$out/picoefi/inc/efi/ia64/efibind.h"

python3 - "$out/picoefi/inc/efi.h" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
needle = '''#elif defined (__riscv) && __riscv_xlen == 64
#include "efi/riscv64/efibind.h"
'''
replacement = needle + '''#elif defined (__ia64__)
#include "efi/ia64/efibind.h"
'''
if text.count(needle) != 1:
    raise SystemExit("PicoEFI architecture selector changed unexpectedly")
path.write_text(text.replace(needle, replacement, 1))
PY

common=(
    -ffreestanding
    -fshort-wchar
    -fno-stack-protector
    -fno-strict-aliasing
    -fno-lto
    -fPIC
    -mfixed-range=f32-f127
    -Wall
    -Wextra
    -Werror
    -I"$out/picoefi/inc"
)

"$cc" "${common[@]}" -c "$src/entry.S" -o "$out/entry.o"
"$cc" "${common[@]}" -c "$src/reloc.S" -o "$out/reloc.o"
"$cc" "${common[@]}" -c "$src/main.c" -o "$out/main.o"

"$cc" \
    -nostdlib \
    -nostartfiles \
    -ffreestanding \
    -fPIC \
    -fno-lto \
    -mfixed-range=f32-f127 \
    -Wl,-pie \
    -Wl,-Bsymbolic \
    -Wl,-znocombreloc \
    -Wl,--gc-sections \
    -Wl,--build-id=none \
    -Wl,-Map="$out/rooster-ia64-efi.map" \
    -T "$src/link_script.lds" \
    "$out/entry.o" "$out/reloc.o" "$out/main.o" \
    -o "$out/rooster-ia64-efi.elf"

"$objcopy" -O binary \
    "$out/rooster-ia64-efi.elf" "$out/BOOTIA64.EFI"
truncate -s "$(( (($(wc -c < "$out/BOOTIA64.EFI") + 4095) / 4096) * 4096 ))" \
    "$out/BOOTIA64.EFI"

"$readelf" -h -l -S -r -d "$out/rooster-ia64-efi.elf" \
    > "$out/readelf.txt"
python3 "$src/verify.py" "$out/BOOTIA64.EFI"

printf 'built %s\n' "$out/BOOTIA64.EFI"
