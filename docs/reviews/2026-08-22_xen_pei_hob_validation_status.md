# Xen/IPF PEI HOB relocation validation

Workflow run: 32562277661
Trigger commit: e8291235cf5064eac6d9a4f86e726e767e7021d5
Result: **failed**
Last phase: `fetch-firmware`

```text
result=failed
failed_phase=fetch-firmware
shell_rc=1
runner_rc=not-run
firmware_sha256=missing
serial_bytes=0
qlog_bytes=0
sysmem_insertions=0
dxe_core_entries=0
memory_service_entries=0
dxe_load_asserts=0
gcd_asserts=0
total_asserts=0
host_aborts=0
```

The acceptance gate is intentionally limited to the relocation
contract: Xen must pass `DxeLoad.c:536`, enter DXE memory-service
initialization, and avoid the prior `Gcd.c:1736` failure. A later
firmware assertion is retained as the next investigation frontier.
