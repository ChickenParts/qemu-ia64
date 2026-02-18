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

## Follow-up (P3-K Core HOB Slot Coherence Root-Cause Fix)
- Implemented root-cause fix for early PEI HOB pointer divergence:
  - `target/ia64/helper.c`
    - `ia64_fw_pei_seed_core_hob_field()` now seeds both core `+0x260` and
      `+0x470` when missing/invalid, sourcing from validated
      `+0x260`/`+0x470`/`+0x478` or cached HOB base.
    - `fw_pei_hob_init_fix` now invokes this seeding path after core discovery
      so early PEI consumers do not see a null `+0x260` while `+0x470` is valid.
- Validation (2026-02-17):
  - Build/tests:
    - `ninja -C build -j4` passed.
    - `scripts/run-ia64-op7-tests.sh 12s` passed.
  - Firmware A/B:
    - with HOB pointer repair disabled
      (`IA64_PEI_HOB_PTR_FIX=0 IA64_PEI_CREATE_HOB_PTR_GUARD=0`):
      no early `pc=0xffe24e80` `EFI_OUT_OF_RESOURCES` transition in a 35s run.
    - default knobs: no early `pc=0xffe24e80` transition in 20s run.
  - Log evidence (`scratch/ia64_logs/qemu.fw.log`):
    - `pei_core_hob_seed core=... off=0x260 old=0 ... new=0x40ef000 ...`
    - no `pei_hob_ptr_fix` events required in the above windows.

## Follow-up (P3-L Retire PEI HOB Pointer Compatibility Shims by Default)
- Default behavior changed to root-cause-first:
  - `QEMU_IA64_PEI_HOB_FLOW_TRACE` default is now off.
  - `QEMU_IA64_PEI_HOB_PTR_FIX` default is now off.
  - `QEMU_IA64_PEI_CREATE_HOB_PTR_GUARD` default is now off.
  - knobs remain available for bisect compatibility.
- Validation (2026-02-17):
  - 5x repeated baseline windows with fixes explicitly off
    (`...HOB_PTR_FIX=0 ...CREATE_HOB_PTR_GUARD=0`): all timed out cleanly
    (`rc=124`), no early `pc=0xffe24e80` OOR transition, no
    `pei_hob_ptr_fix` events.
  - 3x default-mode baseline windows after default flip: same outcome.

## Follow-up (P3-M Null `br.call` Guard De-Prioritized/Default-Off)
- `QEMU_IA64_CALL_NULL_FIX` is now default-off (retained as opt-in bisect
  guard).
- Validation:
  - `IA64_EFI_HOB_PATCH=1` with default-off call fix, 90s window:
    no `call_null_fix` logs and no null-branch abort.
  - Historical `pc=0x1ff4f520` null call path is not reproducing in current
    baseline windows; root-cause remains open pending a fresh reproducer.

## Follow-up (CPU-C1 A-unit NaT Test Coverage)
- Added directed bare-kernel NaT propagation selftest:
  - `scripts/ia64-nat-selftest.S`
  - `scripts/run-ia64-nat-tests.sh`
- Coverage includes A-unit integer forms:
  - `add`, `and`, `shladd`, `adds`, `pavg2`.
- Validation:
  - `scripts/run-ia64-nat-tests.sh 15s` passed
    (PASS marker `r8=0x6e617470`, no FAIL marker).

## Follow-up (CPU-C2 Fresh UNIMPL Inventory Refresh)
- Re-ran fresh baseline inventory:
  - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
  - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
  - `scripts/ia64-unimpl-report.sh --format tsv --top 30 ...`
- Result:
  - no `IA64 UNIMPL` lines in fresh firmware/kernel logs.
  - no actionable fresh F-slot hit table for this tranche.

## Follow-up (CPU-C3 RSE/ALAT Correctness Hardening)
- `target/ia64/helper.c`
  - Added RSE strict trace controls:
    - `QEMU_IA64_RSE_STRICT_TRACE`
    - `QEMU_IA64_RSE_STRICT_TRACE_LIMIT`
  - Hardened RSE `loadrs` / `flushrs` behavior:
    - clamps `ar.rsc.loadrs` accounting in lazy mode
    - `loadrs` now consumes/clears `loadrs` and keeps
      `ar.bsp`/`ar.bspstore` coherent
    - `flushrs` now clears `loadrs` and synchronizes
      `ar.bspstore` with `ar.bsp` using lazy/eager-aware spill counts
  - Added strict-trace hooks on `alloc`, `loadrs`, `flushrs`,
    `set_bspstore`, and `ret_restore`.
  - Added ALAT trace controls:
    - `QEMU_IA64_ALAT_TRACE`
    - `QEMU_IA64_ALAT_TRACE_LIMIT`
  - Added ALAT trace points on record/check/invalidate/invala paths.
