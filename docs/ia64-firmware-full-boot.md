# Xen and development-firmware full-boot campaign

This campaign is the first target of the revived IA-64 work. It is deliberately
narrower than the later Itanium 2, IA-32 execution, platform, and performance
work: first make the firmware paths finish booting, then preserve those paths as
hard regressions.

## Target images

The repository currently carries or names the following firmware families:

- Xen IA-64 GFW (`xen-gfw`). `stuff/Flash.fd` is only a placeholder in the
  repository; point `IA64_XEN_BIOS` at a non-empty built GFW image.
- Intel SDV EFI 0.99 debug firmware.
- The unknown-version SDV EFI image.
- SDV `bios117c`, `bios130`, and `mybios` images.
- HP rx4610 109B and 117B images.

The real-hardware images may require additional chipset work after the common
CPU/firmware path is repaired. They stay in the same matrix so a CPU fix cannot
silently regress one platform while advancing another.

## First architectural repair

The current RSE implementation records a private stacked-register snapshot for
`br.call`. Firmware call gates and hand-written assembly can leave stale
snapshots above the real caller. The historical `br.ret` path blindly restored
the top entry, even when its saved return address and CFM did not correspond to
the architectural `b0` and `ar.pfs`.

The target already contained an optional correlated unwind path. Full-system
IA-64 now enables it by default: stale snapshots are unwound until the saved
return address or saved CFM matches the architectural return state, and only
then is the caller window restored.

For regression bisection only:

```sh
QEMU_IA64_LEGACY_BLIND_RET_POP=1 \
  scripts/run-ia64-firmware.sh
```

An explicit `QEMU_IA64_RET_UNWIND_PFS` setting is preserved.

## Full-boot acceptance gates

A process that merely remains alive until timeout is not considered booted.

| Target | Required gate |
|---|---|
| Xen GFW | reaches the EFI shell or boot-manager prompt with no firmware assert, IA-64 unimplemented instruction, null control transfer, or host fatal |
| SDV debug firmware | reaches its shell, setup UI, or an explicitly configured equivalent prompt |
| Production SDV firmware | reaches its boot manager or an explicitly configured equivalent prompt |
| rx4610 images | reaches a stable firmware UI/console; chipset mismatches must be classified, not hidden by timeout |

Every run retains the serial log, host log, command manifest, highest observed
phase, last classified blocker, and the exact text that satisfied the success
oracle.

## Running the matrix

Build QEMU first, build or locate Xen GFW, and then run:

```sh
IA64_XEN_BIOS=/path/to/Flash.fd \
python3 scripts/run-ia64-firmware-matrix.py \
  --timeout 180
```

The repository inventory is strict by default: an absent or zero-length image
fails before execution. During a focused subset run:

```sh
python3 scripts/run-ia64-firmware-matrix.py \
  --no-defaults \
  --firmware xen-gfw=/path/to/Flash.fd \
  --firmware sdv-debug-0.99='stuff/EFI 0.99 Debug SDV.bin' \
  --timeout 180
```

Per-firmware success oracles are supported for images whose console does not
end in an EFI shell prompt:

```sh
python3 scripts/run-ia64-firmware-matrix.py \
  --allow-missing \
  --success 'sdv-debug-0.99=YOUR_STABLE_SETUP_MARKER'
```

The current Xen frontier profile can be reproduced without baking
firmware-specific policy into the matrix driver:

```sh
IA64_XEN_BIOS=/path/to/Flash.fd \
python3 scripts/run-ia64-firmware-matrix.py \
  --allow-missing \
  --set-env IA64_PEI_22560_STATUS_FIX=1 \
  --set-env IA64_PEI_279D0_TRACE=1 \
  --set-env IA64_PEI_279D0_STATUS_FIX=1 \
  --set-env IA64_PEI_279D0_SAFE_MODE=0 \
  --timeout 180
```

Results are written below
`scratch/ia64-firmware-matrix/<UTC timestamp>/`. `summary.json` is the
machine-readable gate; `summary.md` is the compact human report.

## Next cuts after the return repair

1. Re-run Xen GFW with the correlated unwind default and pin the next terminal
   frontier.
2. Convert the newly reached failure into a directed architectural or machine
   test before changing behavior.
3. Run the same commit against every SDV/rx4610 image.
4. Continue until Xen and at least the SDV debug firmware satisfy explicit
   full-boot oracles; then remove or quarantine any firmware-address-specific
   repair that the architectural fix made unnecessary.
