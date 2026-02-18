#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/run-ia64-alat-tests.sh [timeout]

Builds and runs a bare-metal IA-64 directed selftest for ALAT
(`ld*.a`, `ldf*.a`, `chk.a.{clr,nc}`, `invala`) using QEMU kernel entry.

Arguments:
  timeout   Optional timeout passed to `timeout` (default: 12s)

Environment overrides:
  IA64_ALAT_LOGDIR   (default: scratch/ia64_logs/alat)
  IA64_ALAT_OUTDIR   (default: scratch/ia64_alat_selftest)
  IA64_AS            (default: ia64-suse-linux-as)
  IA64_LD            (default: ia64-suse-linux-ld)
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

run_timeout="${1:-12s}"
logdir="${IA64_ALAT_LOGDIR:-scratch/ia64_logs/alat}"
outdir="${IA64_ALAT_OUTDIR:-scratch/ia64_alat_selftest}"
as_bin="${IA64_AS:-ia64-suse-linux-as}"
ld_bin="${IA64_LD:-ia64-suse-linux-ld}"

mkdir -p "$logdir"
mkdir -p "$outdir"

src="scripts/ia64-alat-selftest.S"
obj="$outdir/alat-selftest.o"
bin="$outdir/alat-selftest.elf"

"$as_bin" -o "$obj" "$src"
"$ld_bin" -static -nostdlib -e _start -Ttext=0x5000000 -o "$bin" "$obj"

set +e
QEMU_IA64_BREAK_LOG=1 IA64_LOGDIR="$logdir" IA64_KERNEL="$bin" IA64_INITRD= IA64_APPEND= \
  timeout "$run_timeout" scripts/run-ia64-kernel.sh
rc=$?
set -e

if [[ $rc -ne 124 && $rc -ne 137 ]]; then
  echo "alat test run failed with rc=$rc" >&2
  exit $rc
fi

serial_log="$(ls -1t "$logdir"/serial.*.log 2>/dev/null | head -n1 || true)"
if [[ -z "$serial_log" ]]; then
  echo "no serial log produced in $logdir" >&2
  exit 1
fi

echo "alat test serial: $serial_log"

qemu_log="$logdir/qemu.log"
if [[ ! -f "$qemu_log" ]]; then
  echo "missing qemu log: $qemu_log" >&2
  exit 1
fi

if ! grep -q "IA64: breaki" "$qemu_log"; then
  echo "missing break log markers in $qemu_log" >&2
  exit 1
fi

if grep -q "r8=00000000616c7466" "$qemu_log"; then
  echo "saw ALAT FAIL marker in $qemu_log" >&2
  grep "r8=00000000616c7466" "$qemu_log" | head -n 5 >&2 || true
  exit 1
fi

if ! grep -q "r8=00000000616c7470" "$qemu_log"; then
  echo "missing ALAT PASS marker in $qemu_log" >&2
  exit 1
fi

echo "alat tests passed"
