#!/usr/bin/env python3
"""Apply the reviewed IA-64 460GX QOM extraction to an exact QEMU tree."""
from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def remove_between(text: str, start: str, end: str, label: str) -> str:
    first = text.find(start)
    if first < 0:
        raise SystemExit(f"{label}: start marker not found")
    last = text.find(end, first)
    if last < 0:
        raise SystemExit(f"{label}: end marker not found")
    return text[:first] + text[last:]


def update_ipf(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include "hw/ia64/gfw.h"\n#include "hw/ia64/iosapic.h"\n',
        '#include "hw/ia64/gfw.h"\n#include "hw/ia64/ipf-460gx.h"\n'
        '#include "hw/ia64/iosapic.h"\n',
        "460GX include",
    )

    text = replace_once(
        text,
        '#define IPF_PCI_FW_BUS 0xff\n'
        '#define IPF_PCI_FW_DEV_COUNT 32\n'
        '#define IPF_PCI_FW_MAX_FUNC 8\n\n'
        'typedef struct IPFPciFwConfig {\n'
        '    bool present;\n'
        '    uint8_t cfg[PCI_CONFIG_SPACE_SIZE];\n'
        '    uint8_t wmask[PCI_CONFIG_SPACE_SIZE];\n'
        '    uint8_t w1c[PCI_CONFIG_SPACE_SIZE];\n'
        '} IPFPciFwConfig;\n\n',
        '',
        "machine-local 460GX config type",
    )

    text = replace_once(
        text,
        '    MemoryRegion gx_mmio;\n'
        '    MemoryRegion gx_mmio_alias;\n'
        '    uint32_t gx_mmio_cb0;\n'
        '    uint32_t gx_mmio_cc0;\n',
        '    IA64IPF460GXState *gx;\n',
        "machine 460GX state",
    )

    text = replace_once(
        text,
        '    uint32_t pci_cfgaddr;\n'
        '    uint32_t trace_pci_cfgaddr;\n'
        '    IPFPciFwConfig pci_fw_cfg[IPF_PCI_FW_DEV_COUNT][IPF_PCI_FW_MAX_FUNC];\n',
        '    uint32_t trace_pci_cfgaddr;\n',
        "machine firmware config state",
    )

    text = replace_once(
        text,
        'static void ipf_pci_fw_cfg_set_ro(IPFPciFwConfig *cfg, uint16_t off,\n'
        '                                  unsigned size, uint64_t value);\n\n',
        '',
        "firmware config forward declaration",
    )

    text = replace_once(
        text,
        '#define IPF_PCI_FW_DEV_SAC 0\n'
        '#define IPF_PCI_FW_DEV_SDC 4\n'
        '#define IPF_PCI_FW_DEV_GXB 2\n'
        '#define IPF_PCI_FW_DEV_MAC 5 /* Memory Card A */\n'
        '#define IPF_PCI_FW_DEV_MDC 6 /* Memory Card B */\n\n'
        '#define IPF_PCI_FW_DEVICE_ID_SAC 0x84e0 /* 460GX System Address Controller */\n'
        '#define IPF_PCI_FW_DEVICE_ID_SDC 0x84e1 /* 460GX System Data Controller */\n'
        '#define IPF_PCI_FW_DEVICE_ID_GXB_FN1 0x84ea /* 460GX AGP Bridge (GXB function 1) */\n'
        '#define IPF_PCI_FW_DEVICE_ID_GXB_FN2 0x84e2 /* 460GX AGP Bridge (GXB function 2) */\n'
        '#define IPF_PCI_FW_DEVICE_ID_MAC 0x84e3 /* 460GX Memory Address Controller */\n'
        '#define IPF_PCI_FW_DEVICE_ID_MDC 0x84e4 /* 460GX Memory Data Controller */\n'
        '#define IPF_PCI_FW_DEVICE_ID_WXB 0x84e6 /* 460GX Wide PCI Expander Bridge */\n'
        '#define IPF_PCI_FW_DEVICE_ID_IHPC 0x123f /* 460GX WXB Integrated Hot-Plug Controller */\n\n',
        '',
        "machine-local 460GX identities",
    )

    text = replace_once(
        text,
        '#define IPF_GX_MMIO_BASE 0x00000000feb00000ULL\n'
        '#define IPF_GX_MMIO_SIZE 0x00001000ULL\n'
        '#define IPF_GX_MMIO_REG_CB0 0x0cb0\n'
        '#define IPF_GX_MMIO_REG_CC0 0x0cc0\n',
        '#define IPF_GX_MMIO_BASE 0x00000000feb00000ULL\n',
        "460GX MMIO constants",
    )

    text = replace_once(
        text,
        '        ipf_pci_fw_cfg_set_ro(&m->pci_fw_cfg[IPF_PCI_FW_DEV_SAC][0],\n'
        '                              0x44, 2, sac44);\n',
        '        ia64_ipf_460gx_set_sac_strap(m->gx, sac44);\n',
        "SAC CMOS strap",
    )

    text = remove_between(
        text,
        'static int ipf_pci_fw_dev_index(uint8_t dev)\n',
        'static uint64_t ipf_legacy_io_read(void *opaque, hwaddr addr, unsigned size)\n',
        "machine-local sparse config implementation",
    )

    text = replace_once(
        text,
        '    if (ipf_pci_fw_cfg_io(m, false, port, size, &val)) {\n',
        '    if (ia64_ipf_460gx_config_data(m->gx, false, port, size, &val)) {\n',
        "sparse config read dispatch",
    )
    text = replace_once(
        text,
        '    if (port == 0xcf8 && size == 4) {\n'
        '        m->pci_cfgaddr = (uint32_t)data;\n'
        '    }\n',
        '    if (port == 0xcf8 && size == 4) {\n'
        '        ia64_ipf_460gx_set_config_address(m->gx, data);\n'
        '    }\n',
        "sparse config address dispatch",
    )
    text = replace_once(
        text,
        '    if (ipf_pci_fw_cfg_io(m, true, port, size, &val32)) {\n',
        '    if (ia64_ipf_460gx_config_data(m->gx, true, port, size, &val32)) {\n',
        "sparse config write dispatch",
    )

    text = remove_between(
        text,
        'static uint32_t ipf_gx_mmio_read_reg(IPFMachineState *m, hwaddr addr)\n',
        'static void ipf_init_iosapic(IPFMachineState *m)\n',
        "machine-local 460GX MMIO implementation",
    )

    text = replace_once(
        text,
        'static void ipf_init_iosapic(IPFMachineState *m)\n',
        'static void ipf_init_460gx(IPFMachineState *m)\n'
        '{\n'
        '    DeviceState *dev = qdev_new(TYPE_IA64_IPF_460GX);\n\n'
        '    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);\n'
        '    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, IPF_GX_MMIO_BASE);\n'
        '    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1,\n'
        '                    IPF_GX_MMIO_BASE | (1ULL << 63));\n'
        '    m->gx = IA64_IPF_460GX(dev);\n\n'
        '    DPRINTF("460GX: mapped control windows at 0x%016" PRIx64\n'
        '            " and 0x%016" PRIx64 "\\n",\n'
        '            (uint64_t)IPF_GX_MMIO_BASE,\n'
        '            (uint64_t)(IPF_GX_MMIO_BASE | (1ULL << 63)));\n'
        '}\n\n'
        'static void ipf_init_iosapic(IPFMachineState *m)\n',
        "460GX realization",
    )

    text = replace_once(
        text,
        '    ipf_init_uart(m, sysmem);\n'
        '    ipf_init_debugcon(m);\n'
        '    ipf_init_pci_fw_cfg(m);\n'
        '    ipf_init_legacy_io(m, sysmem);\n'
        '    ipf_init_iosapic(m);\n'
        '    ipf_init_gx_mmio(m, sysmem);\n',
        '    ipf_init_uart(m, sysmem);\n'
        '    ipf_init_debugcon(m);\n'
        '    ipf_init_460gx(m);\n'
        '    ipf_init_legacy_io(m, sysmem);\n'
        '    ipf_init_iosapic(m);\n',
        "machine 460GX initialization",
    )

    path.write_text(text, encoding="utf-8")


