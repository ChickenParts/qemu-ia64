# Xen/IPF PEI HOB relocation correction

Date: 2026-08-22
Integration target: `metachicken`
Source-validation workflow run: 32562803708

## Corrected contract

Xen's out-of-range PEI-memory request is relocated into normal low
physical RAM without retaining the source firmware address's stale
sign-extension or region tag. The redirected PHIT now publishes the
actual HOB-list base in `EfiMemoryBottom`, and the free-bottom
fallback uses that same base.

Legacy IPF PEI computes the permanent-memory HOB copy span from
`EfiMemoryBottom` and `EfiFreeMemoryBottom`. Publishing the
temporary-memory base inflated that span, stranded the real end
marker, and later caused the DXE IPL loader to consume a malformed
list.

## Validation

This workflow applied the retained patch to commit
`d20607e0b957ce6eb7dd482e453568aed70f3e7f`, configured the IA-64 system target, and built
`qemu-system-ia64` successfully on Ubuntu 24.04.

Runtime validation was performed separately with the retained Xen/IPF
`Flash.fd` whose SHA-256 is
`e143e85874ad57bad631853d48f0d47b7e7dbe6c41b4e558bbb4ea5b45775513`.
With the existing bounded system-memory resource-HOB repair enabled,
the corrected relocation clears the former `DxeLoad.c:536` and
`Gcd.c:1736` frontiers and reaches DXE memory-service
initialization. The next observed boundary is the already-instrumented
null DXE indirect-call contract and is intentionally handled in a
separate feature tranche.

The historical Xen firmware downloader is not part of this source
acceptance gate because its upstream Mercurial endpoint is no longer
reliably available. The firmware hash above remains the canonical
runtime-test identity.
