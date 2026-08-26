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

The audit deliberately selects the historical ``hw/ipf.c`` path, rather than
the current and usually larger ``hw/ia64/ipf.c`` implementation.  Use
``--legacy-path`` only when auditing a repository whose historical ledger lived
at another path::

  scripts/ia64-ipf-audit.py --root . --legacy-path hw/ipf.c \
    --output /tmp/ia64-ipf-audit.json

For the selected path, the largest reachable revision is hashed and recorded
with the selection strategy and candidate count.  If ``hw/ipf.c`` is absent,
the tool may fall back to another non-current path ending in ``ipf.c``; it will
never silently use ``hw/ia64/ipf.c`` as the historical ledger.  The audit then
extracts legacy machine functions, device-creation calls, model identifiers,
address constants, and interrupt constants and compares them with the modern
tree.

It deliberately reports three distinct states:

``machine-wired``
  The current IA-64 machine tree or IA-64 device configuration references the
  obligation.  This is evidence of integration, not proof of correctness.

``model-available``
  A matching model or helper exists elsewhere in QEMU but current IA-64
  machine code does not reference it.  This is an uncompleted port, not a
  completed device.

``unresolved``
  No structural correspondence was found.  Manual review is required; the old
  code may represent a missing device, an obsolete implementation detail, or a
  responsibility now provided under a different name.

Generic creation-call names are retained as navigation evidence, but they are
weak signals because nearly every machine uses helpers such as ``qdev_new`` or
``pci_create_simple``.  The summary therefore also reports identity-only
coverage derived from model and device literals.  Neither percentage is a
completion score.

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
