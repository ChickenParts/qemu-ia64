# IA-64 Roadmap (Q2 2026)

## Scope and Intent

This document is the canonical execution roadmap for IA-64 bringup work from
2026-02-16 through 2026-05-15.

Priority order for this quarter:
1. UNIMPL coverage and decode/execute forward progress.
2. PEI/DXE progression blockers.
3. Correctness debt (NaT/RSE/ALAT) without regressing 1-2.

Status snapshots remain in `status_*.md`; detailed blocker evidence remains in
`docs/ia64-firmware-blockers.md`.

## Baseline (As of 2026-02-16)

- Current status snapshot: `status_2_2026.md`.
- Known hit UNIMPLs include:
  - `pc=a00000010002e240 ri=2 insn=14000000202 A-slot`
  - `pc=a00000010114d4a0 ri=0 insn=01018202830 M-slot`
- Current active blockers are tracked in
  `docs/ia64-firmware-blockers.md`.
- Existing bringup debt and fixed/open lists are tracked in
  `docs/ia64-firmware-todo.md` and `IA64-AUDIT.md`.

## Workstreams

### WS1: UNIMPL Coverage (Primary)

Goal:
- Remove fatal UNIMPL stops on the active firmware and kernel handoff paths.

Outputs:
- Ranked UNIMPL hit table (pc/ri/opcode/slot/hits).
- Batched instruction implementation plan (A, B, C).
- Reduced fatal UNIMPL incidence in baseline runs.

### WS2: Firmware Progression

Goal:
- Advance stable PEI to DXE progression once top UNIMPL stops are removed.

Outputs:
- Resolved or narrowed root causes for:
  - PEI PPI assert loop.
  - PHIT memory range mismatch and HOB consistency.
  - Byte-copy loop argument/stack-slot corruption near `pc=0xffe7b1e0`.

### WS3: Correctness Debt

Goal:
- Improve architectural correctness after boot-path progress stabilizes.

Outputs:
- Incremental NaT propagation expansion.
- RSE dirty/clean + loadrs/flushrs behavior improvements.
- ALAT handling improvements where currently minimal.

### WS4: Regression Discipline

Goal:
- Keep progress measurable and prevent silent backslides.

Outputs:
- Repeatable evidence bundle per checkpoint.
- Stable comparison against a recorded baseline run set.

## Phase Plan and Gates

### Phase 1: UNIMPL Inventory and Triage (2026-02-16 to 2026-03-06)

Execution:
1. Run baseline workflows with:
   - `scripts/run-ia64-firmware.sh`
   - `scripts/run-ia64-kernel.sh`
2. Collect UNIMPL events from run logs into a ranked table using
   `scripts/ia64-unimpl-report.sh`.
3. Map top hit PCs/opcodes to decode locations in `target/ia64/translate.c`.
4. Partition work:
   - Batch A: fatal boot-path UNIMPLs.
   - Batch B: frequent recurring non-fatal hits.
   - Batch C: long-tail low-frequency paths.

Gate P1:
- Ranked UNIMPL table exists and is reproducible from logs.
- Top 10 firmware-path UNIMPL sites have decode ownership mapping.

### Phase 2: UNIMPL Implementation Sprint (2026-03-09 to 2026-04-03)

Execution:
1. Implement/stub Batch A with clear semantics notes.
2. Keep deterministic behavior for deferred semantics and log once per site.
3. Re-run firmware/kernel baselines after each instruction cluster lands.
4. Refresh blocker table after each cluster.

Gate P2:
- The two baseline fatal UNIMPL PCs listed above are no longer fatal stops.
- Fatal UNIMPL stop frequency drops by at least 70% vs Phase 1 baseline set.

### Phase 3: Firmware Blocker Convergence (2026-04-06 to 2026-04-24)

Execution:
1. Triage remaining blockers in order:
   - `fw_pei_err` PPI assert loop.
   - PHIT and active HOB list consistency.
   - Byte-copy loop corruption.
2. Use existing tracing/probing hooks in `target/ia64/helper.c` and
   `target/ia64/translate.c`.
3. Validate that DXE consumes the expected HOB/resource state before each fix.

Gate P3:
- Firmware progresses past the current assert/hang point in at least one stable
  repro path.
- PHIT/HOB evidence matches configured memory expectations for baseline runs.

### Phase 4: Correctness Hardening (2026-04-27 to 2026-05-15)

Execution:
1. Start NaT/RSE/ALAT improvements only after P3 is met.
2. Add regression checks to keep P2/P3 behavior stable.
3. Publish quarter-end status and carry-over list.

Gate P4:
- No regression against P2/P3 gates.
- At least one correctness area (NaT or RSE) shows measurable improvement with
  reproducible evidence.

## Interfaces and Compatibility

Public interfaces:
- No QAPI or machine-type user-visible interface changes are required by this
  roadmap itself.

Project interfaces:
- `IA64_ROADMAP.md` is the canonical roadmap.
- `status_*.md` remains periodic snapshot reporting.
- `docs/ia64-firmware-blockers.md` remains detailed blocker evidence log.

## Test and Evidence Matrix

Use these scenarios in every checkpoint:
1. Baseline firmware run (`scripts/run-ia64-firmware.sh`).
2. Baseline kernel handoff run (`scripts/run-ia64-kernel.sh`).
3. UNIMPL regression check against known historical PCs/opcodes.
4. PEI/DXE blocker traces for active issues:
   - PPI dispatch/lookup context.
   - PHIT/HOB dumps and list consistency checks.
   - Targeted watchpoints for byte-copy corruption path.
5. Noise control run with firmware fastpath disabled (default).

## Risks and Mitigations

- Risk: Long-tail instruction semantics consume schedule with little boot gain.
  - Mitigation: Keep Batch A strict; defer Batch C aggressively.
- Risk: HOB patching masks root causes.
  - Mitigation: Keep patching default-off unless a checkpoint explicitly requires
    it.
- Risk: Debug signal volume obscures root cause.
  - Mitigation: Use fixed trace profiles and a capped evidence bundle per run.
- Risk: Correctness changes destabilize progress.
  - Mitigation: Phase-gate correctness work behind P3 and re-run baseline checks
    for each correctness change.

## Cadence

Weekly cadence:
1. Monday: baseline run set and UNIMPL summary refresh.
2. Midweek: instruction/blocker implementation batch.
3. Friday: gate check and status snapshot update.

Evidence bundle per checkpoint (in `scratch/ia64_logs/`):
- Latest firmware serial log.
- Latest kernel serial log.
- UNIMPL ranked summary (for example:
  `scripts/ia64-unimpl-report.sh --format tsv --top 50`).
- Blocker delta summary (new/resolved/unchanged).

## Defaults and Assumptions

- Quarter scope is fixed to 2026-02-16 through 2026-05-15.
- UNIMPL-first priority is fixed for this roadmap revision.
- Existing local modifications in `target/ia64/helper.c` and the new
  `status_2_2026.md` are treated as active baseline context.
