# IA-64/IPF Environment Variables Reference

This document describes the environment variables available for debugging and tracing
the QEMU IA-64/IPF emulation. These variables are primarily useful for firmware
development, kernel debugging, and understanding emulator behavior.

## Overview

Environment variables are organized into the following categories:
- **IPF Machine Configuration** - Machine-level settings (QEMU_IPF_*)
- **IA-64 CPU Debugging** - CPU instruction and execution tracing (QEMU_IA64_*)
- **Firmware Tracing** - EFI/UEFI firmware debugging
- **PEI Debugging** - Pre-EFI Initialization phase debugging
- **Memory Watchpoints** - Monitor memory reads/writes

## IPF Machine Configuration

### NVRAM and Variable Store

| Variable | Description |
|----------|-------------|
| `QEMU_IPF_NVRAM_FORCE` | Force initialization of NVRAM even if it already contains data |
| `QEMU_IPF_VARSTORE_FORCE` | Force initialization of UEFI variable store |

### Firmware Configuration

| Variable | Description |
|----------|-------------|
| `QEMU_IPF_DUMP_HOB` | Dump Hand-Off Block (HOB) contents on boot |
| `QEMU_IPF_FW_SCAN` | Enable firmware binary scanning and analysis |
| `QEMU_IPF_FW_PEI_PI` | Use PI-style PEI handoff (SEC handoff + PPI list); set 0/false/no for framework startup descriptor |
| `QEMU_IPF_FW_PROBE_FIT` | Probe and log Firmware Interface Table entries |
| `QEMU_IPF_FW_PATCH_FIT` | Enable FIT patching for compatibility |
| `QEMU_IPF_FW_PATCH_GP_GLOBALS` | Patch global pointer references in firmware |
| `QEMU_IPF_FW_MEMMAP_TABLE` | Populate firmware memmap table at 0x2000000 (set 0/false/no to disable) |
| `QEMU_IPF_FW_REGION` | Specify firmware memory region (format: start-end) |
| `QEMU_IPF_FW_DXE_DUMP` | Dump DXE (Driver Execution Environment) phase info |
| `QEMU_IPF_FW_WATCH_DXE` | Enable DXE phase watchpoints |
| `QEMU_IPF_FW_WATCH_RANGE` | Specify address range to watch (format: start-end) |

### Debug Console

| Variable | Description |
|----------|-------------|
| `QEMU_IPF_DEBUGCON_CTX` | Enable context output on debug console |
| `QEMU_IPF_DEBUGCON_LINE` | Enable line-buffered debug console output |
| `QEMU_IPF_DEBUGCON_QEMU_LOG` | Redirect debug console to QEMU log |
| `QEMU_IPF_UART_LINE_TRACE` | Trace UART line output |

### DXE Phase Debugging

| Variable | Description |
|----------|-------------|
| `QEMU_IPF_DXE_TRACE` | Enable DXE driver loading trace |
| `QEMU_IPF_DUMP_HOB_ON_ASSERT` | Dump HOB when firmware assert occurs |

### I/O and MMIO Tracing

| Variable | Description |
|----------|-------------|
| `QEMU_IPF_TRACE_PCI` | Trace PCI configuration space access |
| `QEMU_IPF_TRACE_VGA` | Trace VGA I/O port access |
| `QEMU_IPF_TRACE_POST` | Trace POST code output (port 0x80) |
| `QEMU_IPF_TRACE_IOPORTS` | Trace specific I/O ports (format: start-end) |
| `QEMU_IPF_TRACE_LIMIT` | Limit number of trace entries |
| `QEMU_IPF_TRACE_MMIO` | Trace MMIO access (set to address or 1 for all) |
| `QEMU_IPF_TRACE_MMIO_READ` | Trace MMIO reads only |
| `QEMU_IPF_TRACE_MMIO_LIMIT` | Limit MMIO trace entries |

## IA-64 CPU Debugging

