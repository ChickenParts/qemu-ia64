# Merced PAL reference catalog

This directory catalogs seven historical IA-64 Processor Abstraction Layer
images supplied to the QEMU IA-64 reconstruction on 2026-08-26. The images
are **reference evidence only**. They are not built, installed, selected by
the `ipf` machine, or executed by QEMU.

The supplied container was `Merced_PALs.zip` (1,085,582 bytes, SHA-256
`2428a7d409e4fb0b1d1673fccc289357945249f32bdb57c925c808f352d5b3cb`).
`Merced_PALs.manifest.json` records the archive and every member's identity.
The binary payload is retained separately from the QEMU source history; the
catalog and tooling are sufficient to reject an altered or incomplete copy.

No source, license notice, authenticity statement, hardware provenance, or
explanation of the decimal filename suffixes accompanied the archive.
`PAL_A`, `PAL_B`, and identifiers such as `7727` are consequently treated as
filename labels, not independently verified metadata.

## Verification and deterministic repack

Run from the repository root:

```sh
scripts/ia64-pal-library.py \
  --manifest stuff/Merced_PALs.manifest.json \
  /path/to/Merced_PALs.zip
```

To create the compact, deterministic repository-form archive:

```sh
scripts/ia64-pal-library.py \
  --manifest stuff/Merced_PALs.manifest.json \
  --repack-output /tmp/Merced_PALs.tar.xz \
  /path/to/Merced_PALs.zip
```

The expected repack is 266,932 bytes with SHA-256
`9145e5ac8b1bdbc7c4dfe40b254eabcc20dd30aa6f1ddd194bb5a5656234642b`.
The seven filenames and every member byte are unchanged. The verifier fails
closed on archive, member, size, checksum, hash, or bundle-alignment changes.

## Reproducible PAL-B lineage

Generate the exact same-offset, 16-byte bundle comparison with:

```sh
scripts/ia64-pal-library.py \
  --manifest stuff/Merced_PALs.manifest.json \
  --lineage-output /tmp/Merced_PALs.lineage.json \
  /path/to/Merced_PALs.zip
```

`Merced_PALs.lineage.json` covers the filename series `2216`, `6625`, `7727`,
`7728`, and `8830`. The focused 7727-to-7728 transition changes 397 of 32,860
bundles. Of those changes, 139 remain byte-identical in 8830, five revert to
the 7727 bytes, and 253 change again.

The durable changes form the first five clusters, from offset `0x238d0`
through `0x25bc0`. The later region is substantially more volatile: the
248-bundle cluster at `0x44c40` through `0x45bc0` contains all five exact
reversions and 243 bundles that change again in 8830. These are file-offset
relationships only, not code/data, symbol, entry-point, or semantic claims.

`Merced_PALs.delta.json` retains the finer byte-level comparison of
`PAL_B_7727.bin` and `PAL_B_7728.bin`. Its offsets are forensic leads, not
permission to add image hashes, magic addresses, or revision-specific
execution paths to QEMU.

## Architectural use policy

A behavior observed in one image may motivate a trace, a focused test, or an
architectural question. Production code must model the PAL or platform
contract itself. Firmware-image detection, guest-byte patching, and
address-gated compatibility behavior are expressly out of scope.
