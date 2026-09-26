# IA-64 Rooster EFI bring-up

This branch carries the QEMU side of the coordinated IA-64 Rooster bring-up.
The architectural boundary is deliberately EFI, not QEMU's direct `-kernel`
ELF loader.

## Contract

QEMU/IPF is responsible for providing a platform on which IA-64 firmware can
reach the EFI removable-media boot path and execute `EFI/BOOT/BOOTIA64.EFI`.
Rooster is responsible for being a valid IA-64 EFI application and for taking
over the machine after `ExitBootServices()`.

The direct-kernel path remains useful as an emulator diagnostic because it can
exercise the IA-64 CPU, SAL/PAL, ACPI, interrupts, and memory model without
firmware. It is not part of the Rooster boot contract.

## Reproducible application test

Prepare a FAT directory:

```
scratch/ia64-esp/EFI/BOOT/BOOTIA64.EFI
```

Then run:

```
IA64_BIOS=/path/to/Flash.fd \
IA64_ESP=scratch/ia64-esp \
  scripts/run-ia64-efi-app.sh
```

The wrapper intentionally rejects `-kernel` so that a passing test proves the
firmware/EFI path rather than QEMU loading the application itself.

## Display-capable replay

The firmware matrix keeps two distinct lanes.  Its default `--vga none` lane is
the serial-only CPU/firmware control.  The `--vga std --display none` lane still
instantiates the VGA device and loads its option ROM while remaining suitable
for unattended testing.  Relocated emulator artifacts carry their matching
`vgabios*.bin` files in a sibling `pc-bios` directory, which the runner supplies
to QEMU with `-L`.

The display lane is not a substitute for the serial control and is not merely
packaging insurance: it preserves the path toward firmware framebuffer/GOP
bring-up.  A later acceptance gate must confirm visible framebuffer output and
mode-setting behavior rather than stopping at successful option-ROM loading.

## QEMU readiness gates

For Rooster EFI bring-up, success is staged:

1. Firmware reaches DXE/boot-manager code without synthesized call targets.
2. The FAT-backed IDE disk is enumerated.
3. `BOOTIA64.EFI` is loaded as an IA-64 PE/COFF EFI application.
4. Firmware enters the application's real EFI entry point with a valid image
   handle and system table.
5. Boot Services used by Rooster work: console, loaded-image/device-path,
   filesystem, allocation, memory-map retrieval, and `ExitBootServices()`.
6. After `ExitBootServices()`, architectural services Rooster may retain or
   discover independently are coherent: PAL/SAL, ACPI, IOSAPIC/interrupts,
   timers, PCI and serial.

## Non-goals

Do not add a QEMU-specific Rooster handoff block, magic register convention, or
loader-side ELF interpretation to make Rooster boot. If an EFI application
cannot boot, fix the EFI/platform/emulation contract at the layer that owns the
bug.

Similarly, the diagnostic firmware repair switches in the current bring-up
runner are not part of the target contract. They should remain diagnostics or
be removed as underlying defects are fixed.