### General Debugging

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_HANG_ABORT` | Abort on detected hang/loop |
| `QEMU_IA64_HANG_LOG_EVERY` | Log every N iterations when looping |
| `QEMU_IA64_HANG_DUMP_GCD` | Dump GCD (Global Coherency Domain) on hang |
| `QEMU_IA64_HANG_DUMP_PC` | Dump specific PC on hang |
| `QEMU_IA64_ABORT_PANIC` | Abort QEMU on panic |
| `QEMU_IA64_SP_IN_TEXT` | Check for stack pointer in text section |

### Instruction Tracing

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_MOVB_LOG` | Log MOV to branch register instructions |
| `QEMU_IA64_MOVB_LOG_LIMIT` | Limit MOV BR log entries |
| `QEMU_IA64_MOVB_LOG_B` | Filter by specific branch register |
| `QEMU_IA64_MOVB_LOG_DUMP` | Dump bundle on MOV BR |
| `QEMU_IA64_TB_LOG_LIMIT` | Log first N translated TB start PCs |
| `QEMU_IA64_TB_LOG_MIN_PC` | Filter TB log start (inclusive) |
| `QEMU_IA64_TB_LOG_MAX_PC` | Filter TB log end (inclusive) |
| `QEMU_IA64_MOVRB_LOG` | Log MOV from branch register instructions |
| `QEMU_IA64_BRL_LOG` | Log long branch instructions |
| `QEMU_IA64_RSE_STRICT_TRACE` | Enable strict RSE transition tracing (`alloc`/`loadrs`/`flushrs`/`set_bspstore`/`ret_restore`) |
| `QEMU_IA64_RSE_STRICT_TRACE_LIMIT` | Limit RSE strict trace entries |

### Branch/Call Tracing

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_BCALL_LOG_MIN_PC` | Minimum PC for branch/call logging |
| `QEMU_IA64_BCALL_LOG_MAX_PC` | Maximum PC for branch/call logging |
| `QEMU_IA64_BCALL_LOG_LIMIT` | Limit branch/call log entries |
| `QEMU_IA64_B7_TRACE` | Trace B7 register writes |
| `QEMU_IA64_B7_TRACE_LIMIT` | Limit B7 trace entries |
| `QEMU_IA64_CALL_WATCH_PC` | Watch calls from specific PC |
| `QEMU_IA64_CALL_TRACE_PC` | Trace calls from specific PC |
| `QEMU_IA64_DBG_CALL_PC` | Debug calls at specific PC |
| `QEMU_IA64_DBG_CALL_LIMIT` | Limit call debug entries |

### Memory Watchpoints

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_WATCH_READ` | Watch memory reads at address |
| `QEMU_IA64_WATCH_SIZE` | Size of watch region |
| `QEMU_IA64_WATCH_LIMIT` | Limit watch hits |
| `QEMU_IA64_WATCH_DATA` | Watch for specific data value |
| `QEMU_IA64_WATCH_DATA2` | Second data value to watch |
| `QEMU_IA64_WATCH_TEXT` | Watch execution at text address |
| `QEMU_IA64_WATCH_LOAD_ADDR` | Watch loads from address |
| `QEMU_IA64_WATCH_LOAD_RANGE` | Watch load address range |
| `QEMU_IA64_WATCH_LOAD_LIMIT` | Limit load watch hits |
| `QEMU_IA64_WATCH_STORE` | Watch stores to address |
| `QEMU_IA64_WATCH_STORE_RANGE` | Watch store address range |
| `QEMU_IA64_WATCH_STORE_VALUE` | Watch for specific store value |
| `QEMU_IA64_WATCH_STORE_VALUE_MASK` | Mask for store value matching |
| `QEMU_IA64_WATCH_STORE_LIMIT` | Limit store watch hits |
| `QEMU_IA64_WATCH_R33` | Watch R33 register changes |

