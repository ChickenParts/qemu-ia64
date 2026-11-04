# Merced pinned ITR.I: real Page Not Present vs instruction TLB miss

Directly stacked on review-ready PR #56. That prior tranche fixed the
**source code** of both pinned ITR and DTR matching, and real-qualified
9 DTR cases after `ptc.e`. This tranche independently tests *instruction
fetch* through a pinned ITR, instead of inferring ITR correctness from
successful DTR/data-side execution.

## Architectural contract

Intel *Itanium Architecture Software Developer's Manual*, rev 2.3,
Vol 2 Table 4-3 / §4.1.8 and Vol 3 `itr.i` / `ptc.e`:

- Hidden translation-register **valid** is distinct from architectural
  PTE.P (page present). A matching valid `P=0` ITR must report **0x5000
  Instruction Page Not Present**, not **0x400 Instruction TLB Miss**.
- For a valid present mapping, a denied execute access must report
  **0x5200 Instruction Access Rights** before A=0; a permitted mapping
  with A=0 reports **0x2400 Instruction Access Bit**.
- A successful executed source instruction with PSR.ss=1 delivers
  post-result **0x6000 Single Step**. A pre-result fetch fault must
  neither commit the source instruction nor produce concurrent SS/TB.

The emulator advertises original Merced (51 within-region virtual
bits plus VRN [63:61]), so the physical fixture uses the final bundle
of a 16KiB page in the **upper sign-extended canonical half** of each
selected VRN. This address is valid in every region; it is not a UIA
(noncanonical address) case.

## Real GNU IA-64 execution

`scripts/run-ia64-itr-not-present-tests.py` generates **12 separate
real-IVT guests** with an independent assembler+linker invocation each.
Each guest sets CR.IVA, initializes an architectural r10 sentinel,
and executes these actions:

1. If a pinned entry is intended, configure CR.IFA to the source VA
   page, CR.ITIR for 16KiB and the PTE to an *independently backed*
   aligned physical `edge_page`, with mode-specific P/AR/A attributes.
2. Clear live PSR.ic, execute **actual `itr.i itr[r22]=r20`** into
   a dedicated pinned instruction translation register, then
   **`ptc.e r23` + `srlz.i`**, purging all ITC copies.
3. **After** that purge, install exactly one 16MiB identity `itc.i`
   entry for the harness and interrupt vector table. This preserves
   executable fault handlers with PSR.it=1 while leaving the
   *high-canonical source* exclusively mapped by the pinned ITR.
4. Use real native `rfi` with restored PSR.it=1, PSR.ic=1, PSR.ss=1,
   optional PSR.tb and RI=2 for an ordinary MII bundle (RI=1 for MLX).
5. For a fault, the IVT handler must verify the **precise vector**,
   ISR execute/read and EI bits, faulting source CR.IFA/IIP/IPSR.ri,
   unchanged r10, then repair IIP and RFI once. For a successful source
   instruction, verify 0x6000, CR.IIPA, next IIP/RI0, ISR.ss=8,
   committed r10=4 and a single repaired RFI.

The independent Python unit tests verify that the pinned `itr.i`
runs before `ptc.e`, that the harness identity `itc.i` runs
**after** the purge, and that no edge-page ITC can inadvertently
satisfy the fetch. Source guards require ITR matching on hidden
valid/RID/tag (not on P), and correct priority through the common
instruction-side fault path.

The tests cover VRNs 0..7: four successful pinned ITR fetches,
four non-present P=0 cases (including regions 6 and 7), two
execute-protection and access-bit cases, one MLX access-bit
case checking fault EI=1, and one deliberately unmapped ITR control.

## Exact-merge qualification

A dedicated GitHub Actions workflow must build a **clean
qemu-system-ia64**, execute the 12 instruction-side guests, then run
24 DTLB (including DTR), 32 sequential/high VA, 20 branch UIA,
15 RFI UIA, 20 SS, 17 LP, 5 FP+SS and 12 IFS/RFI suites
(**157 total real guest executions**) on the *same binary*.
GNU IA64 assembly, all source/host regression oracles,
the unchanged non-F fallback census and retained assembly/ELF,
per-guest QEMU logs, source SHA and binary SHA are required
before the new PR may leave draft.

## Outstanding

This tranche does **not** qualify the alternate unimplemented fetch
FAULT policy, noncanonical data address UAD, protection-key/NaTPage,
ITR replacement/overlap/TC arbitration, IA-32, nested RSE or firmware.
Keep issues #21/#45/#27 open and `ISA COMPLETE=NO`.
Do not merge out of stacked order.
