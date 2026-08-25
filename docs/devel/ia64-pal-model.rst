IA-64 PAL model
===============

Design rule
-----------

The IA-64 target models a synthetic QEMU processor and platform.  PAL answers
must be derived from that model and from the architected PAL ABI; another
emulator is useful only as a negative comparison or test oracle.  In
particular, firmware-specific progress is not evidence that a borrowed cache,
frequency, VM, or relocation personality is correct.

The firmware matrix names each input explicitly:

``xen``
  Xen/KVM ``efi-vfirmware``.  The canonical image is fetched from its source
  repository and verified by SHA-256.  It currently exercises the EDK-derived
  PEI and DXE path.

``sdv-debug-0.99``
  Intel Software Development Vehicle debug EFI image already retained under
  ``stuff/``.  It exercises an earlier and substantially different firmware
  path.  It must never silently inherit Xen-specific success criteria.

Relocatable PAL
---------------

The old synthetic PAL entry is intercepted at a fixed address and is not a
copyable PAL image.  ``PAL_COPY_INFO`` and ``PAL_COPY_PAL`` therefore use a
small QEMU-owned IA-64 gateway instead of copying bytes from another emulator:

* the gateway executes ``break 0x1000`` to enter QEMU's PAL service dispatcher;
* it then executes an ordinary ``br.ret`` through ``b0``;
* its reported size is ``sizeof`` the assembled gateway, not a compatibility
  constant;
* its alignment is the architectural 16-byte bundle alignment;
* ``PAL_COPY_PAL`` validates CPU number, allocation size, address form,
  alignment, overflow, and guest-memory write completion;
* copied bytes live in guest RAM, so migration and snapshots preserve them
  without extra hidden host state.

The assembly source is retained and the generated C initializer is checked in.
CI builds an IA-64 assembler, regenerates the initializer, and rejects drift.

Profile consistency work
------------------------

The remaining PAL procedures predate this model and include values introduced
for individual Linux or firmware milestones.  They must be moved behind one
profile structure so these invariants can be tested mechanically:

* ``PAL_CACHE_SUMMARY`` agrees with every successful ``PAL_CACHE_INFO`` query;
* cache sharing agrees with logical/physical topology;
* frequency base and ratios describe one coherent ITC, bus, and core clock;
* page-size and VM-summary masks match the MMU actually implemented by TCG;
* debug/performance register counts do not exceed implemented architectural
  state;
* unsupported behavior returns an architected error rather than a convenient
  value copied from another implementation.

Firmware tests are integration tests, not the definition of PAL semantics.
