.. SPDX-License-Identifier: GPL-2.0-or-later

IA-64 IPF platform port status
===============================

Purpose
-------

The ``ipf`` machine is a current-QEMU reconstruction of the historical IA-64
platform machine.  The historical ``hw/ipf.c`` is useful as a device inventory
and as evidence about the old virtual platform, but it is not a specification.
The architecture manuals, chipset documentation, UEFI/SAL/PAL specifications,
and observable hardware contracts take priority.

The machine must boot Xen-oriented firmware, Intel SDV firmware, and direct
kernels as independent validation lanes.  Code that makes one image advance by
matching its hash, strings, instruction address, register values, or private
memory layout is not an implementation of the platform.

Non-negotiable implementation rules
------------------------------------

* CPU and device behavior must not depend on a firmware filename, hash, string,
  program counter, or image-specific address.
* The MMU and instruction helpers must not rewrite guest firmware data
  structures.  Diagnostic instrumentation may observe guest state but must not
  alter it.
* Machine topology must not change according to whether the payload is firmware
  or a direct kernel.  Boot paths may provide different boot data, not different
  hardware.
* Interrupts cross a device/CPU contract: devices assert routed sources, the
  I/O SAPIC selects a vector, and the local SAPIC performs IRR/IVR/TPR/EOI
  arbitration.  Devices must not inject exception indexes directly.
* Scratch patches, firmware call gates, and compatibility probes are not
  acceptable substitutes for production code or tests.
* A behavior change needs an architectural or device-level test whenever the
  contract can be isolated from a complete firmware boot.

Historical device inventory and current state
----------------------------------------------

The inherited historical machine configured a conventional PC-derived device
set around the IA-64 firmware interface.  The table distinguishes current-QEMU
device paths from remaining partial platform behavior.

