# IA-64 Firmware Blockers

This file is the rolling blocker evidence log referenced by
`IA64_ROADMAP.md`. Keep this focused on current symptoms, repro facts, and
investigation breadcrumbs.

- DXE early failure now splits by HOB patch mode:
  - default mode (`QEMU_IA64_EFI_HOB_PATCH` off): still reaches
    `ASSERT ... Core\Dxe\Gcd\Gcd.c Line 1736, Descrpt: Found`.
  - HOB patch mode (`QEMU_IA64_EFI_HOB_PATCH=1`): synthetic tested sysmem
    resource HOB is now inserted (`RES type=0 attr=0x00002007 tested=1`), and
    execution advances past the prior `Gcd.c:1736` stop.
  - bounded post-GCD null-call repair is retained as a bisect guard but is now
    default-off:
    `QEMU_IA64_CALL_NULL_FIX=1` (opt-in) repairs the historical
    `pc=0x1ff4f520` null `br.call` path.
  - current 90s HOB-patch run window with default-off call fix shows no
    `call_null_fix` and no null-branch abort; this path is not currently
    reproducing in the active baseline.
  Root-cause focus remains on DXE progression and reproducing the exact
  post-GCD call-path failure with current firmware state.
- Byte-copy helper around `pc=0xffe7b1e0` is currently treated as a
  non-blocking trace target (not root cause yet):
  - Probes are wired at:
    - `0xffe24bb0/0xffe24bc0` (`GetHobList` store site)
    - `0xffe7b220/0xffe7b280/0xffe7b2a0/0xffe7b2e0/0xffe7b310/0xffe7b3a0`
  - New interpretation from disassembly/probes:
    - The `slot+0x08 == 0xffffffffffffffff` observation can be a normal
      zero-count exit transient (`cmp.eq` at `0xffe7b290` followed by
      decrement/store at `0xffe7b2a0/0xffe7b2b0`).
    - Triggered "pointer-like count" at loop/body PCs is therefore a
      false-positive for corruption.
  - Trigger mode remains useful, but now keys off setup-stage count only:
    - `QEMU_IA64_FW_PEI_COPY_TRACE=1`
    - `QEMU_IA64_FW_PEI_COPY_TRACE_TRIGGER=1`
    - optional `QEMU_IA64_FW_PEI_COPY_TRACE_PTR_MIN=<hex>`
  - Optional tooling:
    - `scripts/ia64-pei-copy-report.sh scratch/ia64_logs/qemu.fw.log`
- PHIT memory range mismatch: EFI HOB dump shows `mem=[7fffffff1f000000..7fffffff20000000]` (16MiB) even with `-m 512M`; serial shows `Install PeiMemory ... size = 0x1000000`. Investigate why PEI/PHIT is shrinking memory.
- HOB list consistency: confirm the HOB list used by DXE matches the list patched by QEMU (PEI list cloned to 0x3030000). If DXE uses a different list, resource HOB fixes won't be visible.
- FlashMap entries: multiple GUIDed HOBs for FlashMap exist; decode entries to confirm `EFI_FLASH_AREA_EFI_VARIABLES` has correct base/length.
- PEI PPI assert: current run loops in `fw_pei_err` with `EFI_NOT_FOUND`
  (`0x800000000000000e`) / `EFI_END_OF_MEDIA` (`0x800000000000001c`)
  around `pc=0xffe268b0..0xffe27400` after `InstallPeiMemory`. Identify which
  PPI/notify/dispatch path is returning the error; use PPI list/dispatch
  tracing.
- First-bad provenance (2026-02-16 run):
  - `fw_pei_first_bad` reports first `0x800000000000001c` at
    `pc=0xffe22560` (`site=err_watch`) with call frame resolving through
    `PeiServices`.
  - First StatusCode `PeiLocatePpi` calls still return
    `EFI_NOT_FOUND (0x800000000000000e)` before later succeeding after more
    PPI install/dispatch.
  - A corrective locate-return path (`QEMU_IA64_PEI_LOCATE_FIX=1` default)
    is wired, but has not yet moved the current DXE `Gcd.c:1736` assert.