- `target/ia64/translate.c`
  - Updated stale `chk.a.clr f1` comment to match implemented ALAT behavior.
  - Wired FR ALAT recording for advanced FP loads (`ldf*.a`) so
    `chk.a.{clr,nc} f*` can detect aliasing stores.
- New directed selftests:
  - `scripts/ia64-rse-selftest.S`
  - `scripts/run-ia64-rse-tests.sh`
  - `scripts/ia64-alat-selftest.S`
  - `scripts/run-ia64-alat-tests.sh`
- Validation (2026-02-18):
  - Build:
    - `ninja -C build -j4 qemu-system-ia64` passed.
  - Directed tests:
    - `scripts/run-ia64-rse-tests.sh 10s` passed.
    - `scripts/run-ia64-alat-tests.sh 12s` passed.
    - `scripts/run-ia64-op7-tests.sh 12s` passed (regression).
    - `scripts/run-ia64-nat-tests.sh 12s` passed (regression).
  - Runtime smoke:
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
      -> `rc=124` (timeout path, no host abort).
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
      -> `rc=137` (killed after timeout path, no host abort).
  - UNIMPL refresh:
    - `scripts/ia64-unimpl-report.sh --format tsv --top 30 ...`
      reported no `IA64 UNIMPL` lines in the fresh logs.

## Follow-up (CPU-C4 F-slot Core Coverage)
- Added core F-slot instruction coverage in `target/ia64/translate.c`:
  - arithmetic pseudos:
    - `fadd.s` (major `0x8`, `x=1`, `f4=1`)
    - `fsub.s` (major `0xA`, `x=1`, `f4=1`)
    - `fmpy.s` (major `0x8`, `x=1`, `f2=0`)
  - scalar compare forms:
    - `fcmp.{eq,lt,le,unord}.s0`
  - scalar sign/magnitude forms:
    - `fabs`
    - `fneg`
- Added `HELPER(fcmp_s0)` in `target/ia64/helper.c`
  (`target/ia64/helper.h` declaration) and reused existing
  `fma_s1` / `fms_s1` helpers for pseudo arithmetic paths.
- Added directed selftest coverage:
  - `scripts/ia64-fslot-selftest.S`
  - `scripts/run-ia64-fslot-tests.sh`
- Validation (2026-02-18):
  - Build:
    - `ninja -C build -j4 qemu-system-ia64` passed.
  - Directed tests:
    - `scripts/run-ia64-fslot-tests.sh 15s` passed.
    - `scripts/run-ia64-rse-tests.sh 12s` passed.
    - `scripts/run-ia64-alat-tests.sh 12s` passed.
    - `scripts/run-ia64-op7-tests.sh 12s` passed.
    - `scripts/run-ia64-nat-tests.sh 12s` passed.
  - Runtime smoke:
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
      -> `rc=124`.
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
      -> `rc=137`.
  - UNIMPL refresh:
    - `scripts/ia64-unimpl-report.sh --format tsv --top 50 ...`
      reported no `IA64 UNIMPL` lines in fresh logs.

## Follow-up (CPU-C5 FP Special-Value Correctness)
- Corrected IA-64 FP helper conversion semantics in `target/ia64/helper.c`:
  - `ia64_fp_to_ld` now decodes special exponent (`0x1ffff`) as:
    - infinity when mantissa is `1<<63`
    - NaN otherwise
  - `ia64_ld_to_fp` now preserves special results instead of collapsing to
    zero:
    - signed zero
    - infinity
    - canonical quiet NaN
  - fixed exponent handling to avoid wraparound on overflow/underflow:
    - overflow now maps to infinity
    - underflow maps to signed zero
- Extended directed F-slot coverage in `scripts/ia64-fslot-selftest.S`:
  - overflow-to-infinity path via `fadd.s`
  - infinity ordering (`fcmp.lt` finite vs +Inf)
  - NaN unordered compare (`fcmp.unord` true for NaN input)
