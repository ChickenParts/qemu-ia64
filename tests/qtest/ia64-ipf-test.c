/*
 * QTest coverage for IA-64 IPF machine device topology.
 *
 * Copyright (c) 2026 ChickenParts contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"

#define IPF_IOSAPIC_BASE            UINT64_C(0xfec00000)
#define IPF_IOSAPIC_REG_SELECT      0x00
#define IPF_IOSAPIC_WINDOW          0x10
#define IPF_IOSAPIC_EOI             0x40
#define IPF_IOSAPIC_RTE_BASE        0x10
#define IPF_IOSAPIC_REMOTE_IRR      (1U << 14)
#define IPF_IOSAPIC_TRIGGER_LEVEL   (1U << 15)

#define IPF_PIIX_DEV                1
#define IPF_PIIX_PIRQB              0x61
#define IPF_PIIX4_SMBUS_IO_BASE     0xb100
#define TEST_PCI_DEV                2
#define TEST_PCI_IRQ                11
#define TEST_PM_IO_BASE             0x0400
#define TEST_E1000_MMIO_BASE        UINT64_C(0x10000000)
#define E1000_ICR                   0x00c0
#define E1000_ICS                   0x00c8
#define E1000_IMS                   0x00d0
#define E1000_IMC                   0x00d8
#define E1000_TEST_CAUSE            (1U << 0)
#define ACPI_POWER_BUTTON_STATUS    0x0100
#define ACPI_POWER_BUTTON_ENABLE    0x0100

static char *firmware_path;

static uint32_t pci_config_address(unsigned int dev, unsigned int function,
                                   unsigned int offset)
{
    return 0x80000000U | (dev << 11) | (function << 8) | (offset & 0xfc);
}

static uint32_t pci_config_readl(QTestState *qts, unsigned int dev,
                                 unsigned int function, unsigned int offset)
{
    qtest_outl(qts, 0xcf8, pci_config_address(dev, function, offset));
    return qtest_inl(qts, 0xcfc);
}

static uint8_t pci_config_readb(QTestState *qts, unsigned int dev,
                                unsigned int function, unsigned int offset)
{
    qtest_outl(qts, 0xcf8, pci_config_address(dev, function, offset));
    return qtest_inb(qts, 0xcfc + (offset & 3));
}

static void pci_config_writel(QTestState *qts, unsigned int dev,
                              unsigned int function, unsigned int offset,
                              uint32_t value)
{
    qtest_outl(qts, 0xcf8, pci_config_address(dev, function, offset));
    qtest_outl(qts, 0xcfc, value);
}

static void pci_config_writeb(QTestState *qts, unsigned int dev,
                              unsigned int function, unsigned int offset,
                              uint8_t value)
{
    qtest_outl(qts, 0xcf8, pci_config_address(dev, function, offset));
    qtest_outb(qts, 0xcfc + (offset & 3), value);
}

static QTestState *ipf_qtest_start_args(const char *args)
{
    return qtest_initf("-machine ipf,usb=on -m 64M -bios %s "
                       "-display none -nodefaults %s",
                       firmware_path, args);
}

static QTestState *ipf_qtest_start(void)
{
    return ipf_qtest_start_args("");
}

static void assert_pci_id(QTestState *qts, unsigned int function,
                          uint16_t device_id)
{
    uint32_t id = pci_config_readl(qts, IPF_PIIX_DEV, function, 0);

    g_assert_cmphex(id & 0xffff, ==, PCI_VENDOR_ID_INTEL);
    g_assert_cmphex(id >> 16, ==, device_id);
}

static void test_piix4_functions(void)
{
    QTestState *qts = ipf_qtest_start();

    assert_pci_id(qts, 0, PCI_DEVICE_ID_INTEL_82371AB_0);
    assert_pci_id(qts, 1, PCI_DEVICE_ID_INTEL_82371AB);
    assert_pci_id(qts, 2, PCI_DEVICE_ID_INTEL_82371AB_2);
    assert_pci_id(qts, 3, PCI_DEVICE_ID_INTEL_82371AB_3);

    g_assert_cmphex(pci_config_readl(qts, IPF_PIIX_DEV, 3, 0x90) & 0xffc1,
                    ==, IPF_PIIX4_SMBUS_IO_BASE | 1);

    qtest_quit(qts);
}

static void iosapic_select(QTestState *qts, uint32_t reg)
{
    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_REG_SELECT, reg);
}

static uint32_t iosapic_read_window(QTestState *qts)
{
    return qtest_readl(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_WINDOW);
}

static void iosapic_write_window(QTestState *qts, uint32_t value)
{
    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_WINDOW, value);
}

static void test_piix4_sci_reaches_iosapic(void)
{
    const unsigned int sci_pin = 9;
    const uint8_t vector = 0x50;
    const uint32_t rte_low = IPF_IOSAPIC_RTE_BASE + sci_pin * 2;
    QTestState *qts = ipf_qtest_start();
    uint32_t low;

    /* Give the PIIX4 PM function a conventional PM I/O window and enable it. */
    pci_config_writel(qts, IPF_PIIX_DEV, 3, 0x40, TEST_PM_IO_BASE | 1);
    pci_config_writeb(qts, IPF_PIIX_DEV, 3, 0x80, 1);
    g_assert_cmphex(pci_config_readl(qts, IPF_PIIX_DEV, 3, 0x40) & 0xffc1,
                    ==, TEST_PM_IO_BASE | 1);
    g_assert_cmphex(pci_config_readb(qts, IPF_PIIX_DEV, 3, 0x80) & 1, ==, 1);

    /* Program ISA IRQ9 as a level-triggered I/O SAPIC route. */
    iosapic_select(qts, rte_low + 1);
    iosapic_write_window(qts, 0);
    iosapic_select(qts, rte_low);
    iosapic_write_window(qts, vector | IPF_IOSAPIC_TRIGGER_LEVEL);

    /* Enable the power-button event, then request an ACPI powerdown. */
    qtest_outw(qts, TEST_PM_IO_BASE + 2, ACPI_POWER_BUTTON_ENABLE);
    qtest_qmp_assert_success(qts, "{'execute': 'system_powerdown'}");

    g_assert_cmphex(qtest_inw(qts, TEST_PM_IO_BASE) &
                    ACPI_POWER_BUTTON_STATUS,
                    ==, ACPI_POWER_BUTTON_STATUS);
    iosapic_select(qts, rte_low);
    low = iosapic_read_window(qts);
    g_assert_cmphex(low & IPF_IOSAPIC_REMOTE_IRR,
                    ==, IPF_IOSAPIC_REMOTE_IRR);

    /*
     * Deassert SCI before EOI; Remote IRR must then clear without
     * redelivery.
     */
    qtest_outw(qts, TEST_PM_IO_BASE, ACPI_POWER_BUTTON_STATUS);
    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_EOI, vector);
    iosapic_select(qts, rte_low);
    low = iosapic_read_window(qts);
    g_assert_cmphex(low & IPF_IOSAPIC_REMOTE_IRR, ==, 0);

    qtest_quit(qts);
}