- First `0x1c` producer path is now concretely traced:
  - Transition point:
    - `fw_pei_status_transition pc=0xffe22560 prev=EFI_NOT_FOUND -> new=EFI_END_OF_MEDIA`.
  - Immediate pre-transition path (from producer ring dump):
    - `notify_ppi` call: `pc=0xffe278a0 -> tgt=0xffe268d0`
      - descriptor: `notify_fn=0xffe2efb0`
      - GUID: `1388066e-3a57-4efa-98f3-c12f3a958a29`
    - Internal chain then flows through:
      - `0xffe26ea0 -> 0xffe2bcb0`
      - `0xffe2bd90 -> 0xffe2db30`
      - `0xffe2dbd0 -> 0xffe2da10`
      - `0xffe2dad0 -> 0xffe2caf0`
    - followed by `report_status_code` (`0xffe2be10 -> 0xffe22330`) and
      `locate_ppi` (`0xffe22410 -> 0xffe26510`, StatusCode GUID
      `229832d3-7a30-4b36-b827-f40cb7d45436`).
  - Next blocker step should target the notify callback/dispatch chain above,
    not only `PeiLocatePpi` return rewriting.
- Notify-return instrumentation/fix probe (2026-02-18):
  - New bounded notify-return tracing is wired on
    `NotifyPpi` call/return (`ret_restore`/`ret_restore_b0`) with GUID decode.
  - Traced target callback return (`call_pc=0xffe278a0`,
    `tgt=0xffe268d0`, GUID `1388066e-3a57-4efa-98f3-c12f3a958a29`) returns
    `status=0` (`soft=0`) in current baseline windows.
  - As a result, bounded notify-return rewrite
    (`QEMU_IA64_PEI_NOTIFY_STATUS_FIX=1`) does not trigger
    (`notify_fixed=0`) and does not alter the observed
    `pc=0xffe22560` `EFI_NOT_FOUND -> EFI_END_OF_MEDIA` transition.
  - Updated focus: status mutation to `0x1c` occurs after notify return; next
    corrective hook should target the post-notify/report-status chain
    (`pc=0xffe22560` path), not notify return itself.
- `pc=0xffe22560` status-mutation attribution/fix probe (2026-02-18):
  - New bounded trace/fix path is wired at the mutation site:
    - `QEMU_IA64_PEI_22560_TRACE=1`
    - `QEMU_IA64_PEI_22560_STATUS_FIX=1`
  - Fix guard conditions require all of:
    - soft EFI status in `r8`
    - unresolved optional StatusCode path
    - recent producer-chain match (`report_status_code` + `locate_ppi`
      with StatusCode GUID `229832d3-7a30-4b36-b827-f40cb7d45436`)
      within bounded sequence window.
  - A/B evidence (`scratch/ia64_logs/p3o/*`, 45s windows):
    - baseline: `transition_22560=4`, `22560_trace=0`, `22560_fixed=0`
    - trace-only: `transition_22560=4`, `22560_trace=4`, `22560_fixed=0`
    - fix-on: `transition_22560=5`, `22560_trace=5`, `22560_fixed=5`
  - Result:
    - target path at `pc=0xffe22560` is now actively recognized and rewritten
      under bounded guards (`pei_22560_status ... fixed=1 ... chain=1`).
    - residual first-bad in `sem0_fix_on` comes from a later recurrence with
      changed context (post transition through non-EFI `r8=ffffffff0011fff0`),
      so blocker root-cause is narrowed but not fully eliminated.
- `pc=0xffe279d0..0xffe27a10` non-EFI status probe (2026-02-18):
  - Added bounded attribution/fix hooks:
    - `QEMU_IA64_PEI_279D0_TRACE=1`
    - `QEMU_IA64_PEI_279D0_STATUS_FIX=1`
  - Trace confirms post-`22560` mutation recurrence:
    - `fw_pei_status_transition pc=0xffe279d0 ... new=ffffffff0011fff0`
    - `fw_pei_status_transition pc=0xffe279f0 ... new=ffffffff0011bff0`
  - Guarded rewrite path links to recent successful `22560` chain context
    (`seq_link=1`, `delta=10`, `ps_match=1`) and rewrites this signature
    when enabled.
  - A/B evidence (`scratch/ia64_logs/p3p/*`, 45s windows):
    - `p3p_trace_only`: `trace_279d0=4`, `fix_279d0=0`,
      `err_279d0_window=5`, `rc=124`.
    - `p3p_fix_on`: `trace_279d0=1`, `fix_279d0=1`,
      `err_279d0_window=3`, `rc=134`.
  - New blocker uncovered:
    - with `...279D0_STATUS_FIX=1`, firmware run hits host assert:
      `TCG BUG: TB_EXIT_REQUESTED without exitreq/icount`
      (`cpu_loop_exec_tb` assertion in `accel/tcg/cpu-exec.c`).
  - Current stance:
    - `279d0` rewrite remains default-off and investigation-only.
    - immediate next blocker is the TCG assert path reached after this bounded
      rewrite, before additional PEI/DXE forward progress can be claimed.