- Validation (2026-02-17):
  - Build:
    - `ninja -C build -j4 qemu-system-ia64` passed.
  - Directed tests:
    - `scripts/run-ia64-fslot-tests.sh 20s` passed.
    - regression passes:
      - `scripts/run-ia64-op7-tests.sh 12s`
      - `scripts/run-ia64-nat-tests.sh 12s`
      - `scripts/run-ia64-rse-tests.sh 12s`
      - `scripts/run-ia64-alat-tests.sh 12s`
  - Runtime smoke:
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
      -> `rc=124`.
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
      -> `rc=137`.
  - UNIMPL refresh:
    - no `IA64 UNIMPL` lines in fresh firmware/kernel logs.

## Follow-up (P3-N Notify Return Trace + Bounded Fix Probe)
- Implemented notify-return tracking in `target/ia64/helper.c`:
  - Added bounded notify call stack (`IA64PeiNotifyCall`) wired from
    `IA64_PEI_SVC_NOTIFY_PPI` producer calls.
  - Added notify descriptor/GUID decode helper and return-side trace/fix hook
    on `ret_restore` / `ret_restore_b0`.
  - Added bounded knobs:
    - `QEMU_IA64_PEI_NOTIFY_TRACE`
    - `QEMU_IA64_PEI_NOTIFY_TRACE_LIMIT`
    - `QEMU_IA64_PEI_NOTIFY_TRACE_ONESHOT`
    - `QEMU_IA64_PEI_NOTIFY_STATUS_FIX`
    - `QEMU_IA64_PEI_NOTIFY_STATUS_FIX_ALWAYS`
    - `QEMU_IA64_PEI_NOTIFY_STATUS_FIX_LOG_LIMIT`
  - Hardened return matching to pop by `b0` search (not strict top-only) so
    non-local unwind/noisy returns do not strand notify frames.
- `scripts/run-ia64-firmware.sh` now exports the new `IA64_PEI_NOTIFY_*`
  wrappers.
- Validation (2026-02-18, `scratch/ia64_logs/p3n/*`, 45s windows):
  - `base`: `rc=124`, `notify_ret=0`, `status_pc_ffe22560=4`.
  - `notify_fix_on`: `rc=124`, `notify_ret=8`, `notify_fixed=0`,
    `status_pc_ffe22560=4`.
  - `sem0`: `rc=124`, `notify_ret=8`, `notify_fixed=0`,
    `status_pc_ffe22560=4`, first-bad capture active.
  - `sem0_notify_fix_on`: `rc=124`, `notify_ret=8`, `notify_fixed=0`,
    `status_pc_ffe22560=4`, first-bad capture active.
  - Key evidence:
    - traced target notify return
      (`call_pc=0xffe278a0`, `tgt=0xffe268d0`,
      GUID `1388066e-3a57-4efa-98f3-c12f3a958a29`) consistently returns
      `status=0` (`soft=0`), so bounded notify-return rewrite does not fire.
    - current blocker status transition remains at
      `pc=0xffe22560` (`EFI_NOT_FOUND -> EFI_END_OF_MEDIA`) with no behavior
      change from notify-return rewrite enablement.
- Regression/smoke after notify instrumentation changes:
  - `scripts/run-ia64-fslot-tests.sh 15s` passed.
  - `scripts/run-ia64-op7-tests.sh 12s` passed.
  - `scripts/run-ia64-nat-tests.sh 12s` passed.
  - `scripts/run-ia64-rse-tests.sh 12s` passed.
  - `scripts/run-ia64-alat-tests.sh 12s` passed.
  - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
    -> `rc=124`.
  - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
    -> `rc=137`.

