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
  IA64_FW_FASTPATH (default: 0; enable memcpy/memset accel)
  IA64_GUEST_ERRORS (default: 0; enable -d guest_errors/-D)
  IA64_HANG_ABORT (default: 0; empty/0 disables)
  IA64_FW_BREAK0_STRICT_GATE (default: inherited/1; allow fw_break0 only on known call-gate regions)
  IA64_FW_BREAK0_GATE_RETURN (default: inherited/1; return to b0 from known ROM break(0) call-gates)
  IA64_FW_BREAK0_WR_GATE_RETURN (default: inherited/1; return to b0 from known work-RAM break(0) call-gates)
  IA64_FW_BREAK0_GATE_TRACE (default: inherited/off; trace ROM break(0) call-gate flow)
  IA64_FW_BREAK0_GATE_TRACE_LIMIT (default: inherited/64)
  IA64_FW_BREAK0_WR_GATE_TRACE (default: inherited/off; trace work-RAM break(0) gate loop cycles)
  IA64_FW_BREAK0_WR_GATE_TRACE_LIMIT (default: inherited/64)
  IA64_FW_BREAK0_WR_GATE_LOOP_GUARD (default: inherited/off; suppress repeated work-RAM break(0) loop churn via early helper return)
  IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_WINDOW (default: inherited/64)
  IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_THRESHOLD (default: inherited/8)
  IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_LOG_LIMIT (default: inherited/64)
  IA64_FW_XENIPF_MPBUFFER_FIX (default: inherited/1; seed/refresh MP buffer stack slots near IpfEarlyMpInit)
  IA64_FW_XENIPF_MPBUFFER_STICKY (default: inherited/1; keep repairing MP buffer slot/signature drift throughout MP-init window)
  IA64_FW_XENIPF_MPBUFFER_WINDOW (default: inherited/0x200; extend MP-init fix trigger window around ffe59800..ffe59a3f)
  IA64_FW_XENIPF_MPBUFFER_FIX_LOG_LIMIT (default: helper default/16)
  IA64_EFI_HOB_PATCH (default: inherited/off; enable EFI HOB repair path)
  IA64_EFI_HOB_PATCH_TRACE (default: inherited/off; verbose HOB patch logs)
  IA64_CALL_NULL_FIX (default: inherited/off; bisect-only guard for known null br.call target path)
  IA64_CALL_NULL_FIX_LOG_LIMIT (default: helper default)
  IA64_FW_PEI_COPY_TRACE (default: 0; enable fw_pei_copy probes)
  IA64_FW_PEI_COPY_TRACE_LIMIT (default: helper default)
  IA64_FW_PEI_COPY_TRACE_HISTORY (default: helper default)
  IA64_FW_PEI_COPY_TRACE_TRIGGER (default: 0; log first suspicious setup count)
  IA64_FW_PEI_COPY_TRACE_PTR_MIN (default: 0x100000)
  IA64_FW_PEI_COPY_WRITER_TRACE (default: 0; track first writer of copy slot)
  IA64_PEI_STATUS_LIMIT (default: 64; fw_pei_status unique log limit)
  IA64_PEI_FIRST_BAD_STATUS (default: 0x800000000000001c)
  IA64_PEI_FIRST_BAD_ONESHOT (default: 1; capture first match only)
  IA64_PEI_FIRST_BAD_DUMP_LEN (default: 64; bytes for arg dumps)
  IA64_PEI_LOCATE_TRACE (default: 0; trace LocatePpi calls/returns with GUID context)
  IA64_PEI_LOCATE_TRACE_LIMIT (default: 256)
  IA64_PEI_LOCATE_FIX (default: 1; force StatusCode LocatePpi success if present in DB)
  IA64_PEI_INSTALL_TRACE (default: 0; trace InstallPpi calls/returns with descriptor GUID context)
  IA64_PEI_INSTALL_TRACE_LIMIT (default: 256)
  IA64_PEI_LIFECYCLE_TRACE (default: 0; correlate locate-not-found -> install -> locate-success GUID chains)
  IA64_PEI_LIFECYCLE_TRACE_LIMIT (default: 128)
  IA64_PEI_LIFECYCLE_TRACE_WINDOW (default: 256; max producer-seq gap for lifecycle correlation)
  IA64_PEI_BOOT_MODE_TRACE (default: 0; trace PEI GetBootMode call/return context)
  IA64_PEI_BOOT_MODE_RECOVERY_FIX (default: 0; rewrite recovery boot-mode to full configuration when recovery PPI is absent in DxeIpl callsite)
  IA64_PEI_BOOT_MODE_FIX_LOG_LIMIT (default: 64)
  IA64_PEI_PRODUCER_TRACE (default: 1; capture PEI call history for first-bad path dump)
  IA64_PEI_PRODUCER_HISTORY (default: 96; retained call entries for first-bad dump)
  IA64_PEI_PRODUCER_STACK (default: 12; RSE frames dumped at first-bad)
  IA64_PEI_PS_EPOCH_TRACE (default: 0; trace selected PEI PS/core epoch provenance)
  IA64_PEI_PS_EPOCH_TRACE_LIMIT (default: 64)
  IA64_PEI_PS_SELECT_STRICT (default: 1; prefer validated arg-derived PS over stale cached PS)
  IA64_PEI_STATUS_TRANSITION_TRACE (default: 1; log r8 error-status transitions)
  IA64_PEI_STATUS_TRANSITION_LIMIT (default: 64)
  IA64_DXE_LOAD_TRACE (default: 0; trace DXE-load status propagation window at pc=0xffe255a0..0xffe2572c)
  IA64_DXE_LOAD_TRACE_LIMIT (default: 128)
  IA64_DXE_LOAD_TRACE_REPEAT_WINDOW (default: 32; suppress repeated DXE-load probe fingerprints within this producer-seq window)
  IA64_DXE_LOAD_VALUE_TRACE (default: 1; include r33/r34/status-pointer dereference values in DXE-load probe logs)
  IA64_DXE_ASSERT_TRACE (default: 0; trace calls into DxeIpl assert helper target 0xffe7e620 with status/file/line payload)
  IA64_DXE_ASSERT_TRACE_LIMIT (default: 128)
  IA64_PEI_STATUSCODE_SEMANTIC_FIX (default: 1; semantic StatusCode optional-path success)
  IA64_PEI_STATUSCODE_SEMANTIC_FIX_LOG_LIMIT (default: 64)
  IA64_PEI_NOTIFY_TRACE (default: 0; trace notify-ppi return status for traced blocker path)
  IA64_PEI_NOTIFY_TRACE_LIMIT (default: 64)
  IA64_PEI_NOTIFY_TRACE_ONESHOT (default: 1; only log first soft-error notify return)
  IA64_PEI_NOTIFY_STATUS_FIX (default: 0; bounded notify-ppi soft-error rewrite for traced GUID/path)
  IA64_PEI_NOTIFY_STATUS_FIX_ALWAYS (default: 0; ignore unresolved-path guard for notify fix)
  IA64_PEI_NOTIFY_STATUS_FIX_LOG_LIMIT (default: 64)
  IA64_PEI_22560_TRACE (default: 0; trace status mutation context at pc=0xffe22560)
  IA64_PEI_22560_TRACE_LIMIT (default: 64)
  IA64_PEI_22560_STATUS_FIX (default: 0; bounded status rewrite at pc=0xffe22560)
  IA64_PEI_22560_STATUS_FIX_ALWAYS (default: 0; ignore unresolved-path guard for 0xffe22560 fix)
  IA64_PEI_22560_STATUS_FIX_LOG_LIMIT (default: 64)
  IA64_PEI_279D0_TRACE (default: 0; trace non-EFI status path at pc=0xffe279d0..0xffe27a10)
  IA64_PEI_279D0_TRACE_LIMIT (default: 64)
  IA64_PEI_279D0_STATUS_FIX (default: 0; bounded rewrite for non-EFI status at pc=0xffe279d0..0xffe27a10)
  IA64_PEI_279D0_STATUS_FIX_ALWAYS (default: 0; allow ps-link fallback guard for 0xffe279d0 fix)
  IA64_PEI_279D0_STATUS_FIX_LOG_LIMIT (default: 64)
  IA64_PEI_279D0_SAFE_MODE (default: 1; quarantine 279d0 rewrite path and log bundle probes)
  IA64_PEI_279D0_SAFE_MODE_LOG_LIMIT (default: 64)
  IA64_PEI_REPORT_STATUS_SOFTFAIL (default: 1; treat early StatusCode report errors as non-fatal)
  IA64_PEI_REPORT_STATUS_SOFTFAIL_ALWAYS (default: 0; softfail regardless of ppi_end)
  IA64_PEI_HOB_FLOW_TRACE (default: 0; trace GetHobList/CreateHob call-return contract)
  IA64_PEI_HOB_FLOW_TRACE_LIMIT (default: 128)
  IA64_PEI_HOB_PTR_FIX (default: 0; repair success-returned GetHobList with null/invalid out pointer)
  IA64_PEI_HOB_PTR_FIX_LOG_LIMIT (default: 64)
  IA64_PEI_CREATE_HOB_PTR_GUARD (default: 0; guard CreateHob OOR path when out pointer is null/invalid)

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
fw_fastpath="${IA64_FW_FASTPATH:-0}"
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

