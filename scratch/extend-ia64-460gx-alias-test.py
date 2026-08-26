#!/usr/bin/env python3
"""Extend the generated 460GX qtest with region-4 alias coverage."""
from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    path = root / "tests/qtest/ia64-ipf-test.c"
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#define IPF_GX_MMIO_BASE            UINT64_C(0xfeb00000)\n'
        '#define IPF_GX_MMIO_CB0             0x0cb0\n',
        '#define IPF_GX_MMIO_BASE            UINT64_C(0xfeb00000)\n'
        '#define IPF_GX_MMIO_ALIAS_BASE      UINT64_C(0x80000000feb00000)\n'
        '#define IPF_GX_MMIO_CB0             0x0cb0\n',
        "region-4 alias constant",
    )
    text = replace_once(
        text,
        '    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),\n'
        '                    ==, 0x80);\n'
        '    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),\n'
        '                    ==, 0x80);\n\n'
        '    pci_fw_config_writeb',
        '    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),\n'
        '                    ==, 0x80);\n'
        '    g_assert_cmphex(qtest_readl(qts,\n'
        '                               IPF_GX_MMIO_ALIAS_BASE + IPF_GX_MMIO_CB0),\n'
        '                    ==, 0x80);\n'
        '    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),\n'
        '                    ==, 0x80);\n\n'
        '    pci_fw_config_writeb',
        "initial alias read",
    )
    text = replace_once(
        text,
        '    qtest_writel(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0, 1);\n'
        '    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),\n',
        '    qtest_writel(qts, IPF_GX_MMIO_ALIAS_BASE + IPF_GX_MMIO_CB0, 1);\n'
        '    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),\n',
        "alias write to primary read",
    )
    text = replace_once(
        text,
        '    qtest_writel(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0, cc0_value);\n'
        '    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),\n'
        '                    ==, cc0_value | 0x80);\n',
        '    qtest_writel(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0, cc0_value);\n'
        '    g_assert_cmphex(qtest_readl(qts,\n'
        '                               IPF_GX_MMIO_ALIAS_BASE + IPF_GX_MMIO_CC0),\n'
        '                    ==, cc0_value | 0x80);\n',
        "primary write to alias read",
    )
    text = replace_once(
        text,
        '    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),\n'
        '                    ==, 0x80);\n'
        '    assert_gxb_identity',
        '    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),\n'
        '                    ==, 0x80);\n'
        '    g_assert_cmphex(qtest_readl(qts,\n'
        '                               IPF_GX_MMIO_ALIAS_BASE + IPF_GX_MMIO_CC0),\n'
        '                    ==, 0x80);\n'
        '    assert_gxb_identity',
        "post-reset alias default",
    )

    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
