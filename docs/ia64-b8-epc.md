# IA-64 B8 `epc`: Enter Privileged Code

Reference: Intel *Itanium Architecture Software Developer's Manual*,
Volume 3 rev 2.3, **page 3:53**, and §4.5.3.1 B8 on page 3:355.
Access-rights semantics: Volume 2 rev 2.3, §4.1.1.6 Table 4-4.

## Architectural rules

The B8 instruction is major=0, x6[32:27]=0x10, with the remaining
bits fixed zero. **It cannot be predicated**: a nonzero qp/ignored bit
is a fixed-field violation, not a conditional instruction.

1. If `AR.PFS.ppl` (bits 63:62) is **numerically below** `PSR.cpl`,
   raise Illegal Operation fault at the instruction before committing any
   CPL change. The PFS check happens even with PSR.it=0.
2. If `PSR.it=0`, successful `epc` sets CPL=0.
3. If `PSR.it=1`, only the **fetched instruction's** translation with
   `AR=7` (execute-only promotion page) and `PL < CPL` may promote
   `PSR.cpl` to PL. If AR is not 7 or PL is >= CPL, CPL stays unchanged.
   A missing translation is *not* a license to promote.
4. `epc` is not a branch or interruption on success. Instructions later
   in its own instruction group may see old or new privilege; later
   groups must see the new CPL. The translator ends the TB after
   `epc` to ensure correct MMU index/privilege for subsequent groups.

This target models an instruction fetch through pinned ITR entries and
ordinary ITLB entries; it also has early-boot special direct mappings.
`ia64_epc_fetch_rights` inspects only the instruction-side metadata
matching the *same PC*, not a DTLB entry or arbitrary target page.
The current softmmu allows an ITLB match to supersede an ITR match;
the helper intentionally mirrors that selection. A future unified
fetch/permission contract should preserve the chosen AR/PL at fetch
rather than searching again at execution time.

## Validation

The tracked permanent read-only workflow `ia64-b8-epc-validation.yml`
runs the following in order:

- Direct production evaluator's **2048-combination** host truth-table
  test: all 4 CPL, all 4 PPL, both PSR.it modes, both fetch-valid states,
  all 8 TLB.ar, all 4 PL. Production source/ordering mutation checks
  verify fixed-field faults cannot be skipped by predicate qualification.
- Independent GNU IA-64 assembler: the exact B8 encoding from three
  legal B-slot positions; predicated `epc` must be rejected by the
  assembler. The positive corpus never hand-synthesizes valid bytes.
- **Ten real `qemu-system-ia64` profiles**. RFI installs CPL3
  and optionally PSR.it=1. For translated cases an actual `itc.i`
  inserts a 16-MiB identity-mapped translation covering the source and
  IVT, with AR7/PL0,PL2,PL3 or AR1/PL3. The guest checks successful
  CPL promotion, no-promotion cases, and fault-before-commit for
  PFS.ppl mismatches. The real General Exception vector observes
  CR.ISR.ei, CR.IPSR.ri/CPL/IT, CR.IIP, CR.IIM and PFS, then resumes
  after the fault using RFI.
- Inherited B8 `clrrrb` (including NaTs), B6/B7, B9, F12/F15 IVT
  and F-slot guest regressions. Broader F-unit workflows run separately
  on the PR's exact merge ref.

Reproduce:

```sh
cc -std=gnu2x -O2 -Wall -Wextra -Werror \
    -o /tmp/ia64-epc-host tests/ia64/nonf/epc-logic-test.c
/tmp/ia64-epc-host
python3 scripts/ia64-nonf-fallback-audit.py --check
python3 -m unittest discover -s tests/ia64/nonf -p 'test_*.py' -v
python3 scripts/ia64-b8-epc-byte-oracle.py
python3 scripts/run-ia64-b8-epc-tests.py --assemble-only
python3 scripts/run-ia64-b8-epc-tests.py --qemu build/qemu-system-ia64
```

## Limitations

Do not claim full B-unit or all-instruction conformance based on
`epc`. Reference-backed reserved and conditionally reserved forms,
processor-model variance, `vmsw.0/1` virtualization semantics (#34),
general PSR.tb taken-branch traps (#21), and firmware/RSE integration
remain tracked separately. This implementation does not add a
virtualization feature, rewrite a PAL hypercall or pretend that
an unsupported VM switch is a no-op.
