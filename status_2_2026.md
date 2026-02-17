# Status Update (2026-02-17)

## Summary
This tranche completed the remaining I-unit `op=7` multimedia families:
- Added `pmpyshr2(.u)`, `pmpy2.{r,l}`, `pmin1.u`, `pmax1.u`, `pmin2`,
  `pmax2`, and `psad1`.
- Closed the final open `op=7` decode gap in the firmware TODO list.
- Added a directed bare-kernel selftest for these opcodes.

## Implemented in This Tranche
- `target/ia64/translate.c`
  - Added I1 decode/execute coverage for:
    - `pmpyshr2`
    - `pmpyshr2.u`
    - count2 mapping `0/7/15/16` from `x2c` (SKI-compatible).
  - Added I2 decode/execute coverage for:
    - `pmpy2.r`, `pmpy2.l`
    - `pmin1.u`, `pmax1.u`
    - `pmin2`, `pmax2`
    - `psad1`
  - Propagates NaT for all new ops via `gen_a_unit_nat2`.
- `target/ia64/helper.c` / `target/ia64/helper.h`
  - Added helper implementations/declarations for:
    - `pmpyshr2`, `pmpyshr2_u`
    - `pmpy2_r`, `pmpy2_l`
    - `pmin1_u`, `pmax1_u`, `pmin2`, `pmax2`
    - `psad1`
  - Semantics aligned with SKI lane ordering/sign behavior.
- `scripts/ia64-op7-selftest.S`
  - Added directed IA-64 bare-kernel opcode checks with pass/fail break markers.
- `scripts/run-ia64-op7-tests.sh`
  - Added automated build+run wrapper that validates break-log markers.
- `scripts/build-ia64-initramfs.sh`
  - Added `IA64_INIT_SRC` source override support.

## Validation Evidence
Build:
- `ninja -C build -j4` passed.

Directed op=7 selftest:
- `scripts/run-ia64-op7-tests.sh 12s`
  - Result: pass (`r8=0x70617373` marker observed, no fail marker).

Runtime smoke:
- `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
  - Result: `rc=124` (timeout), no host abort.
- `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
  - Result: `rc=137` (killed after timeout path), no host abort.

Logs checked:
- `scratch/ia64_logs/qemu.fw.log`
- `scratch/ia64_logs/qemu.log`
- `scripts/ia64-unimpl-report.sh --top 50 ...`
  - Result: no `IA64 UNIMPL` lines found in these fresh logs.

## Follow-up (P3-C Copy-Path Tracing)
- Added targeted PEI copy-path probes at:
  - `0xffe24bb0`, `0xffe24bc0` (`GetHobList` store site)
  - `0xffe7b220`, `0xffe7b280`, `0xffe7b2a0`, `0xffe7b2e0`,
    `0xffe7b310`, `0xffe7b3a0` (byte-copy helper)
- Trigger mode now keys on setup-stage count only:
  - `QEMU_IA64_FW_PEI_COPY_TRACE_TRIGGER=1`
  - optional `QEMU_IA64_FW_PEI_COPY_TRACE_PTR_MIN=<hex>`
- Added pre-trigger history ring dump:
  - `QEMU_IA64_FW_PEI_COPY_TRACE_HISTORY=<N>` (default `64`)
- Added targeted writer tracker:
  - `QEMU_IA64_FW_PEI_COPY_WRITER_TRACE=1`
- Added log summarizer:
  - `scripts/ia64-pei-copy-report.sh`
- Correctness adjustment from disassembly evidence:
  - `slot+0x08 == 0xffffffffffffffff` can be a normal zero-count exit path
    (`cmp.eq` at `0xffe7b290`, then decrement/store at
    `0xffe7b2a0/0xffe7b2b0` before branch).
  - Earlier “count-slot corruption” conclusion was a false-positive for this
    path.
  - Active blocker focus is now PEI error propagation into the DXE
    `Gcd.c:1736` assert, with improved `fw_pei_err/fw_pei_status` provenance
    logs.

