# Merced ITC.D data translation, Page-Not-Present and AR/A/D fault order

Directly stacked on PR #54, without ancestor merges or branch rewrites.

## Architectural oracle

Intel Itanium Architecture SDM rev2.3 Vol2 §4.1.1.6, §4.1.8
(p2:69–71), Table 4-4 and §8 vector definitions require **after
successful TLB match**, the priority:

1. Page Not Present (`P=0`) → **0x5000**.
2. NaT page, protection key and key permissions (not yet implemented by
   this QEMU target).
3. Page access rights / privilege denial → **0x5300 data**,
   **0x5200 instruction**.
4. Accessed bit `A=0` → **0x2800 data**, **0x2400 instruction**.
5. Dirty bit `D=0` on a permitted store → **0x2000**.

An ITC.D insert with `P=0` must retain a *valid matching translation*
whose page-present bit is clear; `P` is not the internal TLB entry-valid
bit. This distinguishes 0x5000 from 0x800 DTLB miss. The current
`ia64_insert_tlb()` discarded `P=0` entries, and the TLB-hit path
incorrectly selected A/D faults *before* page-access rights.

The patch corrects these two behaviors **within the already implemented
fault classes only**. It does not assert that key permissions, NaT-page,
RSE mandatory references, VHPT walkers, data unimplemented-address
faults, and instruction/data cross-domain protection are complete.

## Actual GNU-assembled fault/commit fixtures

`scripts/run-ia64-dtlb-fault-priority-tests.py` drives 15 firmware-free
real-IVT profiles with a clean `qemu-system-ia64`:

- True `itc.d` 16KiB mappings of a virtual data address to a distinct,
  16KiB-aligned physical data page, at `PSR.dt=1`. Region 6/7 mapped
  load/store controls verify **explicit DTLB translation wins** over
  Linux direct-map compatibility shortcuts, and a region-5 high-canonical
  data-VA load guards the upper sign-extended space.
- Missing ITC.D load/store controls require **0x800** DTLB fault.
- Read-only `AR=1` stores with `A=D=0` or `D=0` must take **0x5300**,
  not 0x2800 or 0x2000. Positive read-only reads must still work.
- Writable `AR=2` load/store with `A=0` must take **0x2800**; a
  permitted store with `A=1,D=0` must take **0x2000**.
- Two `P=0` data translations, including a protected would-be store,
  must take **0x5000**, not a DTLB miss or an AR/A/D fault.

On each fault, the real handler asserts the vector, faulting data
`CR.IFA`, faulting source `CR.IIP/IPSR.ri=0`, ISR read vs write flags,
unchanged register and physical data, and **one repaired native `rfi`**.
Success cases check the committed loaded value or physical store.
All fixtures set saved `PSR.dt=1` via `CR.IPSR`+native `rfi`,
without relying on `ssm/rsm` to *enable* the high PSR flags.

Python source-independent truth-table assertions check all 15 profiles
against their expected vectors and verify production helper ordering.
A dedicated exact-merge CI workflow compiles a clean emulator, executes
the new DTLB guests and the inherited sequential UIA, taken branch,
RFI UIA, SS, LP, FP+SS, and IFS/RFI real guests **on the same QEMU
binary**. Logs, independent assembly/ELFs, policy checks and source +
binary hashes are retained. Keep the PR **draft** until all tests pass.

## Boundaries and pending work

- This tranche tests ITC.D, not every DTR P=0 entry, VHPT or TC purge case.
- Real data **unimplemented-address (UAD) faults** in the noncanonical
  gap and key/NaT-page priorities are still unqualified.
- IA-32/debug, nested RSE/firmware, broader architectural completeness
  remain open.
- Issues #21/#45/#27 remain OPEN; `ISA COMPLETE=NO`. Keep the entire
  stacked ancestry unchanged and merge nothing out of order.
