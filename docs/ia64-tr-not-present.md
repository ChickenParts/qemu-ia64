# Merced pinned translation-register Page Not Present qualification

Directly stacked on PR #55, which qualified **15** real ITC.D data-translation
cases and corrected the TLB-cache present/access-rights/access-bit/dirty-bit
priority. This tranche extends that same real-IVT harness to *pinned* DTR
translations while preserving all inherited tests and stack ancestry.

## Architectural distinction

Intel *Itanium Architecture Software Developer's Manual*, rev. 2.3
Volume 2, Table 4-3 (p. 2:55): the translation's **P** (page present)
bit is distinct from a hidden **valid** bit. An enabled matching entry
with `P=0` produces **Data/Instruction Page Not Present**, not a TLB
miss. Volume 3 `itr`, pp. 3:148–150, defines explicit pinned DTR/ITR
slots selected by the register operand and the IFA/ITIR/RR translation
inputs. A `ptc.e` instruction purges the translation *cache*, not
pinned Translation Registers.

## Production gap and bounded fix

In `ia64_cpu_tlb_fill()`, the DTR and ITR matching loops checked each
entry's hidden `valid` flag **then discarded any translation whose
PTE.P bit was zero**. The loops also incorrectly manufactured
`tr_match.p=1` for every matching TR. Both paths now match based on
hidden validity, RID and page-aligned VA (regardless of PTE.P), and
copy the actual `PTE_P` bit to the entry presented to the common
post-lookup fault priority. That existing common path selects
`0x5000` when P is zero, before rights/A/D. A nonmatching TR still
falls through to TC lookup or normal TLB-miss handling.

This does **not** claim complete TR/TC overlap arbitration when software
deliberately installs incompatible translations: software must avoid
ambiguous overlapping translations. All real DTR tests explicitly
purge the opportunistic TC copy first.

## Real-IVT regression matrix

Nine new profiles extend the existing `scripts/run-ia64-dtlb-fault-priority-tests.py`
from **15 to 24**. Each new profile does the following in actual GNU
IA-64-assembled guest code:

1. Install `cr.ifa` and `cr.itir` for a 16 KiB virtual page backed by
   a *different* 16 KiB physical `data_page`.
2. Clear live `PSR.ic` for the privileged `itr.d dtr[r22]=r20`
   instruction with a dedicated pinned DTR slot.
3. Execute `ptc.e r23` followed by `srlz.d` so QEMU's opportunistic
   **DTC copy cannot satisfy the lookup**, leaving only the pinned DTR.
4. Enter a load/store M-slot using genuine `cr.ipsr` + `rfi` with
   `PSR.dt=1`. Check actual fault vector, `CR.IFA/IIP/IPSR/ISR`,
   uncommitted register/memory on fault, and one repaired RFI; positive
   DTR mappings must commit the expected physical data or store value.

Cases include valid reads/writes in regions 0/6/7, AR-only and
AR+A/D denial, A-bit and D-bit faults, and **three P=0** DTR entries
in VRNs 5, 6 and 7. Each of those three must raise `0x5000` and
**not** `0x800` (DTLB miss); the region 6/7 cases additionally
prevent the Linux-compatible direct-map fallback from bypassing the
negative pinned translation.

Fail-closed Python guards assert the real DTR instruction followed by
the TC purge (never just an ITC.D insert), `P=0` classification
independently of the implementation, the source lookup's hidden
validity vs P logic, and that the purge helper leaves pinned TR state
untouched. The inherited CI workflow must perform a *fresh exact PR
merge-ref* QEMU build and run all **24 new/inherited DTLB cases plus
121 other IVT guests on the same binary**. It retains assembly, ELF,
per-guest QEMU logs/result files and emulator/source hashes.

## Explicit limits

- The code corrects ITR and DTR P=0 paths, but the **new real guest
  matrix exercises DTR**; a distinct instruction-side pinned ITR
  P=0 guest is still required to close that coverage.
- `ptc.e` TC-purge behavior is used as provided by the existing
  implementation; a full purge and replacement matrix is separate.
- TLB permission keys, NaTPage, noncanonical data-address UAD,
  nested RSE, IA-32, SAL/PAL and firmware boot are not qualified.
- Keep the existing ISA audit/issues #21/#45/#27 open,
  `ISA COMPLETE=NO`, and preserve the entire PR stack in order.
