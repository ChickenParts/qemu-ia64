#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/run-ia64-kernel.sh [--] [extra qemu args...]

Environment overrides:
  QEMU_BIN     (default: ./build/qemu-system-ia64)
  IA64_KERNEL  (default: stuff/vmlinux-ia64-main)
  IA64_BIOS    (default: stuff/Flash.fd if it exists, else empty)
  IA64_INITRD  (default: empty)
  IA64_APPEND  (default: console=hcdp earlyprintk=hcdp ignore_loglevel loglevel=7 keep_bootcon)
  IA64_MEM     (default: 512M)
  IA64_SMP     (default: 1)
  IA64_LOGDIR  (default: scratch/ia64_logs)

Outputs:
  - Serial log is timestamped (kept):  $IA64_LOGDIR/serial.<ts>.log
  - QEMU -D log is overwritten:        $IA64_LOGDIR/qemu.log
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ "${1:-}" == "--" ]]; then
  shift
fi

qemu_bin="${QEMU_BIN:-./build/qemu-system-ia64}"
kernel="${IA64_KERNEL:-stuff/vmlinux-ia64-main}"
initrd="${IA64_INITRD:-}"
append="${IA64_APPEND:-console=hcdp earlyprintk=hcdp ignore_loglevel loglevel=7 keep_bootcon}"
mem="${IA64_MEM:-512M}"
smp="${IA64_SMP:-1}"
logdir="${IA64_LOGDIR:-scratch/ia64_logs}"

bios_default=""
if [[ -f "stuff/Flash.fd" ]]; then
  bios_default="stuff/Flash.fd"
fi
bios="${IA64_BIOS:-$bios_default}"

mkdir -p "$logdir"

ts="$(date +%Y%m%d-%H%M%S)"
serial_log="$logdir/serial.$ts.log"
qemu_log="$logdir/qemu.log"

export QEMU_IA64_LOG_BREAK_STR="${QEMU_IA64_LOG_BREAK_STR:-1}"

args=(
  -accel tcg
  -M ipf
  -m "$mem"
  -smp "$smp"
  -display none
  -monitor none
  -serial "file:$serial_log"
  -d guest_errors
  -D "$qemu_log"
)

if [[ -n "$bios" ]]; then
  args+=( -bios "$bios" )
fi

args+=( -kernel "$kernel" -append "$append" )

if [[ -n "$initrd" ]]; then
  args+=( -initrd "$initrd" )
fi

exec "$qemu_bin" "${args[@]}" "$@"