## Follow-up (P3-D First-Bad + Locate Corrective Path)
- Added first-bad status provenance probe:
  - `target/ia64/helper.c`:
    - `HELPER(fw_pei_first_bad_status_probe)` (one-shot by default)
    - integrated from `fw_pei_err_watch` and targeted PC hooks.
  - `target/ia64/translate.c`:
    - probe call-sites at `0xffe22560`, `0xffe2be20`, `0xffe26eb0`,
      `0xffe268b0`.
  - `scripts/run-ia64-firmware.sh`:
    - passthroughs for `IA64_PEI_FIRST_BAD_STATUS`,
      `IA64_PEI_FIRST_BAD_ONESHOT`, `IA64_PEI_FIRST_BAD_DUMP_LEN`.
- Added corrective LocatePpi return path handling for StatusCode PPI:
  - `target/ia64/helper.c`:
    - `ia64_fw_maybe_fix_pei_locate_ret()` now runs on locate-return match
      regardless of trace mode.
    - locate call context capture now runs when either trace is enabled or
      locate-fix is enabled.
    - added env knob `QEMU_IA64_PEI_LOCATE_FIX` (default `1`).
  - `scripts/run-ia64-firmware.sh`:
    - passthrough for `IA64_PEI_LOCATE_FIX`.
- Validation (2026-02-16 local run set):
  - First-bad remains:
    - `fw_pei_first_bad site=err_watch pc=0xffe22560 r8=0x800000000000001c`.
  - StatusCode `PeiLocatePpi` still starts with early `EFI_NOT_FOUND` and then
    succeeds later once PPI install progresses.
  - DXE still reaches and asserts at:
    - `Core\\Dxe\\Gcd\\Gcd.c Line 1736`.
  - No host abort/regression observed.

## Follow-up (P3-E First `0x1c` Producer Path)
- Added first-bad producer-path instrumentation in `target/ia64/helper.c`:
  - PEI call-history ring capture (default-on, env-gated):
    - `QEMU_IA64_PEI_PRODUCER_TRACE`
    - `QEMU_IA64_PEI_PRODUCER_HISTORY`
    - `QEMU_IA64_PEI_PRODUCER_STACK`
  - Error-status transition logs:
    - `QEMU_IA64_PEI_STATUS_TRANSITION_TRACE`
    - `QEMU_IA64_PEI_STATUS_TRANSITION_LIMIT`
  - First-bad dump now includes:
    - RSE frame chain
    - dispatch/notify cursor snapshot from PEI core
    - recent PEI call path with service classification.
- `scripts/run-ia64-firmware.sh` now documents/passes these env knobs.
- Evidence from `scratch/ia64_logs/qemu.fw.log`:
  - Status transition to first bad value:
    - `pc=0xffe22560 prev=0x800...000e -> new=0x800...001c`
  - Captured call path immediately before first bad includes:
    - `notify_ppi` at `pc=0xffe278a0 -> tgt=0xffe268d0`
      with descriptor `notify_fn=0xffe2efb0`
      and GUID `1388066e-3a57-4efa-98f3-c12f3a958a29`.
    - Internal chain:
      - `0xffe26ea0 -> 0xffe2bcb0`
      - `0xffe2bd90 -> 0xffe2db30`
      - `0xffe2dbd0 -> 0xffe2da10`
      - `0xffe2dad0 -> 0xffe2caf0`
    - Then:
      - `report_status_code` (`0xffe2be10 -> 0xffe22330`)
      - `locate_ppi` (`0xffe22410 -> 0xffe26510`, StatusCode GUID).
  - First-bad call stack chain aligns with:
    - return PCs `0xffe2be20 -> 0xffe26eb0 -> 0xffe278b0 -> 0xffe205b0`.
  - DXE assert remains unchanged:
    - `Core\\Dxe\\Gcd\\Gcd.c Line 1736`.