### Unit Debugging

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_DBG_CMP` | Debug compare instructions |
| `QEMU_IA64_DBG_CMP_PC` | Filter compare debug by PC |
| `QEMU_IA64_DBG_SXT` | Debug sign extension instructions |
| `QEMU_IA64_DBG_SXT_PC` | Filter SXT debug by PC |
| `QEMU_IA64_DBG_IUNIT` | Debug I-unit instructions |
| `QEMU_IA64_DBG_IUNIT_PC` | Filter I-unit debug by PC |
| `QEMU_IA64_DBG_BUNIT` | Debug B-unit instructions |
| `QEMU_IA64_DBG_BUNIT_PC` | Filter B-unit debug by PC |
| `QEMU_IA64_DBG_BRET` | Debug branch return instructions |
| `QEMU_IA64_DBG_BRET_PC` | Filter BRET debug by PC |
| `QEMU_IA64_DBG_MUNIT` | Debug M-unit instructions |
| `QEMU_IA64_DBG_MUNIT_PC` | Filter M-unit debug by PC |
| `QEMU_IA64_DBG_AR_LC` | Debug AR.LC changes |
| `QEMU_IA64_ALAT_TRACE` | Trace ALAT record/check/invalidate events |
| `QEMU_IA64_ALAT_TRACE_LIMIT` | Limit ALAT trace entries |

### Probe Debugging

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_DBG_PROBE` | Enable probe instruction debugging |
| `QEMU_IA64_DBG_PROBE_RANGE` | Probe address range |
| `QEMU_IA64_DBG_PROBE_LIMIT` | Limit probe debug entries |
| `QEMU_IA64_DBG_PROBE_DUMP_R2` | Dump R2 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_R8` | Dump R8 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_R12` | Dump R12 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_R30` | Dump R30 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_R31` | Dump R31 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_R32` | Dump R32 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_R33` | Dump R33 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_R34` | Dump R34 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_R35` | Dump R35 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_R36` | Dump R36 on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_ADDR` | Dump memory at address on probe |
| `QEMU_IA64_DBG_PROBE_DUMP_ADDR_LEN` | Length of memory dump |
| `QEMU_IA64_DBG_PROBE_HOB_FAILFAST` | Fail fast on HOB probe issues |

### Loop and String Debugging

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_DBG_LOOP_TRACE` | Trace loop detection |
| `QEMU_IA64_DBG_LOOP_LIMIT` | Limit loop trace entries |
| `QEMU_IA64_DBG_STR` | Debug string operations |
| `QEMU_IA64_DBG_STR_LIMIT` | Limit string debug entries |

### Register Debugging

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_DBG_R12` | Debug R12 (stack pointer) changes |
| `QEMU_IA64_DBG_R12_DUMP` | Dump bundle on R12 change |
| `QEMU_IA64_DBG_R12_LIMIT` | Limit R12 debug entries |
| `QEMU_IA64_DBG_PEIMAGE` | Debug PE image loading |
| `QEMU_IA64_DBG_PEIMAGE_DUMP` | Dump PE image details |

### Break Instruction Handling

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_BREAK_LOG` | Log break instructions |
| `QEMU_IA64_BREAK_ABORT` | Abort on break instruction |
| `QEMU_IA64_BREAK_DUMP` | Dump state on break |
| `QEMU_IA64_BREAK_DUMP_BUNDLES` | Dump surrounding bundles on break |
| `QEMU_IA64_LOG_BREAK` | Log break with specific immediate |
| `QEMU_IA64_LOG_BREAK_STR` | Log break with string output |

## Firmware Debugging

### General Firmware Tracing

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_FW_LOG` | Enable general firmware logging |
| `QEMU_IA64_FW_FASTPATH` | Enable firmware fast-path optimizations |
| `QEMU_IA64_FW_FASTPATH_TRACE` | Trace fast-path execution |
| `QEMU_IA64_FW_FASTPATH_TRACE_LIMIT` | Limit fast-path trace entries |
| `QEMU_IA64_FW_FASTPATH_TRACE_MATCH_SRC` | Trace fast-path only when source matches |
| `QEMU_IA64_FW_FASTPATH_TRACE_MATCH_DST_RANGE` | Trace fast-path only when destination is in range |
| `QEMU_IA64_FW_FASTPATH_TRACE_MATCH_LEN` | Trace fast-path only when length matches |
| `QEMU_IA64_FW_R8_TRACE` | Trace R8 (return value) |
| `QEMU_IA64_FW_R8_TRACE_MIN_PC` | Minimum PC for R8 trace |
| `QEMU_IA64_FW_R8_TRACE_MAX_PC` | Maximum PC for R8 trace |
| `QEMU_IA64_FW_R8_TARGET` | Specific R8 value to trace |
| `QEMU_IA64_FW_R8_DUMP` | Dump state on R8 match |
| `QEMU_IA64_FW_R8_DUMP_BUNDLES` | Bundles to dump on R8 match |
| `QEMU_IA64_FW_CALL_TRACE` | Trace firmware function calls |
| `QEMU_IA64_FW_CALL_TRACE_LIMIT` | Limit call trace entries |
| `QEMU_IA64_FW_DUMP_PC` | Dump state at specific PC |
| `QEMU_IA64_FW_DUMP_BUNDLES` | Number of bundles to dump |