- `pc=0xffe279d0` safe-mode quarantine guard (2026-02-18):
  - Added new safe-mode controls:
    - `QEMU_IA64_PEI_279D0_SAFE_MODE` (default on)
    - `QEMU_IA64_PEI_279D0_SAFE_MODE_LOG_LIMIT`
  - Safe mode now quarantines the `279d0` rewrite path even when
    `...279D0_STATUS_FIX=1`:
    - mutation context is still traced (`pei_279d0_status ...`)
    - helper captures a probe bundle at `0x80000000ffffb2b0` /
      `0x00000000ffffb2b0` (`pei_279d0_status safe_probe ...`)
    - status rewrite is suppressed (`safe_quarantine=1`, `fixed=0`)
  - A/B evidence (`scratch/ia64_logs/p3q/*`, 45s windows):
    - `safe_default`
      (`22560 fix on`, `279d0 trace on`, `279d0 fix on`, safe-mode default):
      `rc=124`, `trace_279d0=4`, `fix_279d0=0`, `safe_quarantine=4`,
      no host assert.
    - `safe_off`
      (`...279D0_SAFE_MODE=0`):
      `rc=134`, `fix_279d0=1`, host assert reproduced
      (`TB_EXIT_REQUESTED without exitreq/icount`).
  - Current stance:
    - keep safe mode default-on while `...SAFE_MODE=0` remains an
      investigation mode.
    - retain `...SAFE_MODE=0` as repro switch for post-quarantine behavior.
- `TB_EXIT_REQUESTED` host assert root-cause/fix (2026-02-18):
  - Root cause:
    - `target/ia64/translate.c` marked `DISAS_NORETURN` in a `break.m`
      mixed path that can return via `fw_break0` when
      `fw_preboot_active != 0`.
    - this allowed TB fallthrough into the generic exit-request epilogue and
      triggered:
      `TB_EXIT_REQUESTED without exitreq/icount`.
  - Fixes:
    - `HELPER(unimpl)` is now declared/compiled as no-return
      (`TCG_CALL_NO_RETURN`, `G_NORETURN`), and `gen_unimpl()` no longer emits
      an extra `exit_tb` after calling it.
    - removed `DISAS_NORETURN` from the mixed `break.m` `fw_break0`/`breaki`
      path so only true no-return paths are marked no-return.
  - Validation (`scratch/ia64_logs/p3r/commit3_safe_off.out`, 45s):
    - `...279D0_STATUS_FIX=1 ...279D0_SAFE_MODE=0` now runs to timeout
      (`rc=124`) with no host assert.
    - bounded rewrite still fires (`pei_279d0_status ... fixed=1 ...`).
  - Next blocker exposed:
    - run now reaches guest-side `IA64 UNIMPL` at
      `pc=80000000ffffb2b0` and then loops in break vector `0x2c00`
      (repeated `fw_break0`/`fault vec=0x2c00`).
- `pc=0x80000000ffffb2b0` A10/A11 decode + I-slot break(0) parity pass
  (2026-02-18):
  - `target/ia64/translate.c` now covers A10/A11 `pshladd2`/`pshradd2` in the
    major-`0xF` A-unit form and maps `count2` as `[1..3]` (`x2b + 1`).
  - major `0xF` A-unit dispatch is now enabled for I-slot predecode.
  - I-slot `break.i` hypercall decode now mirrors M-slot mixed-path handling
    so firmware `break(0)` routes through `fw_break0` instead of immediately
    faulting into `0x2c00`.
  - validation (`IA64_GUEST_ERRORS=1`, `...279D0_SAFE_MODE=0`, 45s):
    - `rc=124`, no host assert.
    - prior `IA64 UNIMPL pc=...ffffb2b0` is cleared.
    - prior repeated `fault vec=0x2c00 ip=0x2c00` loop is no longer the first
      blocker.
    - new first blocker is:
      - `IA64 UNIMPL: pc=80000000ffffb2f0 ... reserved template`
      - then illegal-op/general-exception path at `vec=0x5400`.
