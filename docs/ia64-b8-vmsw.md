# IA-64 B8 `vmsw.0` / `vmsw.1`

The B8 encodings use major 0 and x6 values `0x18` and `0x19`.
They are unpredicated: every bit outside x6, including `qp`, is fixed zero.

## Current CPU-model contract

The repository currently exposes one `itanium` CPU model.  It does not model
the IA-64 virtual-machine environment or `PSR.vm`.  This tranche therefore
implements the architecturally honest subset:

1. Decode both legal words before ordinary predicate qualification.
2. Treat nonzero `qp` and all other fixed-field violations as Illegal
   Operation.
3. Deliver Illegal Operation for a legal `vmsw.0` or `vmsw.1`, at every CPL,
   because the processor model does not implement the VM feature.  Feature
   absence precedes the CPL and virtualization-environment checks.
4. Do not modify PSR, CPL, translations, or any virtual-processor state.

This is intentionally not a no-op and intentionally does not add a CPU model
that can set `PSR.vm` without the rest of the virtualization contract.

## PAL feature reporting

`PAL_PROC_GET_FEATURES(feature_set=0)` now succeeds and reports:

- implemented: `NoVM` (bit 40)
- current setting: `NoVM` (bit 40)
- controllable: zero
- `EnableVmsw` (bit 54): clear in all three vectors

Other feature-set numbers preserve the existing invalid-argument response.
The PAL result and instruction behavior therefore describe the same CPU.

## Deferred virtualizing model

A model that recognizes and executes virtualization extensions must land as a
separate tranche.  At minimum it must add and validate:

- model-specific VM capability and enable state
- Privileged Operation when the feature exists and CPL is nonzero
- Virtualization fault at vector `0x6100` when instruction translation,
  execute-only fetch rights, VM-environment enablement, or `EnableVmsw`
  rejects the transition
- successful `PSR.vm` clear/set semantics
- virtualization interception for the wider instruction and interruption set
- interruption/RFI state handling and address-width rules while `PSR.vm=1`
- truthful PAL virtual-processor procedures and processor feature controls

Until those pieces are present together, setting `PSR.vm` would create a mode
whose architectural consequences are not implemented.

## Validation

The dedicated workflow performs:

- direct production-header encoding tests over all 64 x6 values and all 64
  `qp` values, plus every remaining fixed bit for both legal x6 values
- PAL feature-set tests for sets 0 through 63
- source-order assertions proving decode occurs before `gen_qp_skip`
- a full `qemu-system-ia64` build
- four real-IVT guest cases covering both instructions, CPL0/CPL3 and all
  three B slots; CPL3 must still receive Illegal rather than Privileged
- inherited EPC, `clrrrb`, B6/B7, B9, F12/F15 and F-slot regressions

Host-only reproduction:

```sh
cc -std=gnu2x -O2 -Wall -Wextra -Werror \
  -o /tmp/ia64-vmsw-policy tests/ia64/nonf/vmsw-policy-test.c
/tmp/ia64-vmsw-policy
python3 -m unittest tests.ia64.nonf.test_vmsw_source -v
```

Full runtime reproduction additionally requires the IA-64 GNU assembler and
linker, then:

```sh
python3 scripts/run-ia64-b8-vmsw-tests.py --assemble-only
python3 scripts/run-ia64-b8-vmsw-tests.py --qemu build/qemu-system-ia64
```