### Firmware Break Handling

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_FW_BREAK_HYPERCALL` | Break on hypercall |
| `QEMU_IA64_FW_BREAK_LOG` | Log firmware breaks |
| `QEMU_IA64_FW_BREAK0_DUMP` | Dump on break 0 |
| `QEMU_IA64_FW_BREAK0_DUMP_LEN` | Length of break 0 dump |
| `QEMU_IA64_FW_BREAK0_ABORT_ASSERT` | Abort on firmware assert |
| `QEMU_IA64_FW_BREAK0_SCAN_ALWAYS` | Always scan on break 0 |
| `QEMU_IA64_FW_BREAK0_SCAN_LIMIT` | Limit break 0 scans |
| `QEMU_IA64_FW_BREAK0_LOG_LIMIT` | Limit break 0 log entries |
| `QEMU_IA64_FW_BREAK0_STACK_DUMP` | Dump stack on break 0 |
| `QEMU_IA64_FW_BREAK0_STACK_DUMP_LEN` | Stack dump length |
| `QEMU_IA64_FW_BREAK0_DUMP_FILE` | File to dump break 0 info |
| `QEMU_IA64_FW_GCD_DUMP` | Dump GCD on break |

### FVB (Firmware Volume Block) Tracing

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_FW_FVB_TRACE` | Trace FVB operations |
| `QEMU_IA64_FW_BOOTLOOP_LOG` | Log boot loop detection |

### SAL (System Abstraction Layer)

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_FW_SAL_TRACE` | Trace SAL calls |
| `QEMU_IA64_FW_SAL_TRACE_LIMIT` | Limit SAL trace entries |
| `QEMU_IA64_SAL_CALL_ABORT` | Abort on SAL call |
| `QEMU_IA64_SAL_CALL_ABORT_PC` | Abort SAL call at specific PC |
| `QEMU_IA64_SAL_CALL_DUMP` | Dump SAL call details |
| `QEMU_IA64_SAL_RET_ABORT` | Abort on SAL return |
| `QEMU_IA64_SAL_PCI_FAILFAST` | Fail fast on SAL PCI issues |
| `QEMU_IA64_SAL_PCI_DUMP` | Dump SAL PCI operations |
| `QEMU_IA64_SAL_PCI_DUMP_BUNDLES` | Dump bundles on SAL PCI |

### Firmware Storage

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_FW_FLASH_BASE` | Override flash window base used by ESAL/FVB and scans |
| `QEMU_IA64_FW_FLASH_SIZE` | Override flash window size (aligned to 64KiB blocks) |

## PEI (Pre-EFI Initialization) Debugging

### General PEI Tracing

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_FW_PEI_LOG` | Enable PEI phase logging |
| `QEMU_IA64_PEI_CALL_TRACE` | Trace PEI function calls |
| `QEMU_IA64_PEI_PS_SCAN` | Scan PEI services |
| `QEMU_IA64_PEI_PS_DUMP` | Dump PEI services |
| `QEMU_IA64_PEI_PS_DUMP_TABLE` | Dump PEI service table |
| `QEMU_IA64_PEI_STARTUP_DUMP` | Dump PEI startup info |
| `QEMU_IA64_PEI_DISPATCH_DUMP` | Dump PEI dispatch info |

### PPI (PEI-to-PEI Interface)

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_PEI_PPI_DUMP` | Dump PPI installations |
| `QEMU_IA64_PEI_PPI_DUMP_ALWAYS` | Always dump PPI info |
| `QEMU_IA64_PEI_PPI_LIST_DUMP` | Dump PPI list |
| `QEMU_IA64_PEI_LOCATE_TRACE` | Trace PPI locate operations |
| `QEMU_IA64_PEI_LOCATE_TRACE_LIMIT` | Limit locate trace entries |
| `QEMU_IA64_PEI_LOCATE_FIX` | Fix StatusCode LocatePpi return when StatusCode PPI is present |
| `QEMU_IA64_PEI_INSTALL_TRACE` | Trace PPI installations |
| `QEMU_IA64_PEI_INSTALL_TRACE_LIMIT` | Limit install trace entries |
| `QEMU_IA64_PEI_PRE_INSTALL_PROBE` | Probe before PPI install |
| `QEMU_IA64_PEI_INSTALL_PPLIST_TRACE` | Trace PPI list changes |
| `QEMU_IA64_PEI_INSTALL_PPLIST_ADDR` | Watch PPI list address |

