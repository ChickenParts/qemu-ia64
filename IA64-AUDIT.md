# IA-64 TCG Target and IPF Machine Audit Report

## Executive Summary

This report presents a comprehensive audit of the IA-64 (Itanium) TCG target and IPF machine implementation in this QEMU fork. The codebase represents a substantial emulation effort (~32,000 lines of IA64-specific code) focused on firmware bringup for EDK2/Xen firmware and Linux kernel boot.

**Overall Assessment**: The implementation is architecturally sound with comprehensive CPU state modeling, but has several incomplete areas and known blockers that prevent full firmware boot. The codebase is clearly under active development with focus on PEI/DXE firmware phase support.

---

## Status Update (2026-01-01)

The following items from the original audit have been addressed since the report
was generated:

- **GDB support** is implemented in `target/ia64/gdbstub.c` and wired up in
  `target/ia64/cpu.c` (525 core registers).
- **MMU index selection** now respects PSR.IT/DT and CPL in
  `target/ia64/cpu.c`, with a CPL-based fallback in `target/ia64/cpu.h`
  for user-only builds.
- **IOSAPIC routing** now includes a working register window, IRQ pin handling,
  EOI/remote IRR tracking, and interrupt delivery in `hw/ia64/ipf.c`.
- **Environment variables** are documented in
  `docs/ia64-environment-variables.md`.

The rest of the audit findings remain relevant unless explicitly noted below.

---

## 1. Code Statistics and Structure

| Component | Location | Lines | Purpose |
|-----------|----------|-------|---------|
| cpu.c | target/ia64/ | 635 | CPU init, reset, interrupt handling |
| cpu.h | target/ia64/ | 405 | CPU state structures, constants |
| helper.c | target/ia64/ | 18,819 | Runtime helpers, TLB, RSE, firmware hooks |
| translate.c | target/ia64/ | 6,650 | TCG code generation, instruction decode |
| helper.h | target/ia64/ | 152 | Helper declarations (153 helpers) |
| ipf.c | hw/ia64/ | 5,571 | Machine init, devices, firmware loading |
| gfw.c | hw/ia64/ | 398 | Guest Firmware HOB builder |
| virt.c | hw/ia64/ | 63 | Alternative minimal machine type |
| **Total** | | **~32,700** | |

---

## 2. Critical Bugs and Issues

### 2.1 GDB Support (RESOLVED)

**Location**: `target/ia64/cpu.c:593-594`
```c
cc->gdb_read_register = NULL; // TODO
cc->gdb_write_register = NULL; // TODO
```

**Update**: Implemented in `target/ia64/gdbstub.c` and registered in
`target/ia64/cpu.c` with a 525-register layout.

**Status**: Resolved.

### 2.2 MMU Index Calculation (RESOLVED)

**Location**: `target/ia64/cpu.h:393-398`
```c
static inline int cpu_ia64_mmu_index(CPUState *cs, bool ifetch)
{
    CPUIA64State *env = cpu_env(cs);
    (void)env;
    return MMU_KERNEL_IDX; // Placeholder
}
```

**Update**: `ia64_cpu_mmu_index()` now respects PSR.IT/DT and CPL to return
physical vs user/kernel indices as appropriate.

**Status**: Resolved.

### 2.3 NaT Propagation Incomplete (MEDIUM)

**Location**: `target/ia64/translate.c:1173` (documented in ia64-firmware-todo.md)

**Issue**: NaT (Not-a-Thing) bits are only propagated for padd/psub operations. Other integer ALU operations ignore NaT bits and silently compute values.

**Impact**: Speculative execution patterns relying on NaT propagation may produce incorrect results.

**Affected Operations**: Most A-unit integer operations except parallel add/sub.

### 2.4 ALAT Modeling Minimal (MEDIUM)

**Location**: `target/ia64/cpu.h:41-48`
```c
/*
 * Advanced Load Address Table (ALAT), used by ld*.a + chk.a.{nc,clr}.
 *
 * For bringup we only model enough to satisfy Linux' usage patterns:
 * - Record ld*.a destinations with their load address + size.
 * - Invalidate overlapping entries on stores.
 * - chk.a.{nc,clr} branches on miss; .clr also clears the entry.
 */
```

**Impact**: Advanced load speculation not fully modeled. Code relying on precise ALAT semantics may behave incorrectly.

### 2.5 RSE Dirty/Clean Partition Tracking Incomplete (MEDIUM)

**Location**: Documented in `docs/ia64-firmware-todo.md:59-60`

**Issue**: Register Stack Engine tracking for dirty/clean partitions is incomplete. Lazy mode (ar.rsc.mode=0) skips eager spills but loadrs/flushrs semantics are not fully modeled.

**Impact**: Complex RSE usage patterns (nested interrupts, unusual backing store manipulations) may produce incorrect behavior.

### 2.6 Floating Point Approximation (LOW-MEDIUM)

**Location**: `target/ia64/cpu.h:22`
```c
uint64_t f[128][2];  /* Floating Point Registers (82-bit, stored as 2x64 for now) */
```

**Issue**: IA-64 uses 82-bit extended precision FP registers. Currently stored as 2x64-bit values.

