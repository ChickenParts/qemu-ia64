#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/build-ia64-initramfs.sh [outdir]

Builds a minimal IA-64 initramfs that provides /init (no libc) and basic /dev
nodes. Output is written to: outdir/initramfs.cpio.gz

Environment overrides:
  IA64_AS            (default: ia64-suse-linux-as)
  IA64_LD            (default: ia64-suse-linux-ld)
  IA64_STRIP         (default: ia64-suse-linux-strip)
  IA64_GEN_INIT_CPIO (default: stuff/linux-ia64/usr/gen_init_cpio)
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

outdir="${1:-scratch/ia64_initramfs}"
as_bin="${IA64_AS:-ia64-suse-linux-as}"
ld_bin="${IA64_LD:-ia64-suse-linux-ld}"
strip_bin="${IA64_STRIP:-ia64-suse-linux-strip}"
gen_cpio="${IA64_GEN_INIT_CPIO:-stuff/linux-ia64/usr/gen_init_cpio}"

src="scripts/ia64-initramfs/init.S"
obj="$outdir/init.o"
init="$outdir/init"
list="$outdir/initramfs.list"
cpio="$outdir/initramfs.cpio"
gz="$outdir/initramfs.cpio.gz"

mkdir -p "$outdir"

"$as_bin" -o "$obj" "$src"
"$ld_bin" -static -nostdlib -e _start -o "$init" "$obj"
"$strip_bin" -s "$init" 2>/dev/null || true

cat >"$list" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/kmsg 0644 0 0 c 1 11
nod /dev/null 0666 0 0 c 1 3
dir /proc 0755 0 0
dir /sys 0755 0 0
file /init $init 0755 0 0
EOF

"$gen_cpio" -o "$cpio" "$list"
gzip -9 -c "$cpio" >"$gz"

echo "$gz"