### PEI Memory Operations

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_PEI_STORE_WATCH` | Watch PEI stores |
| `QEMU_IA64_PEI_STORE_WATCH_LIMIT` | Limit store watch entries |
| `QEMU_IA64_PEI_LOAD_WATCH` | Watch PEI loads |
| `QEMU_IA64_PEI_LOAD_WATCH_LIMIT` | Limit load watch entries |
| `QEMU_IA64_PEI_MEMDUMP` | Dump memory at address |
| `QEMU_IA64_PEI_RSE_TRACE` | Trace RSE (Register Stack Engine) |

### PEI Error Handling

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_PEI_ERR_LIMIT` | Limit PEI error reports |
| `QEMU_IA64_PEI_ERR_ABORT` | Abort on PEI error |
| `QEMU_IA64_PEI_ERR_ABORT_PC` | Abort PEI error at specific PC |
| `QEMU_IA64_PEI_ABORT_A1_7E` | Abort on A1=0x7E condition |
| `QEMU_IA64_PEI_PTR_CHAIN_POST_ABORT` | Abort after pointer chain |
| `QEMU_IA64_PEI_OOR_DUMP` | Dump on out-of-range access |
| `QEMU_IA64_PEI_OOR_DUMP_BUNDLES` | Bundles to dump on OOR |
| `QEMU_IA64_PEI_CLEAR_OLDCORE` | Clear old core memory |
| `QEMU_IA64_PEI_STATUSCODE_SEMANTIC_FIX` | Treat unresolved StatusCode report path as optional (default on) |
| `QEMU_IA64_PEI_STATUSCODE_SEMANTIC_FIX_LOG_LIMIT` | Limit StatusCode semantic-fix logs |
| `QEMU_IA64_PEI_NOTIFY_TRACE` | Trace `NotifyPpi` return status for the blocker callback path (default off) |
| `QEMU_IA64_PEI_NOTIFY_TRACE_LIMIT` | Limit notify-return trace entries |
| `QEMU_IA64_PEI_NOTIFY_TRACE_ONESHOT` | Log only first soft-error notify return when enabled (default on) |
| `QEMU_IA64_PEI_NOTIFY_STATUS_FIX` | Enable bounded notify-return soft-error rewrite for traced GUID/path (default off) |
| `QEMU_IA64_PEI_NOTIFY_STATUS_FIX_ALWAYS` | Ignore unresolved-path guard for notify-return rewrite |
| `QEMU_IA64_PEI_NOTIFY_STATUS_FIX_LOG_LIMIT` | Limit notify-return fix logs |
| `QEMU_IA64_PEI_22560_TRACE` | Trace status-mutation context at `pc=0xffe22560` (default off) |
| `QEMU_IA64_PEI_22560_TRACE_LIMIT` | Limit `pc=0xffe22560` trace entries |
| `QEMU_IA64_PEI_22560_STATUS_FIX` | Enable bounded soft-error rewrite at `pc=0xffe22560` when StatusCode locate/report chain matches (default off) |
| `QEMU_IA64_PEI_22560_STATUS_FIX_ALWAYS` | Ignore unresolved-path guard for `pc=0xffe22560` rewrite |
| `QEMU_IA64_PEI_22560_STATUS_FIX_LOG_LIMIT` | Limit `pc=0xffe22560` rewrite logs |
| `QEMU_IA64_PEI_279D0_TRACE` | Trace non-EFI status mutation path at `pc=0xffe279d0..0xffe27a10` (default off) |
| `QEMU_IA64_PEI_279D0_TRACE_LIMIT` | Limit `pc=0xffe279d0..0xffe27a10` trace entries |
| `QEMU_IA64_PEI_279D0_STATUS_FIX` | Enable bounded rewrite for non-EFI status signature (`0xffffffff0011fff0/0xffffffff0011bff0`) in the `pc=0xffe279d0..0xffe27a10` window (default off) |
| `QEMU_IA64_PEI_279D0_STATUS_FIX_ALWAYS` | Allow fallback PS-link guard bypass for `pc=0xffe279d0` bounded rewrite |
| `QEMU_IA64_PEI_279D0_STATUS_FIX_LOG_LIMIT` | Limit `pc=0xffe279d0` rewrite logs |
| `QEMU_IA64_PEI_279D0_SAFE_MODE` | Quarantine `pc=0xffe279d0` rewrite path (while still tracing/probing) to avoid host abort during investigation (default on) |
| `QEMU_IA64_PEI_279D0_SAFE_MODE_LOG_LIMIT` | Limit `279d0` safe-mode block/probe logs |
| `QEMU_IA64_PEI_REPORT_STATUS_SOFTFAIL` | Legacy report_status soft-fail path (kept for compatibility) |
| `QEMU_IA64_PEI_REPORT_STATUS_SOFTFAIL_ALWAYS` | Legacy override to always soft-fail report_status errors |
| `QEMU_IA64_PEI_HOB_FLOW_TRACE` | Trace PEI `GetHobList`/`CreateHob` call-return pointer contract (default off) |
| `QEMU_IA64_PEI_HOB_FLOW_TRACE_LIMIT` | Limit PEI HOB-flow trace entries |
| `QEMU_IA64_PEI_HOB_PTR_FIX` | Repair successful `GetHobList` returns with null/invalid out HOB pointer (default off; bisect guard) |
| `QEMU_IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | Limit PEI HOB-pointer fix logs |
| `QEMU_IA64_PEI_CREATE_HOB_PTR_GUARD` | Guard `CreateHob` OOR returns when output HOB pointer is null/invalid (default off; bisect guard) |
| `QEMU_IA64_PSR_LOG` | Log PSR (Processor Status Register) changes |

## HOB (Hand-Off Block) Debugging

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_EFI_HOB_DUMP` | Dump HOB contents |
| `QEMU_IA64_EFI_HOB_PATCH` | Enable HOB patching |
| `QEMU_IA64_EFI_HOB_DUMP_AFTER_PATCH` | Dump HOB after patching |
| `QEMU_IA64_EFI_HOB_PATCH_TRACE` | Trace HOB patch operations |
| `QEMU_IA64_EFI_HOB_FORCE_RAM` | Force HOB RAM type |
| `QEMU_IA64_EFI_MEMTYPE_HOB` | Set memory type in HOB |