- `fw_break0` strict-gate allowlist alignment (2026-02-18):
  - `target/ia64/translate.c` now allowlists known break(0) gate regions
    (canonical physical space) instead of a single PC:
    - ROM gate cluster: `0x00000000ffffb2a0..0x00000000ffffb2df`
      (covers observed `...b2a0` and `...b2d0`).
    - SAL/PCI work-RAM stubs:
      `0x0000000100003000..0x00000001000031ff`
      (covers observed `0x100003000`, `0x1000030c0`, `0x100003120`).
  - keeps `QEMU_IA64_FW_BREAK0_STRICT_GATE` default-on semantics, while
    restoring legitimate firmware break(0) hooks that regressed with the
    single-PC gate.
  - validation (`scratch/ia64_logs/p3t_strict_gate_safeoff.*`, 30s):
    - `rc=124`.
    - no early `fault vec=0x2c00 ip=0x100003000` recursion.
    - first blocker remains the expected:
      `IA64 UNIMPL: pc=80000000ffffb2f0 ... reserved template`,
      then `vec=0x5400`.
- ROM `break(0)` gate return semantics + provenance (2026-02-18):
  - `target/ia64/translate.c` now adds a ROM-gate return path for
    strict-allowlisted `break(0)` sites:
    - when `pc in 0x00000000ffffb2a0..0x00000000ffffb2df`,
      `fw_break0` now resumes at `b0` (`pc = b0 & ~0xF`, `ri = 0`)
      instead of falling through into gate data.
    - env knob: `QEMU_IA64_FW_BREAK0_GATE_RETURN` (default: on).
  - `target/ia64/helper.c` adds focused ROM-gate trace hooks:
    - `QEMU_IA64_FW_BREAK0_GATE_TRACE`
    - `QEMU_IA64_FW_BREAK0_GATE_TRACE_LIMIT`
    - trace line tag: `fw_break0_gate rom_gate_break0 ...`
  - validation (safe-off repro):
    - fix on (`scratch/ia64_logs/p3u_gate_ret_safeoff.*`, 45s):
      - `rc=124`.
      - `IA64 UNIMPL pc=...ffffb2f0` and `vec=0x5400 ip=...ffffb2f0`
        are cleared.
      - follow-up exposed new return-path blocker:
        `fault vec=0x2c00 ip=0x280`
        (last branch `from=0xffe70310 to=0x280`).
    - fix off (`scratch/ia64_logs/p3u_gate_ret_off_safeoff.*`, 30s):
      - reproduces prior blocker:
        `IA64 UNIMPL pc=...ffffb2f0` then `vec=0x5400`.
- ROM gate return-path unwind fix (2026-02-18):
  - `target/ia64/translate.c`
    - ROM gate return fastpath now executes `ret_restore_b0` before jumping to
      `b0`, so call-gate returns unwind modeled stacked-register state.
  - `target/ia64/helper.c`
    - fixed `ret_restore_b0` non-pop tail path so it stores the active frame
      (removed dead nested `if (do_pop) { if (!do_pop) ... }` logic).
  - validation (safe-off repro):
    - `IA64_GUEST_ERRORS=1 IA64_PEI_22560_STATUS_FIX=1`
      `IA64_PEI_279D0_TRACE=1 IA64_PEI_279D0_STATUS_FIX=1`
      `IA64_PEI_279D0_SAFE_MODE=0 timeout 90s scripts/run-ia64-firmware.sh`
      -> `rc=124`.
    - evidence in `scratch/ia64_logs/qemu.fw.log`:
      - gate edge now unwinds:
        `ret_restore_b0 ip=0x80000000ffffb2a0 ... pop=1`
      - prior blocker cleared:
        no `fault vec=0x2c00 ip=0x280` and no `IA64 UNIMPL` in the 90s window.
