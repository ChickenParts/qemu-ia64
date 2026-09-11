#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: IA64_BIOS=path/to/Flash.fd IA64_ESP=path/to/esp scripts/run-ia64-efi-app.sh [--] [extra qemu args...]

Runs the IPF machine through firmware and boots an IA-64 EFI application from
EFI/BOOT/BOOTIA64.EFI on a FAT-backed IDE disk.

This is intentionally an EFI-path harness. It does not use QEMU -kernel and it
does not define a private Rooster/QEMU handoff ABI.

Environment:
  QEMU_BIN       qemu-system-ia64 binary (default: ./build/qemu-system-ia64)
  IA64_BIOS      firmware image (default: stuff/Flash.fd)
  IA64_ESP       directory used as FAT disk (default: scratch/ia64-esp)
  IA64_LOGDIR    firmware runner logs (default: scratch/ia64-rooster-efi)
  IA64_MEM       guest memory (default: 512M)
  IA64_SMP       vCPU count (default: 1)
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
esp="${IA64_ESP:-scratch/ia64-esp}"
logdir="${IA64_LOGDIR:-scratch/ia64-rooster-efi}"
bootefi="$esp/EFI/BOOT/BOOTIA64.EFI"

if [[ ! -x "$qemu_bin" ]]; then
    echo "error: QEMU binary not executable: $qemu_bin" >&2
    exit 2
fi
if [[ ! -s "$bios" ]]; then
    echo "error: IA-64 firmware missing/empty: $bios" >&2
    exit 2
fi
if [[ ! -s "$bootefi" ]]; then
    echo "error: EFI application missing/empty: $bootefi" >&2
    echo "       expected the removable-media path EFI/BOOT/BOOTIA64.EFI" >&2
    exit 2
fi

mkdir -p "$logdir"

export QEMU_BIN="$qemu_bin"
export IA64_BIOS="$bios"
export IA64_LOGDIR="$logdir"
export IA64_MEM="${IA64_MEM:-512M}"
export IA64_SMP="${IA64_SMP:-1}"

# Do not allow a caller to accidentally turn this into a direct-kernel test.
for arg in "$@"; do
    if [[ "$arg" == "-kernel" || "$arg" == -kernel=* ]]; then
        echo "error: -kernel is deliberately unsupported by the EFI-app harness" >&2
        exit 2
    fi
done

exec scripts/run-ia64-firmware.sh -- \
    -drive "file=fat:rw:$esp,format=raw,media=disk,if=ide" \
    "$@"