## Kernel-Level Debugging

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_LOG_K3` | Log K3 (kernel level 3) events |
| `QEMU_IA64_LOG_K3_LIMIT` | Limit K3 log entries |
| `QEMU_IA64_LOG_K4` | Log K4 (kernel level 4) events |
| `QEMU_IA64_LOG_K4_LIMIT` | Limit K4 log entries |
| `QEMU_IA64_LOG_K5` | Log K5 (kernel level 5) events |
| `QEMU_IA64_LOG_K5_LIMIT` | Limit K5 log entries |
| `QEMU_IA64_FORCE_K3` | Force K3 mode |
| `QEMU_IA64_LOG_BSPSTORE` | Log BSP store operations |
| `QEMU_IA64_DIVHELP_LOG` | Log division helper calls |
| `QEMU_IA64_MANUAL_CALL_LOG` | Log manual call linkage |
| `QEMU_IA64_RET_UNWIND_PFS` | Enable return PFS unwinding |
| `QEMU_IA64_RET_WATCH_B0` | Watch B0 on returns |
| `QEMU_IA64_RET_TRACE_RANGE` | Trace ret_restore/b0 in a PC range (start-end or start+len) |
| `QEMU_IA64_RET_TRACE_LIMIT` | Limit return trace entries |

## Assert and Debug Buffer

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_DBG_ASSERT_BUF` | Address of assert buffer |
| `QEMU_IA64_DBG_ASSERT_BUF_LEN` | Length of assert buffer |
| `QEMU_IA64_DBG_ASSERT_BUF_DUMP` | Dump assert buffer |
| `QEMU_IA64_DBG_DUMP_CODE` | Dump code on debug |
| `QEMU_IA64_DBG_DUMP_BUNDLES` | Number of bundles to dump |
| `QEMU_IA64_ABORT_NULL_BRANCH` | Abort on null branch target |
| `QEMU_IA64_ABORT_BRANCH_TO` | Abort when branching to address |
| `QEMU_IA64_CALL_NULL_FIX` | Repair known firmware null `br.call` target path (`pc=0x1ff4f520`) (default off; bisect guard) |
| `QEMU_IA64_CALL_NULL_FIX_LOG_LIMIT` | Limit null-call fix log entries |
| `QEMU_IA64_UNIMPL_DUMP` | Dump bundles when hitting an unimplemented instruction |
| `QEMU_IA64_UNIMPL_DUMP_BUNDLES` | Bundles to dump for unimplemented instructions |

