# IA-64 B9: break.b, nop.b, hint.b

Reference: Intel *Itanium Architecture Software Developer Manual*,
Volume 3 revision 2.3 (May 2010), document 323207, section 4.5 (B9).

## Decoder and architectural behavior

| Form | Major opcode | x6[32:27] | Effect |
|---|---:|---:|---|
| `break.b imm21` | 0 | 0 | Break Instruction fault at IVT + 0x2c00 |
| `nop.b imm21` | 2 | 0 | No architecturally visible effect |
| `hint.b imm21` | 2 | 1 | Hint; no architecturally visible effect |

The 21-bit immediate uses bits [25:6] and [36]. Unlike the B-unit
no-op, **`break.b 0` is the all-zero 41-bit B-slot instruction**.
Do not skip it as a bundle filler. Other instruction units have their own
distinct zero-word/no-op behavior.

Qualified break execution raises the existing architectural break
exception rather than firmware callgate handling: CR.IIM receives the
21-bit immediate, CR.ISR.ei and CR.IPSR.ri identify the faulting slot,
CR.IIP identifies the bundle, and the vector handler may resume after
the fault. False qualification suppresses it.

QEMU's existing shared break helper is used; no hypercall fallback,
firmware opcode patch, or global B-slot unsupported-opcode NOP was added.

## GNU IA-64 assembler caution

GNU `as` defaults to **rejecting** `hint.b` on supported toolchains.
This is an assembler policy, not an unrecognized opcode.
The explicit `-mhint.b=ok` flag is required to assemble the positive
hint fixtures. The byte-oracle and IVT scripts enable this flag by
default while still allowing `IA64_AS` overrides.

GNU opcode cross-check: binutils 2.43 `opcodes/ia64-opc-b.c` has
`OpX6 (0, 0x00)` for `break.b`, `OpX6 (2, 0x00)` for `nop.b`,
and `OpX6 (2, 0x01)` for `hint.b`. No binutils source is vendored.

## Validation gates

```sh
python3 scripts/ia64-nonf-fallback-audit.py --check
python3 -m unittest discover -s tests/ia64/nonf -p 'test_*.py'
python3 scripts/ia64-b9-byte-oracle.py
python3 scripts/run-ia64-b9-exception-tests.py --assemble-only
python3 scripts/run-ia64-b9-exception-tests.py --qemu build/qemu-system-ia64
```

The byte-oracle inspects nine independently assembled 41-bit B-slot
words, including all-zero break, high-immediate-bit break, qualified
break, all three bundle slots, ordinary no-ops and hints. The separate
real-IVT guest verifies 5 execution profiles. The branch's read-only
GitHub Actions workflow builds real `qemu-system-ia64` and runs all
these gates plus inherited F12/F15 IVT and F-slot regression tests.

## Outstanding B-unit work

This tranche does **not** implement B6/B7 branch prediction, B8
`clrrrb`, `epc`, or `vmsw`, validate all reserved/ignored B
fields, or deliver general PSR.tb taken-branch traps (#21). The
remaining B-unit burn-down is tracked in #27, and the non-F audit in
#7/#26. The broader IA-64 ISA remains incomplete.