- `279d0` rewrite/link hardening + optional-path predicate tightening
  (2026-02-18):
  - `target/ia64/helper.c`:
    - `pc=0xffe279d0..0xffe27a10` rewrite now requires `22560` context
      register match (`r33/r34`) and a local producer chain proof
      (`locate_ppi` StatusCode GUID + nearby `report_status_code`)
      in addition to existing seq/PS linkage.
    - added explicit bounded `pei_279d0_event` provenance ring with
      per-hit gate decisions and linked `22560` context snapshots.
    - first-bad dumps now include recent `pei_279d0_event` history.
  - shared `statuscode_optional_path` eligibility evaluator now gates both:
    - `pei_report_status_fix` rewrite
    - `fw_pei_first_bad_skip` suppression.
  - tightened semantic rewrite predicate now requires:
    - unresolved optional path
    - valid PEI service table pointer
    - return PC in PEI firmware region
    - StatusCode type class in `1..3`.
  - validation (`IA64_GUEST_ERRORS=1 IA64_PEI_22560_STATUS_FIX=1 IA64_PEI_279D0_TRACE=1 IA64_PEI_279D0_STATUS_FIX=1 IA64_PEI_279D0_SAFE_MODE=0 timeout 45s scripts/run-ia64-firmware.sh`):
    - `rc=124`, no host assert.
    - `pei_279d0_status` now records `fixed=0` with
      `ctx_reg_match=0 ctx_link=0` on the observed mismatched context path.
    - no semantic rewrite observed at non-PEI return PCs
      (for example `ret_pc=0x000000001fff01d0` no longer rewritten).
- PEI PS-epoch provenance + strict selector coupling (2026-02-18):
  - `target/ia64/helper.c`:
    - added bounded `pei_ps_epoch` ring for PS/core provenance captured from
      producer calls and current-PS selector decisions.
    - first-bad dumps now include `pei_ps_epoch_history`.
    - added strict selector knob (default on):
      `QEMU_IA64_PEI_PS_SELECT_STRICT`.
    - strict selector prefers validated arg-derived PS (`r32`/`r33`) over
      stale cached PS and uses producer-chain/dispatched-state tie-breaks.
  - rewrites now couple to selected PS epoch:
    - `pc=0xffe22560` fix requires epoch match unless forced.
    - semantic `statuscode_optional_path` rewrite/skip requires epoch match
      in strict mode.
    - `279d0` `do_fix_ready` now includes PS-epoch continuity
      (`ps_epoch_seen/match/link`).
    - mismatch logs are explicit:
      - `pei_22560_status reject=ps_epoch_mismatch ...`
      - `pei_report_status_fix reject=ps_epoch_mismatch ...`
  - validation (`IA64_GUEST_ERRORS=1 IA64_PEI_22560_STATUS_FIX=1 IA64_PEI_279D0_TRACE=1 IA64_PEI_279D0_STATUS_FIX=1 IA64_PEI_279D0_SAFE_MODE=0 timeout 45s scripts/run-ia64-firmware.sh`):
    - `rc=124`, no host assert.
    - `pei_22560_status ... epoch_seen=1 epoch_match=1 ...` on matched path.
    - stale-epoch tail recurrences are now rejected/logged instead of silently
      rewritten.
- Firmware progress-loop telemetry + bounded rewrite de-dup (2026-02-18):
  - `target/ia64/helper.c`:
    - added bounded `fw_progress` marker ring and optional loop detector for
      repeated `break(0)`/StatusCode signatures.
    - added rewrite de-dup guard (default on) for both:
      - `pc=0xffe22560` rewrite path
      - semantic `pei_report_status_fix` rewrite path.
    - first-bad optional-path skip logs now include rewrite de-dup state:
      `rewrite_dedup`, `rewrite_dedup_delta`, `rewrite_fp`.
  - knobs:
    - `QEMU_IA64_PEI_PROGRESS_TRACE`
    - `QEMU_IA64_PEI_PROGRESS_LOOP_TRACE`
    - `QEMU_IA64_PEI_STATUS_REWRITE_DEDUP`
    - `QEMU_IA64_PEI_STATUS_REWRITE_DEDUP_WINDOW`.
  - validation (`IA64_GUEST_ERRORS=1 IA64_PEI_22560_STATUS_FIX=1 IA64_PEI_279D0_TRACE=1 IA64_PEI_279D0_STATUS_FIX=1 IA64_PEI_279D0_SAFE_MODE=0 timeout 45s scripts/run-ia64-firmware.sh`):
    - `rc=124`, no host assert.
    - evidence now shows bounded duplicate suppression:
      - `pei_report_status_fix reject=dedup_repeat ... dedup_delta=...`
      - `fw_pei_first_bad_skip ... rewrite_dedup=1 ...`
