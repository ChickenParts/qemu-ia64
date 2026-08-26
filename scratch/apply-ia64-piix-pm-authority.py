#!/usr/bin/env python3
"""Make PIIX4 the sole IA-64 ACPI PM register authority."""
from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def replace_span(text: str, start: str, end: str, new: str,
                 label: str) -> str:
    if text.count(start) != 1:
        raise SystemExit(
            f"{label}: expected one start marker, found {text.count(start)}"
        )
    first = text.index(start)
    last = text.find(end, first + len(start))
    if last < 0:
        raise SystemExit(f"{label}: end marker not found")
    return text[:first] + new + text[last:]


def update_piix4_header(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "    MemoryRegion io;\n"
        "    uint32_t io_base;\n\n"
        "    MemoryRegion io_gpe;\n",
        "    MemoryRegion io;\n"
        "    uint32_t io_base;\n"
        "    uint32_t reset_io_base;\n"
        "    bool reset_io_enabled;\n\n"
        "    MemoryRegion io_gpe;\n",
        "PIIX4 PM reset defaults",
    )
    path.write_text(text, encoding="utf-8")


def update_piix_header(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = replace_once(
        text,
        "    uint32_t smb_io_base;\n\n"
        "    /* Reset Control Register contents */\n",
        "    uint32_t smb_io_base;\n"
        "    uint32_t pm_io_base;\n"
        "    bool pm_io_enabled;\n\n"
        "    /* Reset Control Register contents */\n",
        "PIIX parent PM defaults",
    )
    path.write_text(text, encoding="utf-8")


def update_piix4_source(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")

    helper = '''static void piix4_pm_apply_io_defaults(PIIX4PMState *s)
{
    PCIDevice *d = PCI_DEVICE(s);

    /*
     * Machine wiring defaults are separate from the guest-migrated PCI
     * configuration.  Migration restores the latter and post-load remaps it.
     */
    pci_set_long(d->config + 0x40, s->reset_io_base | 1);
    d->config[0x80] = s->reset_io_enabled;
}

'''
    text = replace_once(
        text,
        "static void smbus_io_space_update(PIIX4PMState *s)\n",
        helper + "static void smbus_io_space_update(PIIX4PMState *s)\n",
        "PIIX4 PM default helper",
    )

    text = replace_once(
        text,
        "    pci_conf[0x40] = 0x01; /* PM io base read only bit */\n"
        "    pci_conf[0x80] = 0;\n",
        "    piix4_pm_apply_io_defaults(s);\n",
        "PIIX4 PM reset configuration",
    )

    text = replace_once(
        text,
        "    pci_conf[0x09] = 0x00;\n"
        "    pci_conf[0x3d] = 0x01; // interrupt pin 1\n\n"
        "    /* APM */\n",
        "    pci_conf[0x09] = 0x00;\n"
        "    pci_conf[0x3d] = 0x01; // interrupt pin 1\n\n"
        "    if (s->reset_io_base & ~0xffc0U) {\n"
        "        error_setg(errp,\n"
        "                   \"PIIX4 PM I/O base 0x%x must be 64-byte \"\n"
        "                   \"aligned within 16-bit I/O space\",\n"
        "                   s->reset_io_base);\n"
        "        return;\n"
        "    }\n"
        "    piix4_pm_apply_io_defaults(s);\n\n"
        "    /* APM */\n",
        "PIIX4 PM realization validation",
    )

    text = replace_once(
        text,
        "    acpi_pm1_cnt_init(&s->ar, &s->io, s->disable_s3, s->disable_s4, s->s4_val,\n"
        "                      !s->smm_compat && !s->smm_enabled);\n"
        "    acpi_gpe_init(&s->ar, GPE_LEN);\n",
        "    acpi_pm1_cnt_init(&s->ar, &s->io, s->disable_s3, s->disable_s4, s->s4_val,\n"
        "                      !s->smm_compat && !s->smm_enabled);\n"
        "    pm_io_space_update(s);\n"
        "    acpi_gpe_init(&s->ar, GPE_LEN);\n",
        "PIIX4 PM initial I/O mapping",
    )

    text = replace_once(
        text,
        "static const Property piix4_pm_properties[] = {\n"
        "    DEFINE_PROP_UINT32(\"smb_io_base\", PIIX4PMState, smb_io_base, 0),\n",
        "static const Property piix4_pm_properties[] = {\n"
        "    DEFINE_PROP_UINT32(\"reset-io-base\", PIIX4PMState,\n"
        "                       reset_io_base, 0),\n"
        "    DEFINE_PROP_BOOL(\"reset-io-enabled\", PIIX4PMState,\n"
        "                     reset_io_enabled, false),\n"
        "    DEFINE_PROP_UINT32(\"smb_io_base\", PIIX4PMState, smb_io_base, 0),\n",
        "PIIX4 PM reset properties",
    )

    path.write_text(text, encoding="utf-8")


def update_piix_source(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "        qdev_prop_set_uint32(DEVICE(&d->pm), \"smb_io_base\", d->smb_io_base);\n"
        "        qdev_prop_set_bit(DEVICE(&d->pm), \"smm-enabled\", d->smm_enabled);\n",
        "        qdev_prop_set_uint32(DEVICE(&d->pm), \"smb_io_base\", d->smb_io_base);\n"
        "        qdev_prop_set_uint32(DEVICE(&d->pm), \"reset-io-base\",\n"
        "                             d->pm_io_base);\n"
        "        qdev_prop_set_bit(DEVICE(&d->pm), \"reset-io-enabled\",\n"
        "                          d->pm_io_enabled);\n"
        "        qdev_prop_set_bit(DEVICE(&d->pm), \"smm-enabled\", d->smm_enabled);\n",
        "PIIX PM child wiring",
    )

    text = replace_once(
        text,
        "static const Property pci_piix_props[] = {\n"
        "    DEFINE_PROP_UINT32(\"smb_io_base\", PIIXState, smb_io_base, 0),\n",
        "static const Property pci_piix_props[] = {\n"
        "    DEFINE_PROP_UINT32(\"smb_io_base\", PIIXState, smb_io_base, 0),\n"
        "    DEFINE_PROP_UINT32(\"pm_io_base\", PIIXState, pm_io_base, 0),\n"
        "    DEFINE_PROP_BOOL(\"pm_io_enabled\", PIIXState, pm_io_enabled,\n"
        "                     false),\n",
        "PIIX parent PM properties",
    )

    path.write_text(text, encoding="utf-8")


def update_ipf_source(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "    MemoryRegion legacy_io_mmio;\n"
        "    MemoryRegion legacy_io_mmio_hi;\n"
        "    MemoryRegion acpi_pm_mmio;\n"
        "    uint16_t acpi_pm1_evt_sts;\n"
        "    uint16_t acpi_pm1_evt_en;\n"
        "    uint16_t acpi_pm1_cnt;\n"
        "    uint64_t acpi_pm_timer_start_ns;\n"
        "    IA64IOSAPICState *iosapic;\n",
        "    MemoryRegion legacy_io_mmio;\n"
        "    MemoryRegion legacy_io_mmio_hi;\n"
        "    IA64IOSAPICState *iosapic;\n",
        "machine-local ACPI PM state",
    )

    text = replace_span(
        text,
        "/* ACPI PM1/PMTMR register block (I/O port encoded to segment 0xff). */\n",
        "/*\n * Legacy port-I/O space window.\n",
        "",
        "private ACPI PM base",
    )

    text = replace_once(
        text,
        "#define IPF_PIIX4_SMBUS_IO_BASE 0xb100\n",
        "#define IPF_PIIX4_PM_IO_BASE 0x0400\n"
        "#define IPF_PIIX4_SMBUS_IO_BASE 0xb100\n",
        "IA-64 PIIX4 PM base",
    )

    text = replace_once(
        text,
        "    qdev_prop_set_uint32(DEVICE(piix), \"smb_io_base\",\n"
        "                         IPF_PIIX4_SMBUS_IO_BASE);\n\n"
        "    for (i = 0; i < ISA_NUM_IRQS; i++) {\n",
        "    qdev_prop_set_uint32(DEVICE(piix), \"smb_io_base\",\n"
        "                         IPF_PIIX4_SMBUS_IO_BASE);\n"
        "    qdev_prop_set_uint32(DEVICE(piix), \"pm_io_base\",\n"
        "                         IPF_PIIX4_PM_IO_BASE);\n"
        "    qdev_prop_set_bit(DEVICE(piix), \"pm_io_enabled\", true);\n\n"
        "    for (i = 0; i < ISA_NUM_IRQS; i++) {\n",
        "IA-64 PIIX4 PM wiring",
    )

    text = replace_span(
        text,
        "static uint64_t ipf_acpi_pm_read(void *opaque, hwaddr addr, unsigned size)\n",
        "static void ipf_init_460gx(IPFMachineState *m)\n",
        "",
        "private ACPI PM implementation",
    )

    text = replace_span(
        text,
        "    /*\n"
        "     * Provide the ACPI PM1/PMTMR register block for both firmware and direct\n",
        "    /* Optional firmware load if provided. */\n",
        "",
        "private ACPI PM realization",
    )

    fadt = '''            /*
             * PIIX4 owns the PM1 event/control registers, PM timer, and SCI.
             * Advertise that one I/O window through both legacy fields and
             * Generic Address Structures.
             */
            fadt.pm1a_event_block =
                cpu_to_le32(IPF_PIIX4_PM_IO_BASE);
            fadt.pm1_event_length = 4;
            fadt.pm1a_control_block =
                cpu_to_le32(IPF_PIIX4_PM_IO_BASE + 0x04);
            fadt.pm1_control_length = 2;
            fadt.pm_timer_block =
                cpu_to_le32(IPF_PIIX4_PM_IO_BASE + 0x08);
            fadt.pm_timer_length = 4;

            /* Extended (GAS) equivalents use System I/O space. */
            fadt.xpm1a_event_block.space_id = 1;
            fadt.xpm1a_event_block.bit_width = 32;
            fadt.xpm1a_event_block.access_width = 0;
            fadt.xpm1a_event_block.address =
                cpu_to_le64(IPF_PIIX4_PM_IO_BASE);
            fadt.xpm1a_control_block.space_id = 1;
            fadt.xpm1a_control_block.bit_width = 16;
            fadt.xpm1a_control_block.access_width = 0;
            fadt.xpm1a_control_block.address =
                cpu_to_le64(IPF_PIIX4_PM_IO_BASE + 0x04);
            fadt.xpm_timer_block.space_id = 1;
            fadt.xpm_timer_block.bit_width = 32;
            fadt.xpm_timer_block.access_width = 0;
            fadt.xpm_timer_block.address =
                cpu_to_le64(IPF_PIIX4_PM_IO_BASE + 0x08);
'''
    text = replace_span(
        text,
        "            /*\n"
        "             * Fixed-feature register blocks.\n",
        "            fadt.header.checksum =",
        fadt,
        "direct-kernel FADT PM authority",
    )

    path.write_text(text, encoding="utf-8")


def update_qtest(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "#define TEST_PM_IO_BASE             0x0400\n",
        "#define TEST_PM_IO_BASE             0x0400\n"
        "#define TEST_PM_TIMER_OFFSET        0x08\n"
        "#define TEST_PM_TIMER_MASK          0x00ffffffU\n",
        "PIIX4 PM qtest constants",
    )

    helper = '''static void assert_piix4_pm_defaults(QTestState *qts)
{
    g_assert_cmphex(pci_config_readl(qts, IPF_PIIX_DEV, 3, 0x40) & 0xffc1,
                    ==, TEST_PM_IO_BASE | 1);
    g_assert_cmphex(pci_config_readb(qts, IPF_PIIX_DEV, 3, 0x80) & 1,
                    ==, 1);
}

'''
    text = replace_once(
        text,
        "static void test_piix4_functions(void)\n",
        helper + "static void test_piix4_functions(void)\n",
        "PIIX4 PM default assertion helper",
    )

    text = replace_once(
        text,
        "    g_assert_cmphex(pci_config_readl(qts, IPF_PIIX_DEV, 3, 0x90) & 0xffc1,\n"
        "                    ==, IPF_PIIX4_SMBUS_IO_BASE | 1);\n\n"
        "    qtest_quit(qts);\n"
        "}\n\n"
        "static void iosapic_select",
        "    g_assert_cmphex(pci_config_readl(qts, IPF_PIIX_DEV, 3, 0x90) & 0xffc1,\n"
        "                    ==, IPF_PIIX4_SMBUS_IO_BASE | 1);\n"
        "    assert_piix4_pm_defaults(qts);\n\n"
        "    uint32_t before = qtest_inl(qts, TEST_PM_IO_BASE +\n"
        "                               TEST_PM_TIMER_OFFSET) &\n"
        "                      TEST_PM_TIMER_MASK;\n"
        "    qtest_clock_step(qts, 10 * 1000 * 1000);\n"
        "    uint32_t after = qtest_inl(qts, TEST_PM_IO_BASE +\n"
        "                              TEST_PM_TIMER_OFFSET) &\n"
        "                     TEST_PM_TIMER_MASK;\n"
        "    g_assert_cmpuint((after - before) & TEST_PM_TIMER_MASK, >, 0);\n\n"
        "    qtest_quit(qts);\n"
        "}\n\n"
        "static void test_piix4_pm_reset(void)\n"
        "{\n"
        "    const uint32_t alternate_base = 0x0800;\n"
        "    QTestState *qts = ipf_qtest_start();\n\n"
        "    assert_piix4_pm_defaults(qts);\n"
        "    pci_config_writel(qts, IPF_PIIX_DEV, 3, 0x40,\n"
        "                       alternate_base | 1);\n"
        "    pci_config_writeb(qts, IPF_PIIX_DEV, 3, 0x80, 0);\n"
        "    g_assert_cmphex(pci_config_readl(qts, IPF_PIIX_DEV, 3, 0x40) &\n"
        "                    0xffc1, ==, alternate_base | 1);\n"
        "    g_assert_cmphex(pci_config_readb(qts, IPF_PIIX_DEV, 3, 0x80) &\n"
        "                    1, ==, 0);\n\n"
        "    qtest_system_reset(qts);\n"
        "    assert_piix4_pm_defaults(qts);\n\n"
        "    qtest_quit(qts);\n"
        "}\n\n"
        "static void iosapic_select",
        "PIIX4 PM timer and reset contracts",
    )

    text = replace_span(
        text,
        "    /* Give the PIIX4 PM function a conventional PM I/O window and enable it. */\n",
        "    /* Program ISA IRQ9 as a level-triggered I/O SAPIC route. */\n",
        "    assert_piix4_pm_defaults(qts);\n\n",
        "PIIX4 SCI default PM window",
    )

    text = replace_once(
        text,
        "    qtest_add_func(\"/ia64/ipf/piix4-functions\", test_piix4_functions);\n"
        "    qtest_add_func(\"/ia64/ipf/piix4-sci-iosapic\",\n",
        "    qtest_add_func(\"/ia64/ipf/piix4-functions\", test_piix4_functions);\n"
        "    qtest_add_func(\"/ia64/ipf/piix4-pm-reset\",\n"
        "                   test_piix4_pm_reset);\n"
        "    qtest_add_func(\"/ia64/ipf/piix4-sci-iosapic\",\n",
        "PIIX4 PM reset qtest registration",
    )

    path.write_text(text, encoding="utf-8")


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    update_piix4_header(root / "include/hw/acpi/piix4.h")
    update_piix_header(root / "include/hw/southbridge/piix.h")
    update_piix4_source(root / "hw/acpi/piix4.c")
    update_piix_source(root / "hw/isa/piix.c")
    update_ipf_source(root / "hw/ia64/ipf.c")
    update_qtest(root / "tests/qtest/ia64-ipf-test.c")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
