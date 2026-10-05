#!/usr/bin/env python3
"""One-use hash-locked local patch transport; removed by its publishing commit."""
import base64
import hashlib
import lzma
from pathlib import Path
import subprocess

BASE = {'.github/workflows/ia64-f9-validation.yml': '2682b78f1be20c8a3fa1ea545e6e0ae2ba9840bcdcf1ba978df394d07e22c825', 'docs/generated/ia64-instruction-coverage.md': '0d7fe7f704aff9d5eadd9ca7e03bc25e4ac403ae518a798ced7b54d6f48d74cf', 'scripts/ia64-isa-coverage.py': 'e362a61fb64615afb0db5ed996dad948ea09f3fefa892dcb42b49469a8cd25de', 'scripts/run-ia64-f10-tests.py': None, 'scripts/run-ia64-f9-tests.py': '1749f833b3b1690dcd8b79473a27d85d8e12501cc066b54df716f6fb0d8cedf8', 'target/ia64/fp-convert.h': None, 'target/ia64/helper.c': 'f199f226396d5ea59bebb82fdd17c94d0d5c8d5fd80818ca0aaea31b6f662347', 'target/ia64/helper.h': 'e381336f3312d0c3de35ebe8e0440a5f359fca7a8d362d497643f42cae033eda', 'target/ia64/translate.c': '700bf559d5d3ef69ba0f5b2fec8940cf6e96c3600f442584452629a3dc94bc1f', 'tests/ia64/isa/README.md': 'fb71c5b9eee7c81688ad2f32c8d2ccdad33c9cdcbfdbb2095851e4b04c9893d1', 'tests/ia64/isa/coverage.json5': '48e8ccf5c3e1d42089651751ed6635a00326442cc89d0955fe38ef0f034ad214', 'tests/ia64/isa/f-unit-baseline.json5': '57b2f21735ff9c2873ecec88e37b6dcfde9e1f08c2902281726476e26b6d2a9d', 'tests/ia64/isa/test_coverage.py': 'fbbd64a5d3daf9f9aaba51430e143bd194301a2bf30d2f1af923c16121902e27', 'tests/ia64/isa/test_f10.py': None}
EXPECTED = {'.github/workflows/ia64-f9-validation.yml': 'c3b56baf54cb4b540304e77a00bdade153d453f9a649c10dc6763e2e8a5d9c28', 'docs/generated/ia64-instruction-coverage.md': '984c6f2d29c3f957b38643ae0e772b10cb2124f6d2bdf6483e9a7c2e1e23792e', 'scripts/ia64-isa-coverage.py': '59d42d8a514f2c788caf3e1fb3ecde78b7fffac0322422f3d53cb96ed1d3112a', 'scripts/run-ia64-f10-tests.py': 'dbd03941afb2c541dbbb542eafd5f56792a3178f9e5e4e2b3e78a30d9c0ba767', 'scripts/run-ia64-f9-tests.py': '22422174cc60181e8de064059c8bde9fd02d00e183b6af9853c81d26a55ad7b4', 'target/ia64/fp-convert.h': 'fc5247979d942548368757118ba3add910af283f1aafdd0ca2c25954df00cf66', 'target/ia64/helper.c': 'ce6b7c5101a74c312e70ff06aeb0d47f789621ab2e629e302f1a924198604a19', 'target/ia64/helper.h': 'c222c51869ccdf9fb7000dea951c203efbaec8e2da890b9fa3d63fc7bdc4928a', 'target/ia64/translate.c': 'b8db79fef4cdc8f9c6b62271feade0eeec50c3543f3e3fdf6fa8ad84a009a8a3', 'tests/ia64/isa/README.md': 'ddfef28ca0529a84c2137b6e8f6b62d6a5c6f4327ffee9522c8d182f57b7de9b', 'tests/ia64/isa/coverage.json5': '78b4df4fa4ba0670572560a5a2a8883bed01241c4bb190ce8a885b12152e7849', 'tests/ia64/isa/f-unit-baseline.json5': '23e6c282d5dcb1b220f2ee6ba1013ff2a3efa7ce0ed6cc58c170eef1fc6adb16', 'tests/ia64/isa/test_coverage.py': '321582f39794c6d22581c47cf69cd0f98980f2e014ccf1dd2f024e3668a9384f', 'tests/ia64/isa/test_f10.py': 'dd19de63ada844f04116ed9c53074c1733b6411aec4cc293b9fd15b20a2969cd'}
PATCH_SHA256 = '79f67a234fc1df18f19df1a3ebe0569b4fe55f2022bdae54e4f4d87f084769d3'


def main():
    root = Path(__file__).resolve().parents[1]
    for name, expected in BASE.items():
        path = root / name
        observed = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
        if observed != expected:
            raise SystemExit('base mismatch: ' + name)
    text = ''.join((root / f'scripts/f10-stage/patch.{i:02d}').read_text().strip()
                   for i in range(5))
    patch = lzma.decompress(base64.b64decode(text, validate=True), memlimit=128 << 20)
    if hashlib.sha256(patch).hexdigest() != PATCH_SHA256:
        raise SystemExit('patch checksum mismatch')
    subprocess.run(['git', 'apply', '--check', '-'], input=patch, cwd=root, check=True)
    subprocess.run(['git', 'apply', '-'], input=patch, cwd=root, check=True)
    for name, expected in EXPECTED.items():
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
            raise SystemExit('candidate mismatch: ' + name)
    print(f'Exact reviewed F10 candidate verified: {len(EXPECTED)} files; patch {PATCH_SHA256}')


if __name__ == '__main__':
    main()