**Impact**:
- Precision loss in extended-precision operations
- IEEE-754 rounding modes not implemented
- Limited FP operation support (only basic fma, fms, frcpa, fcvt)

---

## 3. Unimplemented Instructions

Based on grep analysis, the following instruction categories generate `gen_unimpl()` calls:

| Category | Location | Description |
|----------|----------|-------------|
| A-slot fallback | translate.c:2756 | Unhandled A-unit encodings |
| B-slot major=4 | translate.c:3078 | Branch type variants |
| B-slot fallback | translate.c:3126 | Unhandled B-unit encodings |
| Reserved templates | translate.c:3138 | Template 0x1, 0x3, etc. |
| break.m hypercall | translate.c:3380 | Some hypercall codes |
| M-slot variants | translate.c:3397, 3939, 4424, 4833, 4842 | Various M-unit encodings |
| I-slot variants | translate.c:5227, 5552, 5972, 5986 | Various I-unit encodings |
| mux1 mbtype | translate.c:5295 | Multimedia permutation |
| dep.z variants | translate.c:5625 | Deposit instructions |
| extr variants | translate.c:5881 | Extract instructions |
| F-slot | translate.c:6293 | Floating point operations |
| X-slot | translate.c:6325, 6417 | Extended slot encodings |

**Recommendation**: Prioritize implementing instructions observed during firmware/kernel execution. Add logging to identify which unimplemented instructions are hit most frequently.

---

## 4. Firmware Blockers (Active)

From `docs/ia64-firmware-blockers.md`:

### 4.1 DXE GCD Assert
**Symptom**: `ASSERT ... Gcd.c Line 1736, Descrpt: Found`
**Cause**: No resource descriptor matching PHIT memory range
**Status**: Blocking DXE phase boot

### 4.2 PHIT Memory Range Mismatch
**Symptom**: HOB shows 16MiB even with `-m 512M`
**Details**: Serial shows `Install PeiMemory ... size = 0x1000000`
**Status**: Under investigation

### 4.3 HOB List Consistency
**Issue**: DXE may use different HOB list than the one patched by QEMU
**Location**: PEI list cloned to 0x3030000

### 4.4 PEI PPI Assert
**Symptom**: `Pei/Ppi/Ppi.c Line 209` with `EFI_INVALID_PARAMETER`
**Status**: Need PPI dispatch tracing

### 4.5 AfterMemMP.c Assert
**Location**: Line 161, hang at `pc=0xffe737a0`
**Status**: Source of failing status needs tracing

---

## 5. Open Technical Debt

From `docs/ia64-firmware-todo.md`:

### 5.1 SAL Systab Collision
**Issue**: Synthetic SAL systab written to fixed low RAM without proper memory allocation from firmware.
**Locations**:
- `target/ia64/helper.c:1849`
- `target/ia64/helper.c:2174`
- `hw/ia64/ipf.c:946`

### 5.2 Build-Specific Firmware Patches
**Issue**: Status-code callgate patch uses fixed FV offsets, build-specific.
**Location**: `hw/ia64/ipf.c:423`

### 5.3 Extended PCI Config (ECAM)
**Issue**: Type-1 extended PCI config not implemented.
**Impact**: Firmware using ECAM for extended config space will fail.

### 5.4 EFI System Table Scanning
**Issue**: Uses heuristic wide-range scanning instead of firmware-provided pointers.
**Location**: `target/ia64/helper.c:2046`

### 5.5 Fixed Flash Layout Assumption
**Issue**: SAL/ESAL Flash access assumes fixed flash base/size.
**Location**: `target/ia64/helper.c:106`

---

## 6. Code Quality Issues

### 6.1 Disabled Compiler Warnings
**Location**: `hw/ia64/ipf.c:87`
```c
// XXX: Disable Wunused-variable and Wunused-parameter and Wunused-function
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
```

**Impact**: Real bugs may be masked by disabled warnings. Multiple commented-out FIXME blocks (lines 2877, 2939, 2970, 3021).

**Recommendation**: Clean up unused code and remove pragma suppressions.

### 6.2 Commented-Out Code Blocks
**Locations**:
- `hw/ia64/ipf.c:2877-3021` - Multiple FIXME blocks
- `hw/ia64/ipf.c:3124` - RTC RAM TODO
- `hw/ia64/ipf.c:5411-5414` - EEPROM/SPD TODO

### 6.3 Excessive Debug State in CPUArchState
**Location**: `target/ia64/cpu.h:111-231`

The CPU state structure contains ~120 lines of debug/trace state fields (b0_trace, dbg_fw_*, fw_pei_*, etc.). While useful for bringup, this adds significant memory overhead per vCPU.

**Recommendation**: Consider compile-time conditionals to exclude debug state in release builds.

### 6.4 Environment Variable Proliferation
The codebase uses numerous environment variables for debugging:
- `QEMU_IA64_BCALL_LOG_*`
- `QEMU_IA64_FW_FASTPATH`
- `QEMU_IA64_FW_R8_TRACE*`
- `QEMU_IA64_HANG_ABORT`
- `QEMU_IPF_FW_WATCH_*`
- `QEMU_IPF_DEBUGCON_*`
- `QEMU_IPF_TRACE_*`
- `QEMU_IPF_VARSTORE_FORCE`
- And many more...