- Rewrite de-dup transition safeguards/logging (2026-02-18):
  - `target/ia64/helper.c` now classifies dedup transition causes for
    rewrite fingerprints:
    - context transitions:
      `ret_pc`, `ps_ptr`, `type`, `value`, `instance`
    - sequence transitions:
      `seq_gap`, `seq_rewind`.
  - transition logs now fire on dedup resume paths for both rewrite sites:
    - `pei_22560_status dedup_transition=...`
    - `pei_report_status_fix dedup_transition=...`
    - each log includes `prev_fp/new_fp`, `prev_seq/new_seq`,
      `dedup_window`.
  - validation (`IA64_GUEST_ERRORS=1 IA64_PEI_22560_STATUS_FIX=1 IA64_PEI_279D0_TRACE=1 IA64_PEI_279D0_STATUS_FIX=1 IA64_PEI_279D0_SAFE_MODE=0 timeout 45s scripts/run-ia64-firmware.sh`):
    - `rc=124`, no host assert.
    - evidence in `scratch/ia64_logs/qemu.fw.log`:
      - `pei_22560_status dedup_transition=type|value ...`
      - `pei_22560_status dedup_transition=seq_gap ...`
      - `pei_report_status_fix dedup_transition=seq_gap ...`
      - `pei_report_status_fix dedup_transition=ret_pc|type|value ...`
- StatusCode semantic handling is now wired (default on):
  - `QEMU_IA64_PEI_STATUSCODE_SEMANTIC_FIX=1` treats unresolved StatusCode
    report path as optional and rewrites return status at the
    `report_status_code` boundary:
    - `pei_report_status_fix reason=statuscode_optional_path ...`
  - First-bad capture for this optional path is now explicitly suppressed:
    - `fw_pei_first_bad_skip reason=statuscode_optional_path ...`
  - Legacy knobs remain for bisecting compatibility:
    - `QEMU_IA64_PEI_REPORT_STATUS_SOFTFAIL`
    - `QEMU_IA64_PEI_REPORT_STATUS_SOFTFAIL_ALWAYS`
- PEI core HOB-pointer slot seeding now keeps both core slots coherent
  (`+0x260` and `+0x470`) for initial and relocated core instances:
  - `ia64_fw_pei_seed_core_hob_field()` now seeds missing/invalid
    `+0x260`/`+0x470` from validated `core+0x260/+0x470/+0x478` or cached HOB.
  - `fw_pei_hob_init_fix` now calls this seeding path after core discovery.
  - Evidence:
    - `pei_core_hob_seed core=... off=0x260 old=0 ... new=0x40ef000 ...`
    - `pei_core_hob_seed core=... off=0x470 old=0 ... new=0x40ef000 ...`
- PEI HOB flow contract tracing/repair remains available as a bisect guard
  (default off):
  - `QEMU_IA64_PEI_HOB_FLOW_TRACE=1` logs `GetHobList`/`CreateHob` call-return
    pointer contract with core/HOB provenance (`pei_hob_flow ...`).
  - `QEMU_IA64_PEI_HOB_PTR_FIX=1` repairs successful `GetHobList` returns when
    output HOB pointer is null/invalid
    (`pei_hob_ptr_fix reason=get_hob_list_null_or_invalid_out ...`).
  - `QEMU_IA64_PEI_CREATE_HOB_PTR_GUARD=1` provides a bounded guard for the
    `CreateHob` OOR+null-out case (pointer repair only; no status rewrite).
  - A/B evidence (2026-02-17):
    - fixes off (`...HOB_PTR_FIX=0 ...CREATE_HOB_PTR_GUARD=0`) after slot
      coherence seeding: no early `pc=0xffe24e80` transition in repeated
      baseline windows, and no `pei_hob_ptr_fix` events required.
    - defaults were flipped to off to keep root-cause behavior primary and
      retain repair logic only for bisect compatibility.
  - Root-cause update:
    - early OOR path was driven by core `+0x260` being left null/invalid while
      `+0x470` was valid; `GetHobList`/`CreateHob` consumers on that path read
      the stale `+0x260` slot.
    - bounded HOB out-pointer repair remains as compatibility guard/bisect aid.
- Tested sysmem HOB coverage fix is now in place for HOB patch mode:
  - system-memory synthesis now validates coverage against PHIT physical
    `EfiFreeMemoryBottom..Top` and inserts when missing.
  - evidence:
    - `hob_patch: inserted 1 sysmem resource HOB(s)`
    - `efi_hob_dump: RES type=0 attr=0x00002007 tested=1 ...`
