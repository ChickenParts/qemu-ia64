# Rooster IA-64 EFI smoke fixture

This directory contains the architecture-critical subset of the coordinated
`ChickenParts/Rooster:ia64-rooster-bringup` implementation.  It exists here as
a QEMU firmware test payload so the public QEMU repository can build and retain
a known `BOOTIA64.EFI` artifact even while the private Rooster repository has no
hosted Actions runner available.

The fixture proves only the IA-64 EFI application boundary:

- PE32+ machine type `0x0200`;
- entry through an IA-64 PLABEL/function descriptor;
- correct global-pointer setup;
- dynamic relocation, including `R_IA64_FPTR64LSB`;
- a normal C call to EFI `OutputString`.

It must not grow into a second bootloader.  Source fixes made here must be kept
in sync with the corresponding files under `src/arch/ia64/picoefi` and
`src/arch/ia64/efi` in the Rooster branch.  QEMU still boots this payload through
firmware as `EFI/BOOT/BOOTIA64.EFI`; `-kernel` is not part of this test.