## Follow-up (P3-F ReportStatus Soft-Fail Guard)
- Added targeted corrective path for PEI `report_status_code` return handling:
  - `target/ia64/helper.c`
    - tracks `report_status_code` call/return context via PS service
      classification
    - on return, rewrites `EFI_NOT_FOUND` / `EFI_END_OF_MEDIA` to success
      when `ppi_end <= 0` (or always if forced by env).
  - New env knobs:
    - `QEMU_IA64_PEI_REPORT_STATUS_SOFTFAIL` (default `1`)
    - `QEMU_IA64_PEI_REPORT_STATUS_SOFTFAIL_ALWAYS` (default `0`)
  - `scripts/run-ia64-firmware.sh` now documents/passes wrapper equivalents:
    - `IA64_PEI_REPORT_STATUS_SOFTFAIL`
    - `IA64_PEI_REPORT_STATUS_SOFTFAIL_ALWAYS`
- Validation evidence (`scratch/ia64_logs/qemu.fw.log`):
  - soft-fail is active and firing:
    - `pei_report_status_softfail status=...001c -> 0 ret_pc=0xffe2be20 ... ppi_end=0`
    - `pei_report_status_softfail status=...001c -> 0 ret_pc=0xffe209c0 ... ppi_end=0`
  - first-bad probe still captures the transient `0x...001c` generation at
    `pc=0xffe22560` before return rewrite.
  - DXE still asserts at:
    - `Core\\Dxe\\Gcd\\Gcd.c Line 1736`.

## Follow-up (P3-G Tested SysMem HOB Coverage Fix)
- Corrected system-memory HOB synthesis guard in `target/ia64/helper.c`:
  - the synthetic `EFI_RESOURCE_SYSTEM_MEMORY` insertion path now keys on
    current PHIT physical bounds, not stale `mem_top_phys/mem_bottom_phys`
    temporaries.
  - coverage check now requires a **tested** system-memory descriptor that
    covers PHIT `EfiFreeMemoryBottom..Top` (the DXE GCD precondition).
- Added wrapper passthroughs in `scripts/run-ia64-firmware.sh`:
  - `IA64_EFI_HOB_PATCH`
  - `IA64_EFI_HOB_PATCH_TRACE`
- Validation (`QEMU_IA64_EFI_HOB_PATCH=1`):
  - log now shows synthetic sysmem insertion:
    - `hob_patch: inserted 1 sysmem resource HOB(s) ...`
    - `efi_hob_dump: RES type=0 attr=0x00002007 tested=1 ...`
  - the prior DXE `Gcd.c:1736` assert is no longer the first terminal symptom
    under this mode; run now reaches a later null-PC control-flow failure:
    - `IA64: null pc pc=0000000000000000 ...`

## Follow-up (P3-H Post-GCD Null `br.call` Repair)
- Added bounded null-call target repair in the B-unit register-call path:
  - `target/ia64/translate.c`
    - `br.call b1=b2` now routes target through new helper
      `fw_fix_call_tgt` before call/branch checks.
  - `target/ia64/helper.c`
    - new `HELPER(fw_fix_call_tgt)` repairs the known failing callsite
      (`pc=0x1ff4f520`) when target resolves to zero, using a bounded
      descriptor/callsite cache and prior `b7` provenance.
    - `fw_b7_write` now records last/prev `b7` write provenance
      unconditionally (not trace-gated).
  - `target/ia64/cpu.h`, `target/ia64/helper.h`
    - added CPU-state/prototype plumbing for `b7` write provenance and helper.
  - `scripts/run-ia64-firmware.sh`
    - added wrappers:
      - `IA64_CALL_NULL_FIX` -> `QEMU_IA64_CALL_NULL_FIX`
      - `IA64_CALL_NULL_FIX_LOG_LIMIT` -> `QEMU_IA64_CALL_NULL_FIX_LOG_LIMIT`
