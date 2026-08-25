# IA-64 relocatable PAL firmware validation

- Commit tested: `b3b8ae763a06c3b0405724f41d8514c7bec31fa0`
- Expected Flash.fd SHA-256: `e143e85874ad57bad631853d48f0d47b7e7dbe6c41b4e558bbb4ea5b45775513`
- Actual Flash.fd SHA-256: `-`
- Expected size: `10485760` bytes
- Actual size: `-` bytes

## Result matrix

| Check | Exit/status |
|---|---:|
| PAL source contract | 0 |
| Configure | 0 |
| Build | 0 |
| Firmware fetch | 1 |
| Firmware identity | 2 |
| Firmware replay | 127 |
| PAL 30 trace matches | 0 |
| PAL 256 trace matches | 0 |
| Unimplemented-operation matches | 0 |
| Serial ASSERT matches | 0 |
| Host-abort matches | 0 |
| Overall validation status | 1 |

A replay exit of 124 is the expected timeout outcome and is not itself a failure.

## Firmware provenance

```text
Trying Xen IA-64 virtual firmware source: https://xenbits.xen.org/ext/efi-vfirmware.hg
abort: HTTP Error 403: Forbidden
Trying Xen IA-64 virtual firmware source: https://xenbits.xensource.com/ext/efi-vfirmware.hg
abort: HTTP Error 403: Forbidden
Trying Xen IA-64 virtual firmware source: http://xenbits.xensource.com/ext/efi-vfirmware.hg
abort: HTTP Error 403: Forbidden
error: unable to clone the historical efi-vfirmware repository
```

## PAL and DXE highlights

```text
```

## Serial tail

```text
<serial log unavailable>
```

## QEMU log tail

```text
```
