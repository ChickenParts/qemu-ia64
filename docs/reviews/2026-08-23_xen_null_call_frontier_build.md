# Xen/IPF post-GCD emulator export

Date: 2026-08-23
Source trigger: `655b57a0a92c474736facb269046781d57408d08`
Source tree: `64a3fa2898e306b8d323000fd4a9ae883d11ab76`
Workflow run: `32682815668`
Artifact: `ia64-xen-current-emulator-32682815668`
qemu-system-ia64 SHA-256: `e75ff90f738b662df8737ef3c2c2d6a06ba01951bd46591e0ee466d2db55ba7e`

The artifact is a replayable IA-64 system emulator built from the
exact diagnostic branch rooted at the current `metachicken` head.
It intentionally contains no Xen firmware image. Runtime replay uses
the separately retained canonical `Flash.fd` with SHA-256
`e143e85874ad57bad631853d48f0d47b7e7dbe6c41b4e558bbb4ea5b45775513`.