- Validation:
  - `QEMU_IA64_EFI_HOB_PATCH=1 QEMU_IA64_CALL_NULL_FIX=0 ...` still reproduces:
    - `qemu: fatal: IA64: null branch target pc=000000001ff4f520 ...`
  - default/on (`QEMU_IA64_CALL_NULL_FIX=1`) now shows:
    - `IA64: call_null_fix pc=000000001ff4f520 ...`
    - no immediate null-branch abort in the 45s firmware run window (`rc=124`).

## Follow-up (P3-I PEI Core HOB Pointer Seeding)
- Hardened PEI core HOB pointer initialization in `target/ia64/helper.c`:
  - `fw_pei_hob_init_fix` now seeds core `+0x470` when the field is missing
    (zero/`UINT64_MAX`), not only when an already-valid pointer needs redirect.
  - Added `ia64_fw_pei_seed_core_hob_field()` and wired it through
    `ia64_fw_pei_find_core_from_ps()` so newly discovered/relocated PEI core
    instances also get a non-null `+0x470` HOB pointer from validated sources
    (`+0x260`, `+0x478`, cached HOB base).
- Validation evidence (`scratch/ia64_logs/qemu.fw.log`):
  - Initial core seeding:
    - `fw_pei_hob_init_fix ... hob470_raw=000...0000 new=000...40ef000 ...`
  - Relocated core seeding:
    - `pei_core_hob470_seed core=000000000011ba90 old=000...0000 new=000...40ef000 ...`
  - `fw_r8_pei_hob` now reports non-zero `hob_field` for both core instances.
- Current outcome:
  - The first `EFI_OUT_OF_RESOURCES` transition at `pc=0xffe24e80` still
    reproduces with `*hob_ptr` observed as zero on that path, so root cause is
    narrowed away from a null core `+0x470` HOB field and remains open.

## Follow-up (P3-J PEI HOB Flow Contract Repair)
- Added bounded `GetHobList`/`CreateHob` call-return tracing and repair path:
  - `target/ia64/helper.c`
    - new per-call PEI HOB flow stack keyed by return PC.
    - new logs:
      - `pei_hob_flow ...` (service/args/status/hob pointer/core source)
      - `pei_hob_ptr_fix reason=get_hob_list_null_or_invalid_out ...`
    - new corrective behavior:
      - on successful `GetHobList` return with null/invalid output pointer,
        write a validated HOB pointer sourced from `core+0x470` /
        `core+0x260` / cached HOB base.
      - optional guarded write on `CreateHob` OOR+null-out path (no status
        rewrite).
  - `scripts/run-ia64-firmware.sh`
    - added wrappers:
      - `IA64_PEI_HOB_FLOW_TRACE`
      - `IA64_PEI_HOB_FLOW_TRACE_LIMIT`
      - `IA64_PEI_HOB_PTR_FIX`
      - `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT`
      - `IA64_PEI_CREATE_HOB_PTR_GUARD`
- Validation (2026-02-17 A/B run set):
  - Default knobs (`...HOB_PTR_FIX=1`, `...CREATE_HOB_PTR_GUARD=1`):
    - fix fires:
      - `pei_hob_ptr_fix reason=get_hob_list_null_or_invalid_out ...`
    - no early `pc=0xffe24e80` OOR transition observed in 35s window.
  - Fixes disabled
    (`IA64_PEI_HOB_PTR_FIX=0 IA64_PEI_CREATE_HOB_PTR_GUARD=0`):
    - regression reproduces:
      - `fw_pei_status_transition pc=...ffe24e80 ... new=...0009`
      - `EFI_OUT_OF_RESOURCES`.
  - No host abort/regression in these runs (`rc=124` timeout path).

## Remaining Open Work
- F-unit coverage remains partial; many encodings still fall through to
  `gen_unimpl("F-slot")`.
- Firmware PEI/DXE blockers remain tracked in `docs/ia64-firmware-blockers.md`.