## Follow-up (P3-O `pc=0xffe22560` Mutation Attribution + Bounded Rewrite)
- Implemented post-notify mutation-site tracing/fix in `target/ia64/helper.c`:
  - added bounded env controls:
    - `QEMU_IA64_PEI_22560_TRACE`
    - `QEMU_IA64_PEI_22560_TRACE_LIMIT`
    - `QEMU_IA64_PEI_22560_STATUS_FIX`
    - `QEMU_IA64_PEI_22560_STATUS_FIX_ALWAYS`
    - `QEMU_IA64_PEI_22560_STATUS_FIX_LOG_LIMIT`
  - new mutation-site handler:
    - `ia64_fw_pei_maybe_fix_status_transition_ffe22560(...)`
    - called from `fw_pei_err_watch` after transition logging and before
      first-bad capture.
  - bounded rewrite guard requires:
    - `pc=0xffe22560`
    - soft EFI status (`EFI_NOT_FOUND`/`EFI_END_OF_MEDIA`)
    - unresolved StatusCode optional path (unless `...STATUS_FIX_ALWAYS=1`)
    - recent producer-chain match:
      `report_status_code` + `locate_ppi` StatusCode GUID
      `229832d3-7a30-4b36-b827-f40cb7d45436`
      within bounded sequence window.
- `scripts/run-ia64-firmware.sh` now exports `IA64_PEI_22560_*` wrappers.
- Validation (2026-02-18, `scratch/ia64_logs/p3o/*`, 45s windows):
  - `base`: `rc=124`, `transition_22560=4`, `22560_trace=0`,
    `22560_fixed=0`.
  - `trace_only`: `rc=124`, `transition_22560=4`, `22560_trace=4`,
    `22560_fixed=0`.
  - `fix_on`: `rc=124`, `transition_22560=5`, `22560_trace=5`,
    `22560_fixed=5`.
  - `sem0_fix_on`: `rc=124`, `transition_22560=5`, `22560_trace=5`,
    `22560_fixed=5`, `first_bad=1`.
  - key evidence:
    - bounded site-fix fires on matched chain:
      `pei_22560_status status=...001c fixed=1 ... chain=1 ...`
    - residual first-bad in `sem0_fix_on` is a later recurrence with changed
      context after non-EFI `r8=ffffffff0011fff0` transition; root cause is
      narrowed but not fully closed.
- Regression/smoke:
  - directed tests passed:
    - `scripts/run-ia64-fslot-tests.sh 15s`
    - `scripts/run-ia64-op7-tests.sh 12s`
    - `scripts/run-ia64-nat-tests.sh 12s`
    - `scripts/run-ia64-rse-tests.sh 12s`
    - `scripts/run-ia64-alat-tests.sh 12s`
  - runtime smoke:
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
      -> `rc=124`.
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
      -> `rc=137`.
  - UNIMPL refresh:
    - `scripts/ia64-unimpl-report.sh --format tsv --top 50 ...`
      reported no `IA64 UNIMPL` lines in fresh smoke logs.

## Follow-up (P3-P `pc=0xffe279d0` Non-EFI Status Attribution + Bridge Probe)
- Implemented post-`22560` non-EFI status-path instrumentation in
  `target/ia64/helper.c`:
  - new bounded env controls:
    - `QEMU_IA64_PEI_279D0_TRACE`
    - `QEMU_IA64_PEI_279D0_TRACE_LIMIT`
    - `QEMU_IA64_PEI_279D0_STATUS_FIX`
    - `QEMU_IA64_PEI_279D0_STATUS_FIX_ALWAYS`
    - `QEMU_IA64_PEI_279D0_STATUS_FIX_LOG_LIMIT`
  - new handler:
    - `ia64_fw_pei_maybe_handle_status_transition_ffe279d0(...)`
    - called from `fw_pei_err_watch` after `22560` handling and before
      first-bad capture.
  - trace/fix path targets bounded signature:
    - `pc` in `0xffe279d0..0xffe27a10`
    - `r8` in `{0xffffffff0011fff0, 0xffffffff0011bff0}`
    - recent `22560`-fix context link (`seq` delta window + PS linkage).
  - producer-ring capture is now retained for `279d0` flow even when
    producer-path dump tracing is disabled.
