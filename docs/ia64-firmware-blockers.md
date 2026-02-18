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
