# IA-64 non-local return shadow-stack reconciliation

Date: 2026-08-24
Integration target: `metachicken`

## Failure

Xen/IPF reaches DXE after the PEI/HOB fixes, but its IA-64 context-restore
routine performs a non-local `br.ret`: guest `loadrs`/`flushrs` restores the
architectural register stack and returns directly to an older caller. QEMU's
auxiliary RSE shadow stack previously popped only its newest `br.call` snapshot,
so the skipped frame was restored as the caller. The saved module GP in `r37`
therefore became `0x30a`, and DXE later attempted an indirect call through a
null method descriptor.

## Repair

`ret_restore` now searches downward only for an **exact saved return address**
matching `b0`. When the newest frame does not match but a deeper frame does,
QEMU discards the intervening shadow snapshots before performing the normal
final pop. Ordinary returns are unchanged, recursion chooses the nearest exact
match, and no CFM/PFS inference is used on the default path.

The older `QEMU_IA64_RET_UNWIND_PFS` heuristic remains available only as an
explicit diagnostic fallback when no exact-target unwind occurred.

## Runtime evidence

A/B testing with the canonical Xen/IPF `Flash.fd` showed one bypassed shadow
frame at each context restore. Exact-target reconciliation restored
`r37=0x2015e820`, eliminated the null indirect call at `0x1ff4f520`, initialized
NVRAM, dispatched the runtime variable-service driver, and advanced to the next
independent boundary: `PAL_COPY_INFO` (PAL procedure 30) during Metronome-driver
dispatch.