static void test_pci_intx_reaches_iosapic(void)
{
    const unsigned int iosapic_pin = TEST_PCI_IRQ;
    const uint8_t vector = 0x51;
    const uint32_t rte_low = IPF_IOSAPIC_RTE_BASE + iosapic_pin * 2;
    QTestState *qts = ipf_qtest_start_args("-device e1000,addr=2.0");
    uint32_t id;
    uint32_t low;

    id = pci_config_readl(qts, TEST_PCI_DEV, 0, PCI_VENDOR_ID);
    g_assert_cmphex(id & 0xffff, ==, PCI_VENDOR_ID_INTEL);
    g_assert_cmphex(id >> 16, ==, 0x100e);

    /* Slot 2 INTA swizzles to PIRQ B; route it to architectural IRQ11. */
    pci_config_writeb(qts, IPF_PIIX_DEV, 0, IPF_PIIX_PIRQB, TEST_PCI_IRQ);
    g_assert_cmphex(pci_config_readb(qts, IPF_PIIX_DEV, 0,
                                    IPF_PIIX_PIRQB),
                    ==, TEST_PCI_IRQ);

    iosapic_select(qts, rte_low + 1);
    iosapic_write_window(qts, 0);
    iosapic_select(qts, rte_low);
    iosapic_write_window(qts, vector | IPF_IOSAPIC_TRIGGER_LEVEL);

    pci_config_writel(qts, TEST_PCI_DEV, 0, PCI_BASE_ADDRESS_0,
                       TEST_E1000_MMIO_BASE);
    pci_config_writel(qts, TEST_PCI_DEV, 0, PCI_COMMAND,
                       PCI_COMMAND_MEMORY);
    g_assert_cmphex(pci_config_readl(qts, TEST_PCI_DEV, 0,
                                    PCI_BASE_ADDRESS_0) &
                    PCI_BASE_ADDRESS_MEM_MASK,
                    ==, TEST_E1000_MMIO_BASE);

    /* Clear stale causes, enable one cause, and raise a real PCI INTx. */
    qtest_writel(qts, TEST_E1000_MMIO_BASE + E1000_IMC, UINT32_MAX);
    qtest_readl(qts, TEST_E1000_MMIO_BASE + E1000_ICR);
    qtest_writel(qts, TEST_E1000_MMIO_BASE + E1000_IMS, E1000_TEST_CAUSE);
    qtest_writel(qts, TEST_E1000_MMIO_BASE + E1000_ICS, E1000_TEST_CAUSE);

    iosapic_select(qts, rte_low);
    low = iosapic_read_window(qts);
    g_assert_cmphex(low & IPF_IOSAPIC_REMOTE_IRR,
                    ==, IPF_IOSAPIC_REMOTE_IRR);

    /* Reading ICR deasserts INTx; EOI must then release Remote IRR. */
    g_assert_cmphex(qtest_readl(qts, TEST_E1000_MMIO_BASE + E1000_ICR) &
                    E1000_TEST_CAUSE,
                    ==, E1000_TEST_CAUSE);
    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_EOI, vector);
    iosapic_select(qts, rte_low);
    low = iosapic_read_window(qts);
    g_assert_cmphex(low & IPF_IOSAPIC_REMOTE_IRR, ==, 0);

    qtest_quit(qts);
}

static void create_test_firmware(void)
{
    g_autoptr(GError) error = NULL;
    uint8_t image[4096];
    ssize_t written;
    int fd;

    fd = g_file_open_tmp("ia64-ipf-qtest-XXXXXX", &firmware_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);

    memset(image, 0xff, sizeof(image));
    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
    close(fd);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    create_test_firmware();

    qtest_add_func("/ia64/ipf/piix4-functions", test_piix4_functions);
    qtest_add_func("/ia64/ipf/piix4-sci-iosapic",
                   test_piix4_sci_reaches_iosapic);
    qtest_add_func("/ia64/ipf/pci-intx-iosapic",
                   test_pci_intx_reaches_iosapic);
    ret = g_test_run();

    unlink(firmware_path);
    g_free(firmware_path);
    return ret;
}
