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
set around the IA-64 firmware interface.  The table distinguishes a real port
using a current QEMU device model from an in-file shim or an unported device.

.. list-table::
   :header-rows: 1
   :widths: 20 24 56

   * - Historical component
     - Current state
     - Required work
   * - RAM and firmware windows
     - Present
     - Replace image-specific aliases with documented platform maps.
   * - PCI host/root
     - Custom current-QOM model
     - Validate 460GX windows, routing, DMA, and migration state.
   * - 460GX host functions
     - Config-space shadows
     - Convert firmware-visible shadows into explicit chipset devices.
   * - I/O SAPIC
     - In-file partial model
     - Split into a QOM device; complete delivery modes, destination,
       polarity, EOI, and migration.
   * - Local SAPIC
     - Architectural CPU model
     - IRR/IVR/TPR/EOI arbitration is isolated and unit tested; add
       NMI/ExtINT/IPI and migration.
   * - PIIX southbridge
     - Function 0 only
     - Wire one coherent PIIX4 instance and use its ISA/IDE/UHCI/PM/SMBus
       child functions where appropriate.
   * - ISA bus
     - Standalone workaround
     - Derive it from the southbridge and route IRQs through the platform.
   * - 8259 PIC
     - Not ported
     - Decide from platform documentation whether legacy PIC exposure is
       required; do not fake it in CPU code.
   * - RTC/CMOS
     - Current QEMU RTC
     - Integrate with southbridge/reset and validate firmware-visible contents.
   * - Serial
     - One serial-mm UART
     - Validate the real IPF address/IRQ; add legacy ports only if specified.
   * - Parallel
     - Not ported
     - Port only if part of the chosen IPF compatibility platform.
   * - VGA
     - Current PCI VGA
     - Keep payload-independent; validate PCI routing and firmware discovery.
   * - IDE
     - Not ported
     - Wire the PIIX IDE function and drives.
   * - i8042 keyboard/mouse
     - Not ported
     - Wire through the coherent ISA path.
   * - ISA DMA
     - Not ported
     - Add with ISA devices that require it.
   * - Floppy
     - Not ported
     - Optional compatibility device after DMA/PIC routing is correct.
   * - UHCI USB
     - Not ported
     - Wire the PIIX UHCI function.
   * - ACPI PM/SMBus
     - In-file PM shim; no SMBus
     - Replace with PIIX4 PM/SMBus and populate documented EEPROM devices.
   * - Network
     - Current PCI NIC creation
     - Validate INTx routing and defaults; ISA NE2000 remains unported.
   * - SCSI
     - Not ported by default
     - Reintroduce supported PCI SCSI enumeration and interrupt routing.
   * - Virtio block
     - Generic PCI available
     - Validate firmware support and INTx; do not special-case it in IPF code.
   * - Audio
     - Not ported
     - Optional after core platform devices.
   * - SMP/IPI
     - Incomplete
     - Implement LID targeting, IPI space, rendezvous/reset, and per-CPU state.
   * - Migration
     - Incomplete
     - Add VMState for CPU architectural state and every custom IPF device.

Current assessment
------------------

The original current-tree import contained roughly nine hundred lines of
largely disabled, old-QEMU PC-machine scaffolding.  The current ``ipf.c`` is
roughly 6.7 thousand lines, but line growth is not device-port completion.  A
large share is firmware table construction, tracing, flash/NVRAM handling, and
firmware compatibility machinery.  Fewer than half of the historical device
categories are currently represented by a coherent, current-QEMU device path,
and several represented categories are partial shims rather than finished
ports.

Priority order
--------------

#. Keep local SAPIC arbitration in the CPU and unit-test its architectural
   contract.
#. Remove guest-firmware mutation from MMU and instruction execution paths.
#. Make one payload-independent machine topology.
#. Extract the I/O SAPIC and 460GX functions into migratable QOM devices.
#. Integrate a coherent southbridge/ISA/IDE/UHCI/PM/SMBus topology.
#. Port the remaining compatibility devices through current QEMU APIs.
#. Validate Xen firmware, SDV firmware, and direct kernels independently.
