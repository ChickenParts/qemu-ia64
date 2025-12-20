#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/run-ia64-firmware.sh [--] [extra qemu args...]

Environment overrides:
  QEMU_BIN        (default: ./build/qemu-system-ia64)
  IA64_BIOS       (default: stuff/Flash.fd)
  IA64_MEM        (default: 512M)
  IA64_SMP        (default: 1)
  IA64_LOGDIR     (default: scratch/ia64_logs)
  IA64_DISPLAY    (default: none)
  IA64_FW_FASTPATH (default: 1; enable memcpy/memset accel)
  IA64_GUEST_ERRORS (default: 0; enable -d guest_errors/-D)
  IA64_HANG_ABORT (default: 0; empty/0 disables)

Outputs:
  - Serial log is timestamped (kept):  $IA64_LOGDIR/serial.fw.<ts>.log
  - QEMU -D log is overwritten:        $IA64_LOGDIR/qemu.fw.log
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
bios="${IA64_BIOS:-stuff/Flash.fd}"
mem="${IA64_MEM:-512M}"
smp="${IA64_SMP:-1}"
logdir="${IA64_LOGDIR:-scratch/ia64_logs}"
display="${IA64_DISPLAY:-none}"
fw_fastpath="${IA64_FW_FASTPATH:-1}"
guest_errors="${IA64_GUEST_ERRORS:-0}"

mkdir -p "$logdir"

ts="$(date +%Y%m%d-%H%M%S)"
serial_log="$logdir/serial.fw.$ts.log"
qemu_log="$logdir/qemu.fw.log"

if [[ -n "${fw_fastpath:-}" && "${fw_fastpath:-0}" != "0" ]]; then
  export QEMU_IA64_FW_FASTPATH=1
fi

hang_abort="${IA64_HANG_ABORT:-0}"
if [[ -n "${hang_abort:-}" && "${hang_abort:-0}" != "0" ]]; then
  export QEMU_IA64_HANG_ABORT="$hang_abort"
fi

args=(
  -accel tcg
  -M ipf
  -m "$mem"
  -smp "$smp"
  -display "$display"
  -monitor none
  -serial "file:$serial_log"
  -bios "$bios"
)

if [[ -n "${guest_errors:-}" && "${guest_errors:-0}" != "0" ]]; then
  args+=(-d guest_errors -D "$qemu_log")
fi

exec "$qemu_bin" "${args[@]}" "$@"
