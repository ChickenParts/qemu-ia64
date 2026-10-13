# IA-64 CR.IFS, cover and rfi — architectural-state repair

Normative references:
- Intel Itanium SDM **Volume 2 rev. 2.3 §3.3.5.7**, Figure 3-14,
  Table 3-9 (p. 2:41): IFS.v=bit63; IFS.ifm=[37:0];
  reserved=[62:38], **no IFS.EC field**.
- Intel Itanium SDM **Volume 3 rev. 2.3**, `cover` p. 3:48:
  when PSR.ic=0, copy CFM to IFS.ifm and set v=1;
  when PSR.ic=1, leave CR.IFS unchanged.
- Intel Itanium SDM **Volume 3 rev. 2.3**, `rfi` p. 3:238:
  if IFS.v=1, restore CFM from IFS.ifm; otherwise CFM unchanged.
  AR.EC is independent application register state across this transfer.

## Fixed emulator model

- Interruption entry: when **interrupted** PSR.ic=1, clear IFS.v, without
  synthesizing an IFS snapshot from CFM or AR.EC. Collection-disabled
  interruption leaves IFS alone for software `cover`.
- `cover`: save only CFM[37:0] and set IFS.v when PSR.ic=0, retaining the
  previous IFS without modification when PSR.ic=1.
- `rfi`: restore **only** CFM[37:0] if V is set, never copy IFS reserved
  bits into CFM and never synthesize AR.EC from imaginary IFS.EC bits.
- Apply the same IFS invalidation to external interrupts in cpu.c.
- Keep existing RSE snapshot, bank-switch and early-firmware paths untouched.

These semantics are isolated in `target/ia64/ifs.h` for production and
an independent pure-host state matrix. The real guest suite exercises valid
and invalid IFS, four AR.EC values, both PSR.ic cover cases, and
real interruption/vector/RFI recovery. Checks distinguish architectural
state preservation from an unrelated AR.EC representation cleanup.

This issue does not claim complete RSE, nested interrupt recovery, ISA
conformance, or full firmware boot. See #40 and related B1/B2 #39.