if [[ -n "${IA64_FW_BREAK0_STRICT_GATE:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_STRICT_GATE="${IA64_FW_BREAK0_STRICT_GATE}"
fi

if [[ -n "${IA64_FW_BREAK0_GATE_RETURN:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_GATE_RETURN="${IA64_FW_BREAK0_GATE_RETURN}"
fi

if [[ -n "${IA64_FW_BREAK0_WR_GATE_RETURN:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_WR_GATE_RETURN="${IA64_FW_BREAK0_WR_GATE_RETURN}"
fi

if [[ -n "${IA64_FW_BREAK0_GATE_TRACE:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_GATE_TRACE="${IA64_FW_BREAK0_GATE_TRACE}"
fi

if [[ -n "${IA64_FW_BREAK0_GATE_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_GATE_TRACE_LIMIT="${IA64_FW_BREAK0_GATE_TRACE_LIMIT}"
fi

if [[ -n "${IA64_FW_BREAK0_WR_GATE_TRACE:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_WR_GATE_TRACE="${IA64_FW_BREAK0_WR_GATE_TRACE}"
fi

if [[ -n "${IA64_FW_BREAK0_WR_GATE_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_WR_GATE_TRACE_LIMIT="${IA64_FW_BREAK0_WR_GATE_TRACE_LIMIT}"
fi

if [[ -n "${IA64_FW_BREAK0_WR_GATE_LOOP_GUARD:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_WR_GATE_LOOP_GUARD="${IA64_FW_BREAK0_WR_GATE_LOOP_GUARD}"
fi

if [[ -n "${IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_WINDOW:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_WINDOW="${IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_WINDOW}"
fi

if [[ -n "${IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_THRESHOLD:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_THRESHOLD="${IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_THRESHOLD}"
fi

if [[ -n "${IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_LOG_LIMIT="${IA64_FW_BREAK0_WR_GATE_LOOP_GUARD_LOG_LIMIT}"
fi

if [[ -n "${IA64_FW_XENIPF_MPBUFFER_FIX:-}" ]]; then
  export QEMU_IA64_FW_XENIPF_MPBUFFER_FIX="${IA64_FW_XENIPF_MPBUFFER_FIX}"
fi

if [[ -n "${IA64_FW_XENIPF_MPBUFFER_STICKY:-}" ]]; then
  export QEMU_IA64_FW_XENIPF_MPBUFFER_STICKY="${IA64_FW_XENIPF_MPBUFFER_STICKY}"
fi

if [[ -n "${IA64_FW_XENIPF_MPBUFFER_WINDOW:-}" ]]; then
  export QEMU_IA64_FW_XENIPF_MPBUFFER_WINDOW="${IA64_FW_XENIPF_MPBUFFER_WINDOW}"
fi

if [[ -n "${IA64_FW_XENIPF_MPBUFFER_FIX_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_FW_XENIPF_MPBUFFER_FIX_LOG_LIMIT="${IA64_FW_XENIPF_MPBUFFER_FIX_LOG_LIMIT}"
fi

if [[ -n "${IA64_EFI_HOB_PATCH:-}" ]]; then
  export QEMU_IA64_EFI_HOB_PATCH="${IA64_EFI_HOB_PATCH}"
fi

if [[ -n "${IA64_EFI_HOB_PATCH_TRACE:-}" ]]; then
  export QEMU_IA64_EFI_HOB_PATCH_TRACE="${IA64_EFI_HOB_PATCH_TRACE}"
fi

if [[ -n "${IA64_CALL_NULL_FIX:-}" ]]; then
  export QEMU_IA64_CALL_NULL_FIX="${IA64_CALL_NULL_FIX}"
fi

if [[ -n "${IA64_CALL_NULL_FIX_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_CALL_NULL_FIX_LOG_LIMIT="${IA64_CALL_NULL_FIX_LOG_LIMIT}"
fi

fw_pei_copy_trace="${IA64_FW_PEI_COPY_TRACE:-0}"
if [[ -n "${fw_pei_copy_trace:-}" && "${fw_pei_copy_trace:-0}" != "0" ]]; then
  export QEMU_IA64_FW_PEI_COPY_TRACE=1
fi

if [[ -n "${IA64_FW_PEI_COPY_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_FW_PEI_COPY_TRACE_LIMIT="${IA64_FW_PEI_COPY_TRACE_LIMIT}"
fi

if [[ -n "${IA64_FW_PEI_COPY_TRACE_HISTORY:-}" ]]; then
  export QEMU_IA64_FW_PEI_COPY_TRACE_HISTORY="${IA64_FW_PEI_COPY_TRACE_HISTORY}"
fi

fw_pei_copy_trigger="${IA64_FW_PEI_COPY_TRACE_TRIGGER:-0}"
if [[ -n "${fw_pei_copy_trigger:-}" && "${fw_pei_copy_trigger:-0}" != "0" ]]; then
  export QEMU_IA64_FW_PEI_COPY_TRACE_TRIGGER=1
fi

if [[ -n "${IA64_FW_PEI_COPY_TRACE_PTR_MIN:-}" ]]; then
  export QEMU_IA64_FW_PEI_COPY_TRACE_PTR_MIN="${IA64_FW_PEI_COPY_TRACE_PTR_MIN}"
fi

fw_pei_copy_writer="${IA64_FW_PEI_COPY_WRITER_TRACE:-0}"
if [[ -n "${fw_pei_copy_writer:-}" && "${fw_pei_copy_writer:-0}" != "0" ]]; then
  export QEMU_IA64_FW_PEI_COPY_WRITER_TRACE=1
fi

if [[ -n "${IA64_PEI_STATUS_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_STATUS_LIMIT="${IA64_PEI_STATUS_LIMIT}"
fi

if [[ -n "${IA64_PEI_FIRST_BAD_STATUS:-}" ]]; then
  export QEMU_IA64_PEI_FIRST_BAD_STATUS="${IA64_PEI_FIRST_BAD_STATUS}"
fi

if [[ -n "${IA64_PEI_FIRST_BAD_ONESHOT:-}" ]]; then
  export QEMU_IA64_PEI_FIRST_BAD_ONESHOT="${IA64_PEI_FIRST_BAD_ONESHOT}"
fi

if [[ -n "${IA64_PEI_FIRST_BAD_DUMP_LEN:-}" ]]; then
  export QEMU_IA64_PEI_FIRST_BAD_DUMP_LEN="${IA64_PEI_FIRST_BAD_DUMP_LEN}"
fi

if [[ -n "${IA64_PEI_LOCATE_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_LOCATE_TRACE="${IA64_PEI_LOCATE_TRACE}"
fi

if [[ -n "${IA64_PEI_LOCATE_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_LOCATE_TRACE_LIMIT="${IA64_PEI_LOCATE_TRACE_LIMIT}"
fi

if [[ -n "${IA64_PEI_LOCATE_FIX:-}" ]]; then
  export QEMU_IA64_PEI_LOCATE_FIX="${IA64_PEI_LOCATE_FIX}"
fi

if [[ -n "${IA64_PEI_INSTALL_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_INSTALL_TRACE="${IA64_PEI_INSTALL_TRACE}"
fi

if [[ -n "${IA64_PEI_INSTALL_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_INSTALL_TRACE_LIMIT="${IA64_PEI_INSTALL_TRACE_LIMIT}"
fi

if [[ -n "${IA64_PEI_LIFECYCLE_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_LIFECYCLE_TRACE="${IA64_PEI_LIFECYCLE_TRACE}"
fi

if [[ -n "${IA64_PEI_LIFECYCLE_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_LIFECYCLE_TRACE_LIMIT="${IA64_PEI_LIFECYCLE_TRACE_LIMIT}"
fi

if [[ -n "${IA64_PEI_LIFECYCLE_TRACE_WINDOW:-}" ]]; then
  export QEMU_IA64_PEI_LIFECYCLE_TRACE_WINDOW="${IA64_PEI_LIFECYCLE_TRACE_WINDOW}"
fi

if [[ -n "${IA64_PEI_BOOT_MODE_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_BOOT_MODE_TRACE="${IA64_PEI_BOOT_MODE_TRACE}"
fi

if [[ -n "${IA64_PEI_BOOT_MODE_RECOVERY_FIX:-}" ]]; then
  export QEMU_IA64_PEI_BOOT_MODE_RECOVERY_FIX="${IA64_PEI_BOOT_MODE_RECOVERY_FIX}"
fi

if [[ -n "${IA64_PEI_BOOT_MODE_FIX_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_BOOT_MODE_FIX_LOG_LIMIT="${IA64_PEI_BOOT_MODE_FIX_LOG_LIMIT}"
fi

if [[ -n "${IA64_PEI_PRODUCER_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_PRODUCER_TRACE="${IA64_PEI_PRODUCER_TRACE}"
fi

if [[ -n "${IA64_PEI_PRODUCER_HISTORY:-}" ]]; then
  export QEMU_IA64_PEI_PRODUCER_HISTORY="${IA64_PEI_PRODUCER_HISTORY}"
fi

if [[ -n "${IA64_PEI_PRODUCER_STACK:-}" ]]; then
  export QEMU_IA64_PEI_PRODUCER_STACK="${IA64_PEI_PRODUCER_STACK}"
fi

if [[ -n "${IA64_PEI_PS_EPOCH_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_PS_EPOCH_TRACE="${IA64_PEI_PS_EPOCH_TRACE}"
fi

if [[ -n "${IA64_PEI_PS_EPOCH_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_PS_EPOCH_TRACE_LIMIT="${IA64_PEI_PS_EPOCH_TRACE_LIMIT}"
fi

if [[ -n "${IA64_PEI_PS_SELECT_STRICT:-}" ]]; then
  export QEMU_IA64_PEI_PS_SELECT_STRICT="${IA64_PEI_PS_SELECT_STRICT}"
fi

if [[ -n "${IA64_PEI_STATUS_TRANSITION_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_STATUS_TRANSITION_TRACE="${IA64_PEI_STATUS_TRANSITION_TRACE}"
fi

if [[ -n "${IA64_PEI_STATUS_TRANSITION_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_STATUS_TRANSITION_LIMIT="${IA64_PEI_STATUS_TRANSITION_LIMIT}"
fi

if [[ -n "${IA64_DXE_LOAD_TRACE:-}" ]]; then
  export QEMU_IA64_DXE_LOAD_TRACE="${IA64_DXE_LOAD_TRACE}"
fi

if [[ -n "${IA64_DXE_LOAD_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_DXE_LOAD_TRACE_LIMIT="${IA64_DXE_LOAD_TRACE_LIMIT}"
fi

if [[ -n "${IA64_DXE_LOAD_TRACE_REPEAT_WINDOW:-}" ]]; then
  export QEMU_IA64_DXE_LOAD_TRACE_REPEAT_WINDOW="${IA64_DXE_LOAD_TRACE_REPEAT_WINDOW}"
fi

if [[ -n "${IA64_DXE_LOAD_VALUE_TRACE:-}" ]]; then
  export QEMU_IA64_DXE_LOAD_VALUE_TRACE="${IA64_DXE_LOAD_VALUE_TRACE}"
fi

if [[ -n "${IA64_DXE_ASSERT_TRACE:-}" ]]; then
  export QEMU_IA64_DXE_ASSERT_TRACE="${IA64_DXE_ASSERT_TRACE}"
fi

if [[ -n "${IA64_DXE_ASSERT_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_DXE_ASSERT_TRACE_LIMIT="${IA64_DXE_ASSERT_TRACE_LIMIT}"
fi

if [[ -n "${IA64_PEI_STATUSCODE_SEMANTIC_FIX:-}" ]]; then
  export QEMU_IA64_PEI_STATUSCODE_SEMANTIC_FIX="${IA64_PEI_STATUSCODE_SEMANTIC_FIX}"
fi

if [[ -n "${IA64_PEI_STATUSCODE_SEMANTIC_FIX_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_STATUSCODE_SEMANTIC_FIX_LOG_LIMIT="${IA64_PEI_STATUSCODE_SEMANTIC_FIX_LOG_LIMIT}"
fi

if [[ -n "${IA64_PEI_NOTIFY_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_NOTIFY_TRACE="${IA64_PEI_NOTIFY_TRACE}"
fi

if [[ -n "${IA64_PEI_NOTIFY_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_NOTIFY_TRACE_LIMIT="${IA64_PEI_NOTIFY_TRACE_LIMIT}"
fi

if [[ -n "${IA64_PEI_NOTIFY_TRACE_ONESHOT:-}" ]]; then
  export QEMU_IA64_PEI_NOTIFY_TRACE_ONESHOT="${IA64_PEI_NOTIFY_TRACE_ONESHOT}"
fi

if [[ -n "${IA64_PEI_NOTIFY_STATUS_FIX:-}" ]]; then
  export QEMU_IA64_PEI_NOTIFY_STATUS_FIX="${IA64_PEI_NOTIFY_STATUS_FIX}"
fi

if [[ -n "${IA64_PEI_NOTIFY_STATUS_FIX_ALWAYS:-}" ]]; then
  export QEMU_IA64_PEI_NOTIFY_STATUS_FIX_ALWAYS="${IA64_PEI_NOTIFY_STATUS_FIX_ALWAYS}"
fi

if [[ -n "${IA64_PEI_NOTIFY_STATUS_FIX_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_NOTIFY_STATUS_FIX_LOG_LIMIT="${IA64_PEI_NOTIFY_STATUS_FIX_LOG_LIMIT}"
fi

if [[ -n "${IA64_PEI_22560_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_22560_TRACE="${IA64_PEI_22560_TRACE}"
fi

if [[ -n "${IA64_PEI_22560_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_22560_TRACE_LIMIT="${IA64_PEI_22560_TRACE_LIMIT}"
fi

if [[ -n "${IA64_PEI_22560_STATUS_FIX:-}" ]]; then
  export QEMU_IA64_PEI_22560_STATUS_FIX="${IA64_PEI_22560_STATUS_FIX}"
fi

if [[ -n "${IA64_PEI_22560_STATUS_FIX_ALWAYS:-}" ]]; then
  export QEMU_IA64_PEI_22560_STATUS_FIX_ALWAYS="${IA64_PEI_22560_STATUS_FIX_ALWAYS}"
fi

if [[ -n "${IA64_PEI_22560_STATUS_FIX_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_22560_STATUS_FIX_LOG_LIMIT="${IA64_PEI_22560_STATUS_FIX_LOG_LIMIT}"
fi

if [[ -n "${IA64_PEI_279D0_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_279D0_TRACE="${IA64_PEI_279D0_TRACE}"
fi

if [[ -n "${IA64_PEI_279D0_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_279D0_TRACE_LIMIT="${IA64_PEI_279D0_TRACE_LIMIT}"
fi

if [[ -n "${IA64_PEI_279D0_STATUS_FIX:-}" ]]; then
  export QEMU_IA64_PEI_279D0_STATUS_FIX="${IA64_PEI_279D0_STATUS_FIX}"
fi

if [[ -n "${IA64_PEI_279D0_STATUS_FIX_ALWAYS:-}" ]]; then
  export QEMU_IA64_PEI_279D0_STATUS_FIX_ALWAYS="${IA64_PEI_279D0_STATUS_FIX_ALWAYS}"
fi

if [[ -n "${IA64_PEI_279D0_STATUS_FIX_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_279D0_STATUS_FIX_LOG_LIMIT="${IA64_PEI_279D0_STATUS_FIX_LOG_LIMIT}"
fi

if [[ -n "${IA64_PEI_279D0_SAFE_MODE:-}" ]]; then
  export QEMU_IA64_PEI_279D0_SAFE_MODE="${IA64_PEI_279D0_SAFE_MODE}"
fi

if [[ -n "${IA64_PEI_279D0_SAFE_MODE_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_279D0_SAFE_MODE_LOG_LIMIT="${IA64_PEI_279D0_SAFE_MODE_LOG_LIMIT}"
fi

if [[ -n "${IA64_PEI_REPORT_STATUS_SOFTFAIL:-}" ]]; then
  export QEMU_IA64_PEI_REPORT_STATUS_SOFTFAIL="${IA64_PEI_REPORT_STATUS_SOFTFAIL}"
fi

if [[ -n "${IA64_PEI_REPORT_STATUS_SOFTFAIL_ALWAYS:-}" ]]; then
  export QEMU_IA64_PEI_REPORT_STATUS_SOFTFAIL_ALWAYS="${IA64_PEI_REPORT_STATUS_SOFTFAIL_ALWAYS}"
fi

if [[ -n "${IA64_PEI_HOB_FLOW_TRACE:-}" ]]; then
  export QEMU_IA64_PEI_HOB_FLOW_TRACE="${IA64_PEI_HOB_FLOW_TRACE}"
fi

if [[ -n "${IA64_PEI_HOB_FLOW_TRACE_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_HOB_FLOW_TRACE_LIMIT="${IA64_PEI_HOB_FLOW_TRACE_LIMIT}"
fi

if [[ -n "${IA64_PEI_HOB_PTR_FIX:-}" ]]; then
  export QEMU_IA64_PEI_HOB_PTR_FIX="${IA64_PEI_HOB_PTR_FIX}"
fi

if [[ -n "${IA64_PEI_HOB_PTR_FIX_LOG_LIMIT:-}" ]]; then
  export QEMU_IA64_PEI_HOB_PTR_FIX_LOG_LIMIT="${IA64_PEI_HOB_PTR_FIX_LOG_LIMIT}"
fi

if [[ -n "${IA64_PEI_CREATE_HOB_PTR_GUARD:-}" ]]; then
  export QEMU_IA64_PEI_CREATE_HOB_PTR_GUARD="${IA64_PEI_CREATE_HOB_PTR_GUARD}"
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