- `scripts/run-ia64-firmware.sh` now exports `IA64_PEI_279D0_*` wrappers.
- Validation (2026-02-18, `scratch/ia64_logs/p3p/*`, 45s windows):
  - `base`: `rc=124`, no `279d0` transitions/hits.
  - `p3p_trace_only`
    (`22560 fix on`, `279d0 trace on`, `279d0 fix off`):
    - `transition_279d0=1`, `transition_279f0=1`,
      `err_279d0_window=5`, `trace_279d0=4`, `fix_279d0=0`, `rc=124`.
  - `p3p_fix_on` (`279d0 fix on`):
    - `transition_279d0=1`, `transition_279f0=1`,
      `err_279d0_window=3`, `trace_279d0=1`, `fix_279d0=1`,
      `rc=134` (host assert).
  - `p3p_sem0_fix_on` (semantic fix off + `279d0 fix on`):
    - same `279d0` firing pattern; `first_bad=1`, `rc=134`.
  - key evidence:
    - rewrite fires under bounded link:
      `pei_279d0_status status=ffffffff0011fff0 fixed=1 ... seq_link=1 ...`
    - fix-on runs expose host-side TCG assert:
      `TB_EXIT_REQUESTED without exitreq/icount`
      (`cpu_loop_exec_tb` assertion).
- Regression/smoke (default knob baseline):
  - directed tests passed:
    - `scripts/run-ia64-fslot-tests.sh 15s`
    - `scripts/run-ia64-op7-tests.sh 12s`
    - `scripts/run-ia64-nat-tests.sh 12s`
    - `scripts/run-ia64-rse-tests.sh 12s`
    - `scripts/run-ia64-alat-tests.sh 12s`
  - runtime smoke:
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
      -> `rc=124`.
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
      -> `rc=137`.
  - UNIMPL refresh:
    - no `IA64 UNIMPL` lines in fresh smoke logs.

## Follow-up (P3-Q `279d0` Safe-Mode Quarantine for TCG Assert Path)
- Added bounded safe-mode guard for the `279d0` rewrite path in
  `target/ia64/helper.c`:
  - new env controls:
    - `QEMU_IA64_PEI_279D0_SAFE_MODE` (default on)
    - `QEMU_IA64_PEI_279D0_SAFE_MODE_LOG_LIMIT`
  - when safe mode is active and the bounded `279d0` fix predicate matches:
    - trace context is retained (`pei_279d0_status ...`)
    - helper captures/signals bundle probe state
      (`pei_279d0_status safe_probe ...`)
    - rewrite is quarantined (`safe_quarantine=1`, `fixed=0`).
- `scripts/run-ia64-firmware.sh` now exports wrappers:
  - `IA64_PEI_279D0_SAFE_MODE`
  - `IA64_PEI_279D0_SAFE_MODE_LOG_LIMIT`
- `docs/ia64-environment-variables.md` documents the new knobs.
- Validation (2026-02-18, `scratch/ia64_logs/p3q/*`, 45s windows):
  - `safe_default`
    (`22560 fix on`, `279d0 trace on`, `279d0 fix on`, safe mode default):
    - `rc=124`
    - `transition_279d0=1`, `trace_279d0=4`
    - `fix_279d0=0`, `safe_probe=4`, `safe_quarantine=4`
    - no host assert.
  - `safe_off`
    (`...279D0_SAFE_MODE=0`):
    - `rc=134`
    - `fix_279d0=1`
    - host assert reproduced:
      `TB_EXIT_REQUESTED without exitreq/icount`.
  - `trace_only` (`279d0 fix off`): unchanged baseline profile, `rc=124`.