## Trace Call System

| Variable | Description |
|----------|-------------|
| `QEMU_IA64_TRACE_CALL_PC` | Trace calls from specific PC |
| `QEMU_IA64_TRACE_CALL_TGT` | Trace calls to specific target |
| `QEMU_IA64_TRACE_CALL_TGT_A1_MIN` | Min A1 value for target trace |
| `QEMU_IA64_TRACE_CALL_PC_ABORT` | Abort on traced PC call |
| `QEMU_IA64_TRACE_CALL_TGT_ABORT` | Abort on traced target call |
| `QEMU_IA64_TRACE_CALL_RANGE` | Trace call range (start-end) |
| `QEMU_IA64_TRACE_CALL_RANGE_LIMIT` | Limit range trace entries |
| `QEMU_IA64_TRACE_CALL_RANGE_ABORT` | Abort on range trace |
| `QEMU_IA64_TRACE_CALL_MATCH_A0` | Match A0 value for trace |
| `QEMU_IA64_TRACE_CALL_MATCH_A1` | Match A1 value for trace |
| `QEMU_IA64_TRACE_CALL_MATCH_ABORT` | Abort on match |
| `QEMU_IA64_TRACE_CALL_STATUS` | Trace call status |
| `QEMU_IA64_TRACE_CALL_STATUS_DUMP` | Dump status on trace |
| `QEMU_IA64_TRACE_CALL_DUMP_PEICORE` | Dump PEI core on trace |
| `QEMU_IA64_TRACE_CALL_DUMP_ARGS` | Dump call arguments |
| `QEMU_IA64_TRACE_SUSP_CALLS` | Trace suspicious calls |
| `QEMU_IA64_TRACE_SUSP_CALLS_LIMIT` | Limit suspicious call trace |
| `QEMU_IA64_TRACE_SUSP_CALLS_ABORT` | Abort on suspicious call |
| `QEMU_IA64_TRACE_SUSP_CALLS_DUMP_BUNDLE` | Dump bundle on suspicious |
| `QEMU_IA64_TRACE_SUSP_CALLS_DUMP_CODE` | Dump code on suspicious |

## Usage Examples

### Basic Firmware Debugging
```bash
# Enable firmware logging and HOB dump
export QEMU_IA64_FW_LOG=1
export QEMU_IA64_EFI_HOB_DUMP=1
qemu-system-ia64 -bios Flash.fd -m 256
```

### Tracing PEI Initialization
```bash
# Trace PEI function calls with limit
export QEMU_IA64_PEI_CALL_TRACE=1
export QEMU_IA64_FW_CALL_TRACE_LIMIT=1000
qemu-system-ia64 -bios Flash.fd -m 256
```

### Memory Watchpoint
```bash
# Watch stores to specific address
export QEMU_IA64_WATCH_STORE=0x100000
export QEMU_IA64_WATCH_STORE_LIMIT=50
qemu-system-ia64 -bios Flash.fd -m 256
```

### I/O Port Tracing
```bash
# Trace PCI config space and POST codes
export QEMU_IPF_TRACE_PCI=1
export QEMU_IPF_TRACE_POST=1
export QEMU_IPF_TRACE_LIMIT=1000
qemu-system-ia64 -bios Flash.fd -m 256
```

### SAL Debugging
```bash
# Trace SAL calls with PCI detail
export QEMU_IA64_FW_SAL_TRACE=1
export QEMU_IA64_SAL_PCI_DUMP=1
qemu-system-ia64 -bios Flash.fd -m 256
```

## Notes

1. Most trace variables accept `1` to enable or an address/value for filtering
2. Variables ending in `_LIMIT` set maximum entries before tracing stops
3. Variables ending in `_ABORT` will cause QEMU to abort when condition is met
4. Variables with `_PC` suffix filter by instruction pointer (program counter)
5. Variables with `_RANGE` suffix accept `start-end` format
6. Some variables are mutually exclusive or may produce excessive output
7. Enable `-d cpu` with QEMU for additional context in logs
