IA-64 ``IPF.c`` migration
==========================

The historical ``IPF.c`` machine monolith remains useful as a complete list of
platform responsibilities.  It is not a behavioural specification and its
implementation choices must not be copied blindly into the modern target.
Architectural manuals, chipset documentation, firmware-visible interfaces, and
focused probes are authoritative.

Inventory
---------

Run the structural inventory from a full clone containing the reachable legacy
history::

  scripts/ia64-ipf-audit.py --root . --output /tmp/ia64-ipf-audit.json

The audit locates the largest historical ``IPF.c`` blob, hashes it, extracts
legacy machine functions, device-creation calls, model identifiers, address
constants, and interrupt constants, then compares them with the modern tree.
It deliberately reports three distinct states:

``machine-wired``
  The current ``hw/ia64`` or ``include/hw/ia64`` tree references the
  obligation.  This is evidence of integration, not proof of correctness.

``model-available``
  A matching model or helper exists elsewhere in QEMU but current IA-64
  machine code does not reference it.  This is an uncompleted port, not a
  completed device.

``unresolved``
  No structural correspondence was found.  Manual review is required; the old
  code may represent a missing device, an obsolete implementation detail, or a
  responsibility now provided under a different name.

Completion rule
---------------

A device is considered ported only after review establishes all of the
following:

* register layout, access widths, reset values, and reserved-bit behaviour;
* interrupt source, polarity, trigger mode, masking, and routing;
* DMA addressability, coherency, and ordering;
* firmware-visible discovery and table contents;
* reset, migration, and multi-CPU behaviour; and
* a focused qtest or guest probe that fails when the contract regresses.

A matching symbol, copied function body, successful firmware boot, or existing
QEMU device type is not sufficient by itself.

Root-cause requirement
----------------------

Do not add firmware-name checks, image hashes, magic program-counter checks,
synthetic successful PAL/SAL results, or arbitrary delays to move a boot trace
forward.  Xen and SDV firmware are independent clients of the same emulated
platform.  A divergence between them is diagnostic evidence to explain, not a
reason to fork architectural behaviour.

The historical file should be retired as a reference only after every extracted
responsibility is either covered by a reviewed modern implementation or marked
obsolete with a documented architectural reason.
