# B1/B2 loop branches: slot restriction and while-branch qp semantics

Normative: Intel *Itanium Architecture Software Developer's Manual*, Volume 3
rev 2.3 (323207), **br**, pages 3:23–27, especially Figure 2-4 and
the branch operation at p. 3:27.

1. Every B1 `br.wtop/br.wexit` and B2
   `br.cloop/br.ctop/br.cexit` must execute in **bundle slot 2**.
   A loop in slot 0/1 raises **Illegal Operation**, whether it
   would transfer control or not; this validation precedes normal
   predicate qualification and preserves source-slot metadata.
2. `br.wtop/br.wexit` use **PR[qp] as an operand** to their loop
   decision/EC decrement/PR63 write/rotation. The predicate does **not**
   suppress execution. For false PR[qp], EC>1 still causes wtop to
   branch, EC decrements when nonzero, PR63 clears, and rotation occurs
   even when the branch is not taken.
3. B1 `br.cond` and B3/B4/B5 calls retain ordinary predication.
   B2 counted branches keep fixed-zero encoded qp and existing loop
   execution. The source only changes qualification of B1 while forms,
   not their pre-existing decision/rotation code.

## Implementation

The B12 policy is shared by real translator helper and independent
24,576-case host test. Production helper obtains the **actual source
slot from the translated context**, captures translated GETPC directly,
and faults before any predicate/loop state. The translator uses PR0
for *execution qualification* of B1 while, but keeps the original
encoded qp for `gen_pr_read_bit(qp)` and branch state updates.

## Validation / negative controls

- Independently GNU-assemble all five loop forms in slot 2. Move the
  complete original 41-bit loop instruction into slot 0/1 of a **BBB
  template**, exchanging it with an independently GNU-assembled nop.b.
  Every mutation proves the 41-bit opcode and the bundle template
  remain unchanged; GNU assembler's deliberate source restriction
  is not used as a substitute for real Illegal Operation execution.
- 10 real illegal-slot guest fixtures (five forms × slots0/1),
  checking actual IVT0x5400, RI/EI, non-access fault, zero IIM,
  unchanged AR.LC and AR.EC, then RFI recovery.
- 10 legal B1 while guest profiles: p6 true/false, EC0/1/2/63,
  wtop/wexit taken and untaken paths. Read AR.EC, AR.LC, PR63
  and CFM rotating-GR base via a real br.call/ar.pfs snapshot.
- Build full QEMU and rerun all inherited IFS, B1/B2, B4/B5,
  NoVM/EPC, clear-RRB, B6/B7, B9, F-slot/F-unit regressions.

This does **not** claim target-wide PSR.tb traps (#21), complete
nested RSE/migration, or firmware boot; the broad ISA status remains
**ISA COMPLETE=NO**.
