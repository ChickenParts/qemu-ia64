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
* Scratch patches, firmware call gates, compatibility probes, and arbitrary
  host delays are not acceptable substitutes for production code or tests.
* A behavior change needs an architectural or device-level test whenever the
  contract can be isolated from a complete firmware boot.

Completion rule
---------------

A matching model name, source symbol, successful firmware boot, or historical
function body is not enough to mark a component complete.  Completion requires
review of its register interface, reset values, interrupt and DMA behavior,
firmware discovery, migration state, multi-CPU behavior, and a focused test or
guest probe.  The structural and runtime audit scripts report evidence toward
that review; they do not certify correctness.

Historical device inventory and current state
----------------------------------------------

The inherited machine configured a PC-derived compatibility device set around
an IA-64 firmware interface.  The current tree has now replaced several of the
largest machine-local shims with normal QEMU devices, but the 460GX host model,
ACPI exposure, PCI routing, SMP, and firmware compatibility paths remain
substantial work.

.. list-table::
   :header-rows: 1
   :widths: 20 27 53

   * - Historical component
     - Current state
     - Required work
   * - RAM and firmware windows
     - Present, with machine-local mapping and aliases
     - Derive every window and alias from the selected platform contract;
       remove image-shaped mappings and validate reset and migration.
   * - PCI host/root
     - Custom current-QOM host and root function
     - Validate 460GX windows, configuration cycles, INTx routing, DMA address
       spaces, reset, and migration.
   * - 460GX host functions
     - Machine-local configuration shadows and MMIO registers
     - Replace the shadows with explicit migratable chipset devices and focused
       configuration/MMIO tests.
   * - I/O SAPIC
     - Separate 48-input SysBus/QOM device with VMState
     - Edge and level requests, masking, Remote IRR, shared-vector EOI,
       rejected-delivery retry, and reset have focused coverage.  Add the
       platform's remaining delivery modes and multi-CPU destination routing.
   * - Local SAPIC
     - Architectural CPU model with isolated arbitration tests
     - Complete IPI, NMI/ExtINT, multi-CPU targeting, reset/rendezvous, and
       migration state.
   * - PIIX southbridge
     - One coherent PIIX4 multifunction device
     - Functions 0 through 3 are realized together.  Continue validating PCI
       routing registers, reset, migration, and firmware enumeration.
   * - ISA bus
     - Owned by PIIX4; IRQ lines 0 through 15 feed the I/O SAPIC
     - Validate polarity and trigger expectations for each attached source and
       document the chosen IA-64 legacy-interrupt contract.
   * - 8259 PIC
     - Deliberately disabled in the PIIX4 instance
     - Confirm from platform documentation whether a visible legacy PIC is
       required.  Do not emulate one in CPU helpers or leave a dead PIC output.
   * - RTC/CMOS
     - PIIX4 MC146818 RTC with machine CMOS initialization
     - Validate reset values and firmware-visible memory, and stop advertising
       absent equipment in the CMOS equipment byte.
   * - Serial
     - One MMIO UART
     - Validate the platform address and interrupt route; add legacy COM ports
       only when the compatibility platform requires them.
   * - Parallel
     - Not ported
     - Port only if required by the selected compatibility platform.
   * - VGA
     - Current PCI VGA, payload-independent
     - Validate firmware discovery, BAR placement, legacy aperture ownership,
       and INTx routing.
   * - IDE
     - PIIX4 IDE function with normal drive attachment
     - Add configuration, channel IRQ, reset, DMA, and firmware-enumeration
       coverage.
   * - i8042 keyboard/mouse
     - Not ported
     - Attach through the PIIX ISA bus if specified; until then CMOS must not
       claim that a PS/2 mouse exists.
   * - ISA DMA
     - Not ported
     - Add before any compatibility device that depends on it.
   * - Floppy
     - Not ported
     - Optional after the DMA and interrupt contracts are established.
   * - UHCI USB
     - PIIX4 UHCI function follows the machine USB setting
     - Validate PCI identity, interrupt routing, reset, and guest operation.
   * - ACPI PM/SMBus
     - PIIX4 PM/SMBus plus populated SMBus EEPROMs; old MMIO PM shim remains
     - Reconcile the firmware ACPI tables and Generic Address Structures with
       one authoritative PM implementation.  Remove duplicate state only after
       proving which interface the platform specifies.
   * - Network
     - Current PCI NIC creation
     - Validate defaults, BARs, DMA, and INTx end to end; the historical ISA
       NE2000 path remains unported.
   * - SCSI
     - Not present by default
     - Reintroduce the documented PCI SCSI controller and test discovery, DMA,
       reset, and interrupts.
   * - Virtio block
     - Generic PCI model available
     - Validate firmware support and PCI interrupt behavior without adding
       IPF-specific shortcuts.
   * - Audio
     - Not ported
     - Optional after the core platform is complete.
   * - SMP/IPI
     - Incomplete
     - Implement LID targeting, IPI space, startup/rendezvous, per-CPU reset,
       and multi-CPU interrupt delivery.
   * - Migration
     - Partial
     - The I/O SAPIC has VMState; complete CPU, chipset, firmware/NVRAM, and
       every remaining custom machine state before claiming migration support.

Current assessment
------------------

The device topology is now independent of firmware versus direct-kernel boot.
The I/O SAPIC is no longer embedded in ``ipf.c``, and PIIX4 now owns the single
ISA bus, RTC, IDE, UHCI, PM, and SMBus functions.  Focused tests exercise the
local-SAPIC arbitration contract, the I/O-SAPIC core, all four PIIX4 PCI
functions, and a real PIIX4 SCI path through ISA IRQ 9 and the I/O SAPIC.

That progress does not make the machine complete.  The current ``ipf.c`` still
contains machine-local 460GX register shadows, a second ACPI PM implementation,
firmware table and flash/NVRAM machinery, extensive diagnostic paths, and
firmware compatibility code that must be audited for guest mutation and
trace-shaped behavior.  PCI INTx beyond the SCI test, DMA contracts, SMP/IPI,
and several compatibility devices remain unproven or absent.  Neither a Xen nor
an SDV firmware milestone may be used to waive those platform obligations.

Priority order
--------------

#. Remove firmware mutation, call gates, magic-PC behavior, synthetic success,
   and other trace-shaped compatibility code by fixing the underlying CPU,
   memory, firmware-interface, or device contract.
#. Convert the 460GX host functions and MMIO shadows into explicit QOM devices
   with reset, VMState, and focused tests.
#. Reconcile the machine-local ACPI PM MMIO block with PIIX4 PM/SMBus and the
   firmware-visible ACPI tables, preserving only the documented platform
   interface.
#. Validate PCI INTx, BAR placement, and DMA end to end for VGA, IDE, USB,
   network, and the selected SCSI controller.
#. Add only the documented remaining ISA compatibility devices through the
   coherent PIIX/ISA path.
#. Complete SMP, IPI, local-SAPIC targeting, and per-CPU migration state.
#. Run Xen firmware, SDV firmware, and direct kernels as independent regression
   lanes against the same machine topology.