.. list-table::
   :header-rows: 1
   :widths: 20 25 55

   * - Historical component
     - Current state
     - Required work
   * - RAM and firmware windows
     - Present
     - Direct-kernel port I/O comes from the EFI memory map without editing
       guest kernel symbols.  Firmware loading no longer rewrites FIT metadata,
       GP-relative globals, or fixed status-code pointers; move the remaining
       variable-store and scratchpad provisioning into explicit platform state.
   * - PCI host/root
     - Custom current-QOM model
     - Validate 460GX address windows, DMA reachability, reset, and migration.
   * - 460GX host functions
     - Resettable, migratable QOM device
     - Complete chipset behavior beyond the firmware-visible sparse
       configuration and CB0/CC0 windows.
   * - I/O SAPIC
     - QOM device with isolated core tests
     - Complete destination modes, polarity semantics, special delivery modes,
       and multi-CPU routing.
   * - Local SAPIC
     - Architectural CPU model
     - IRR/IVR/TPR/EOI arbitration is isolated and unit tested; add
       NMI/ExtINT/IPI and complete migration state.
   * - PIIX southbridge
     - Coherent PIIX4 multifunction device
     - Validate remaining reset and migration interactions across all child
       functions.
   * - ISA bus
     - Owned by PIIX4
     - IRQ0..15 route directly to I/O SAPIC inputs; retain this single-bus
       topology as compatibility devices are added.
   * - 8259 PIC
     - Deliberately disabled
     - Decide from platform documentation whether exposing a legacy PIC is
       required; do not fake one in CPU code.
   * - RTC/CMOS
     - PIIX4 RTC on the ISA bus
     - Memory, equipment, and floppy fields are populated; extend only from
       documented firmware requirements.
   * - Serial
     - Routed serial-mm UART with COM1 alias and auxiliary ISA ports
     - MMIO and COM1 share one UART state; IRQ4 and configured COM2--COM4 IRQs
       traverse the PIIX ISA lines into the I/O SAPIC.  Validate console
       discovery across every firmware lane.
   * - Parallel
     - Current ISA parallel ports on request
     - LPT1 register reset and IRQ7 delivery through the I/O SAPIC are tested;
       validate firmware demand and additional configured ports.
   * - VGA
     - Current PCI VGA
     - Validate firmware discovery, option-ROM execution, and INTx behavior.
   * - IDE
     - PIIX IDE child with current drive creation
     - Add boot and data-path coverage with IA-64 firmware and direct kernels.
   * - i8042 keyboard/mouse
     - Current ISA i8042
     - IRQ1 and IRQ12 are tested through the I/O SAPIC; leave the x86-only A20
       output unused.
   * - ISA DMA
     - PIIX-owned dual i8257 controllers
     - Standard channel registers, masking, and reset are machine-tested; add
       end-to-end DMA coverage for each attached legacy device.
   * - Floppy
     - Current ISA FDC at I/O 0x3f0, IRQ6, DMA2
     - Controller reset, IRQ routing, DMA attachment, and CMOS identity are
       tested; add sector-transfer and firmware boot-media coverage.
   * - UHCI USB
     - PIIX UHCI child
     - Validate firmware discovery and interrupt routing with real USB devices.
   * - ACPI PM/SMBus
     - Authoritative PIIX4 PM/SMBus device
     - PM1, timer, reset defaults, SCI, and EEPROM topology are tested; expand
       ACPI tables without adding a second register implementation.
   * - Network
     - Current generic PCI NIC creation
     - INTx reaches the I/O SAPIC; validate firmware defaults and additional
       supported NIC models.
   * - SCSI
     - Current LSI53C895A controllers for populated legacy buses
     - Sparse bus numbering and PCI identity are tested; add command, data-path,
       INTx, and firmware boot coverage.
   * - Virtio block
     - Standard virtio-blk-pci legacy alias
     - ``if=virtio`` enumeration is tested without IPF-specific creation code;
       add data-path, INTx, and firmware support coverage.
   * - Audio
     - Generic PCI devices available on request
     - Optional after the boot-critical platform contract is complete.
   * - SMP/IPI
     - Incomplete
     - Implement LID targeting, IPI space, rendezvous/reset, per-CPU platform
       state, and SMP firmware tables.
   * - Migration
     - Partial
     - Custom 460GX and I/O SAPIC devices have VMState; complete CPU, firmware,
       and remaining machine-owned state and add migration tests.

Current assessment
------------------

The boot-critical PC-derived topology is now represented by current QEMU device
models: PCI host/root, PIIX4, ISA, IDE, UHCI, PM/SMBus, RTC, i8257 DMA, i8042,
floppy, serial, parallel, VGA, generic PCI NIC attachment, legacy LSI SCSI
buses, and the standard virtio PCI storage transport.  The I/O SAPIC and 460GX
facade are separate QOM devices rather than in-file register shims.

Direct-kernel boot now advertises its legacy port-I/O window through an
``EFI_MEMORY_MAPPED_IO_PORT_SPACE`` descriptor, leaves loaded ELF data
intact, and verifies both properties in qtest.
The firmware loader also leaves FIT entries, GP-relative globals, and fixed
status-code pointers untouched.  A synthetic image satisfying all three old
mutation predicates is verified unchanged; the bundled-firmware audit found
no image that depended on those paths.

That does not make the platform complete.  The largest remaining risks are CPU
and chipset architectural coverage, SMP/IPI support, migration completeness,
and firmware compatibility code that still observes or changes image-private
state.  Optional historical devices matter less than removing those shortcuts
and validating one payload-independent machine against all three boot lanes.

Priority order
--------------

#. Remove guest-firmware mutation and image-private behavior from execution and
   machine code, replacing each case with the underlying architectural fix.
#. Validate Xen firmware, SDV firmware, and direct kernels against one topology
   and record reproducible frontier evidence for each lane.
#. Complete 460GX, I/O SAPIC, local SAPIC, SMP/IPI, and migration semantics.
#. Add end-to-end storage and DMA tests for IDE, floppy, SCSI, and virtio.
#. Port optional compatibility devices only when platform evidence or firmware
   enumeration requires them.
