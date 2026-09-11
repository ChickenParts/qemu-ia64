# IA-64 PEI HOB migration frontier

## Reproduced failure

With the canonical Xen/IPF `Flash.fd` image identified by SHA-256

```text
e143e85874ad57bad631853d48f0d47b7e7dbe6c41b4e558bbb4ea5b45775513
```

the firmware reaches the DXE IPL.  Its active permanent HOB list is
structurally valid but contains no `EFI_HOB_TYPE_FV` (`0x0005`) records.  A
valid temporary list remains in the firmware work area and does contain
firmware-volume HOBs.

The resulting failure is not an arbitrary indirect-call failure.  The DXE IPL
searches the HOB list for firmware volumes, fails to locate/load the DXE core,
and later reaches the standard IA-64 stack-transfer path without a valid DXE
core procedure label.

## Architectural interpretation

On IA-64, the first `SwitchStacks()` parameter is a pointer to a procedure
label.  The procedure label contains both the continuation instruction pointer
and its global pointer.  The second parameter is the HOB-list pointer.  Treating
the observed HOB-list address as executable code, or manufacturing an IP for a
zero indirect-call target, would conceal the earlier failure rather than fix
it.

The Rooster boot contract remains:

```text
QEMU IPF machine
  -> IA-64 firmware
  -> EFI removable-media discovery
  -> EFI/BOOT/BOOTIA64.EFI
  -> Rooster EFI entry
```

QEMU's ELF `-kernel` loader is not part of this path.

## Diagnostic tools

`scripts/analyze-ia64-hob-migration.py` compares physical-memory snapshots of
temporary and permanent HOB lists.  It can also emit an offline target-memory
copy with missing FV HOBs inserted before the end marker.  It never modifies a
running guest.

`hw/ia64/hob-migration-probe.c` is an opt-in causality probe controlled by
`QEMU_IA64_PEI_FV_HOB_RESTORE`.  It is deliberately not included in the normal
build.  The dedicated causality workflow includes it transiently, compares an
otherwise identical baseline and probe run, and rejects the result unless:

1. both source and target are structurally valid HOB lists;
2. FV records are copied without duplication;
3. the target heap has sufficient room;
4. the null-call workaround remains disabled; and
5. firmware advances to a later independently recognised stage.

Even a successful result does not make the probe a production fix.  It proves
that the missing FV records are causal and focuses the next investigation on
the guest's temporary-to-permanent PEI migration, including memory-copy and
IA-64 RSE/stack-state transfer.

## Acceptance gates for the root fix

A root fix must satisfy all of the following with the semantic probe and all
PC-specific status/target rewrites disabled:

1. the permanent HOB list naturally contains the firmware-volume records;
2. `PeiFindFile(EFI_FV_FILETYPE_DXE_CORE)` succeeds;
3. the DXE entry procedure label has nonzero, mapped IP and GP words;
4. DXE Core starts without a synthesized call target;
5. firmware enumerates the FAT-backed IDE media;
6. serial output contains `ROOSTER-IA64-EFI-ENTRY: PASS`;
7. Rooster opens its boot volume and emits `ROOSTER-IA64-EFI-FS: PASS`.

The uploaded SDV and rx4610 firmware family is tracked separately because it
currently stops in an earlier FIT/PEI loop and does not exercise the same DXE
IPL migration frontier.