def update_meson(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "ia64_ss.add(files('virt.c', 'ipf.c', 'gfw.c', 'iosapic.c', 'iosapic-core.c'))\n",
        "ia64_ss.add(files('virt.c', 'ipf.c', 'ipf-460gx.c', 'gfw.c', "
        "'iosapic.c', 'iosapic-core.c'))\n",
        "IA-64 Meson source list",
    )
    path.write_text(text, encoding="utf-8")


def update_qtest(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        '#define IPF_LEGACY_IO_BASE          UINT64_C(0xe0000000)\n'
        '#define IPF_IOSAPIC_BASE            UINT64_C(0xfec00000)\n',
        '#define IPF_LEGACY_IO_BASE          UINT64_C(0xe0000000)\n'
        '#define IPF_GX_MMIO_BASE            UINT64_C(0xfeb00000)\n'
        '#define IPF_GX_MMIO_CB0             0x0cb0\n'
        '#define IPF_GX_MMIO_CC0             0x0cc0\n'
        '#define IPF_IOSAPIC_BASE            UINT64_C(0xfec00000)\n',
        "qtest 460GX MMIO constants",
    )
    text = replace_once(
        text,
        '#define TEST_PCI_DEV                2\n',
        '#define TEST_SAC_DEV                0\n'
        '#define TEST_SAC_SECTID             0x80\n'
        '#define TEST_PCI_DEV                2\n',
        "qtest SAC constants",
    )

    reset_test = r'''static void test_460gx_reset(void)
{
    const uint32_t cc0_value = 0x12340001;
    QTestState *qts = ipf_qtest_start_args("-device e1000,addr=2.0");

    g_assert_cmphex(pci_fw_config_readb(qts, 0xff, TEST_SAC_DEV, 0,
                                       TEST_SAC_SECTID),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),
                    ==, 0x80);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),
                    ==, 0x80);

    pci_fw_config_writeb(qts, 0xff, TEST_SAC_DEV, 0,
                         TEST_SAC_SECTID, 0x80);
    g_assert_cmphex(pci_fw_config_readb(qts, 0xff, TEST_SAC_DEV, 0,
                                       TEST_SAC_SECTID),
                    ==, 0x80);

    qtest_writel(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0, 1);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),
                    ==, 0x83);
    qtest_writel(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0, cc0_value);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),
                    ==, cc0_value | 0x80);

    qtest_system_reset(qts);

    g_assert_cmphex(pci_fw_config_readb(qts, 0xff, TEST_SAC_DEV, 0,
                                       TEST_SAC_SECTID),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),
                    ==, 0x80);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),
                    ==, 0x80);
    assert_gxb_identity(qts, 0xff, 1, TEST_GXB_FN1_DEVICE_ID);
    assert_gxb_identity(qts, 0xff, 2, TEST_GXB_FN2_DEVICE_ID);

    qtest_quit(qts);
}

'''
    text = replace_once(
        text,
        'static void program_iosapic_level_route(QTestState *qts,\n',
        reset_test + 'static void program_iosapic_level_route(QTestState *qts,\n',
        "460GX reset qtest",
    )
    text = replace_once(
        text,
        '    qtest_add_func("/ia64/ipf/gxb-sparse-config",\n'
        '                   test_gxb_sparse_config_identity);\n',
        '    qtest_add_func("/ia64/ipf/gxb-sparse-config",\n'
        '                   test_gxb_sparse_config_identity);\n'
        '    qtest_add_func("/ia64/ipf/460gx-reset", test_460gx_reset);\n',
        "460GX reset qtest registration",
    )
    path.write_text(text, encoding="utf-8")


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    update_ipf(root / "hw/ia64/ipf.c")
    update_meson(root / "hw/ia64/meson.build")
    update_qtest(root / "tests/qtest/ia64-ipf-test.c")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