- Regression/smoke after safe-mode wiring:
  - directed tests passed:
    - `scripts/run-ia64-fslot-tests.sh 15s`
    - `scripts/run-ia64-op7-tests.sh 12s`
    - `scripts/run-ia64-nat-tests.sh 12s`
    - `scripts/run-ia64-rse-tests.sh 12s`
    - `scripts/run-ia64-alat-tests.sh 12s`
  - runtime smoke:
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-firmware.sh`
      -> `rc=124`.
    - `IA64_GUEST_ERRORS=1 timeout 60s scripts/run-ia64-kernel.sh`
      -> `rc=137`.
  - UNIMPL refresh:
    - `scripts/ia64-unimpl-report.sh --format tsv --top 50 ...`
      reported no `IA64 UNIMPL` lines in fresh smoke logs.

## Follow-up (P3-R `TB_EXIT_REQUESTED` Assert Root-Cause + Translator Contract Fix)
- Root-caused the host assert to a translator contract bug in
  `target/ia64/translate.c`:
  - `break.m` hypercall decode marked `DISAS_NORETURN` on a mixed runtime path
    that can either:
    - call `breaki` (no return), or
    - call `fw_break0` (returns normally when `fw_preboot_active != 0`).
  - with `DISAS_NORETURN` set, TB fallthrough reaches generic
    `TB_EXIT_REQUESTED` epilogue (`gen_tb_end`) without `exit_request/icount`,
    triggering `cpu_loop_exec_tb` assert.
- Correctness fixes:
  - `HELPER(unimpl)` is now explicitly no-return:
    - `DEF_HELPER_FLAGS_5(..., TCG_CALL_NO_RETURN, noreturn, ...)`
    - `G_NORETURN` helper signature + `g_assert_not_reached()` tail.
  - `gen_unimpl(...)` no longer emits `tcg_gen_exit_tb(NULL, 0)` after
    no-return helper call.
  - `break.m` mixed `fw_break0`/`breaki` path no longer sets
    `ctx->base.is_jmp = DISAS_NORETURN`.
  - assert diagnostics in `cpu_loop_exec_tb` now include `exception_index`.
- Validation (2026-02-18, `scratch/ia64_logs/p3r/commit3_safe_off.out`):
  - repro knobs:
    - `IA64_PEI_22560_STATUS_FIX=1`
    - `IA64_PEI_279D0_TRACE=1`
    - `IA64_PEI_279D0_STATUS_FIX=1`
    - `IA64_PEI_279D0_SAFE_MODE=0`
  - result: `rc=124` (timeout), no host assert.
  - evidence from `scratch/ia64_logs/qemu.fw.log`:
    - bounded rewrite still fires:
      `pei_279d0_status ... fixed=1 ... safe_mode=0 safe_quarantine=0`
    - no `TB_EXIT_REQUESTED without exitreq/icount` lines.
    - execution continues past prior crash point and now exposes guest-side
      illegal-op -> break-vector loop:
      `IA64 UNIMPL ... pc=80000000ffffb2b0 ...` then repeated
      `IA64 fault vec=0x2c00 ip=0x2c00`.
- Regression/smoke:
  - `scripts/run-ia64-op7-tests.sh 12s` passed.

## Follow-up (P3-S A10/A11 Packed Shift+Add + I-slot break(0) Contract Parity)
- Extended A-unit packed shift+add decode coverage in `target/ia64/translate.c`:
  - added A10/A11 `pshladd2`/`pshradd2` handling for major `0xF`
    (`op={4,7}` for `pshladd2`, `op={5,6}` for `pshradd2`).
  - corrected `count2` decode to architectural range `[1..3]`
    (`x2b + 1`, guarded by `x2b < 3`) for both major `0x8` and `0xF` forms.
  - enabled major `0xF` A-unit dispatch in I-slot and M-slot pre-decode.
- Added directed selftest coverage in `scripts/ia64-op7-selftest.S`:
  - `pshladd2 r16 = r14, 1, r15`
  - `pshradd2 r16 = r14, 1, r15`
- Hardened I-slot `break.i` hypercall decode to match M-slot mixed-path rules:
  - firmware `break(0)` now routes through `fw_break0` in I-slot hypercall
    encodings, avoiding immediate break-vector recursion.
  - mixed return/no-return path no longer relies on unconditional
    `DISAS_NORETURN`.
- Validation (2026-02-18):
  - Build:
    - `ninja -C build -j4 qemu-system-ia64` passed.
  - Directed tests:
    - `scripts/run-ia64-op7-tests.sh 12s` passed.
  - Firmware repro (`IA64_GUEST_ERRORS=1`, `...279D0_SAFE_MODE=0`, 45s):
    - `rc=124` (no host assert).
    - `IA64 UNIMPL` at `pc=80000000ffffb2b0` is gone.
    - prior break-vector `0x2c00` recursion is no longer the first blocker.
    - new first blocker in this mode is:
      - `IA64 UNIMPL: pc=80000000ffffb2f0 ... reserved template`
      - followed by general-exception vector handling (`vec=0x5400`).

## Remaining Open Work
- F-unit coverage remains partial; many encodings still fall through to
  `gen_unimpl("F-slot")`.
- Firmware PEI/DXE blockers remain tracked in `docs/ia64-firmware-blockers.md`.
- Host-side `TB_EXIT_REQUESTED` assert is fixed; next blocker is guest-side
  `IA64 UNIMPL` at `0x80000000ffffb2f0` (`reserved template`) reached after
  the former `0xffffb2b0`/`0x2c00` path is cleared in
  `...279D0_SAFE_MODE=0` runs.