**Recommendation**: Document all environment variables in a single location.

---

## 7. Device Implementation Status

| Device | Status | Notes |
|--------|--------|-------|
| PCI Host Bridge | Complete | Intel 82441FX emulation |
| PIIX4 Southbridge | Complete | ISA compatibility |
| Serial (UART) | Complete | Memory-mapped at 0xff5e0000 |
| IOSAPIC | Basic routing | Register window + IRQ pin delivery |
| ACPI PM | Partial | PM1 registers, timer |
| VGA | Complete | Uses pci_vga_init() |
| Legacy I/O | Complete | Port-to-MMIO translation window |
| Audio | Not implemented | Template code commented out |
| Network | Not implemented | Templates present |
| FDC | Not implemented | Commented out |

**Critical Gap**: IOSAPIC provides only register stubs - no actual interrupt routing. This may cause issues for interrupt-dependent firmware/OS operations.

---

## 8. TLB/Memory Management Analysis

### 8.1 TLB Structure
- ITLB/DTLB: 128 entries each (software-managed)
- ITR/DTR: 16 entries each (translation registers)
- ITC/DTC: 128 entries each (translation cache)

### 8.2 Translation Hierarchy
The `ia64_cpu_tlb_fill()` function implements:
1. Per-CPU canonical range (ar.k3-based)
2. Region 7 direct identity mapping
3. Region 6 uncached I/O mapping
4. Physical mode fallback
5. Virtual translation via RR/TLB/TR lookup

### 8.3 VHPT Not Implemented
The Virtual Hash Page Table (VHPT) translation walk is not modeled. Only TLB entries and TRs are used. This is acceptable for current firmware bringup but may be needed for full OS support.

---

## 9. Exception/Interrupt Handling

### 9.1 Implemented Vectors
All standard IA-64 vectors are defined in `cpu.h:352-369`:
- VHPT translation faults
- TLB misses (instruction/data)
- Key misses
- Dirty/access bit faults
- Break instruction
- External interrupt
- Page not present
- Access rights violations
- Unaligned reference

### 9.2 Interrupt Window Management
The implementation maintains separate frame stacks for:
- `rse_frames[]`: Normal br.call/br.ret windowing
- `intr_frames[]`: Interrupt context snapshots

This separation is architecturally correct and prevents interference between IVT handlers and normal call/return.

---

## 10. Recommendations Summary

### Priority 1 (Critical)
1. **Resolve DXE GCD assert** - Blocking firmware boot
2. **Fix PHIT memory range** - Blocking firmware progression

### Priority 2 (High)
3. **Complete NaT propagation** - Affects correctness
4. **Implement commonly-hit unimplemented instructions** - Prioritize firmware paths
5. **Resolve AfterMemMP/DXE asserts** - Trace failing status sources

### Priority 3 (Medium)
6. **Complete RSE dirty/clean tracking**
7. **Improve FP precision handling**
8. **Add ECAM support for extended PCI config (if firmware requires it)**
9. **Address SAL systab collision risk**

### Priority 4 (Code Quality)
10. **Clean up disabled warnings in ipf.c**
11. **Conditionally compile debug state**
12. **Remove commented-out code blocks**

---

## 11. Testing Infrastructure

### Available Test Scripts
- `scripts/run-ia64-firmware.sh` - Firmware testing
- `scripts/run-ia64-kernel.sh` - Kernel/OS testing
- `scripts/build-ia64-initramfs.sh` - Initramfs builder
- `scripts/ia64_dump_fv.py` - Firmware volume parser

### Missing
- No formal unit tests in `tests/unit/` or `tests/functional/`
- No automated CI integration evident
- No instruction coverage testing

**Recommendation**: Add automated testing for basic instruction decode and execution.

---

## 12. Files Requiring Attention

| File | Priority | Issue |
|------|----------|-------|
| `hw/ia64/ipf.c:87-90` | Medium | Disabled compiler warnings |
| `hw/ia64/ipf.c:2877-3021` | Low | Commented FIXME blocks |
| `target/ia64/translate.c` | Medium | Multiple unimplemented instruction paths |
| `target/ia64/helper.c` | Medium | ~19K lines, complex firmware hooks |

---

## Appendix A: Architecture Overview

The IA-64 architecture has unique features that make emulation challenging:
- **EPIC (Explicitly Parallel Instruction Computing)**: 128-bit bundles with 3 instructions
- **Register Stack Engine (RSE)**: Hardware-managed register windowing
- **Predication**: 64 predicate registers for conditional execution
- **Advanced Loads (ALAT)**: Speculative load tracking
- **82-bit Floating Point**: Extended precision registers

This implementation handles most of these features at a functional level sufficient for firmware bringup, with noted gaps in precision and completeness.

---

## Appendix B: Environment Variables Reference

For a full and maintained list of environment variables, see
`docs/ia64-environment-variables.md`.

---

*Report generated: 2025-12-31*
*Codebase: qemu-ia64 (master branch)*
