:orphan:

IA-64 PAL reference-image policy
================================

The catalog under ``stuff/Merced_PALs.*`` describes historical binary evidence
for IA-64 PAL research.  The payload is deliberately outside QEMU's build and
runtime paths.  QEMU must not load it automatically or use image hashes,
revision labels, byte patterns, or internal addresses to select guest-visible
behavior.

What the catalog is useful for
------------------------------

The images can support bounded investigations such as:

* comparing adjacent revisions to localize changed code or data;
* checking which PAL procedures real firmware appears to call;
* designing traces that expose an architectural interaction; and
* constructing a specification-backed regression test once that interaction
  is understood.

The catalog does not establish provenance, authenticity, licensing, or the
meaning of filename suffixes.  It also does not turn an observed implementation
choice into an architectural requirement.

Required implementation path
----------------------------

When a reference image exposes missing behavior, use this sequence:

#. Record the observation as a file offset, trace, or call/result tuple.
#. Identify the corresponding PAL, SAL, chipset, or processor contract.
#. Implement that contract in an independently named target or machine helper.
#. Add a focused unit or qtest regression for the contract.
#. Validate against more than one firmware image when practical.

Do not fix firmware progress by matching a blob hash, patching guest bytes,
checking the current instruction pointer against a known image address, or
returning a value solely because one archived revision happened to do so.

Repository material
-------------------

``stuff/Merced_PALs.manifest.json``
  Cryptographic identities, sizes, bundle counts, and same-family comparisons.

``stuff/Merced_PALs.delta.json``
  A byte-level comparison of the closely related PAL-B 7727 and 7728 images.
  All locations are file offsets; no symbol or semantic mapping is asserted.

``scripts/ia64-pal-library.py``
  A fail-closed verifier and deterministic repacker for the supplied ZIP and
  the cataloged tar.xz representation.
