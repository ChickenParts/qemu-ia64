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
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define IPF_LEGACY_IO_BASE          UINT64_C(0xe0000000)
#define IPF_GX_MMIO_BASE            UINT64_C(0xfeb00000)
#define IPF_GX_MMIO_ALIAS_BASE      UINT64_C(0x80000000feb00000)
#define IPF_GX_MMIO_CB0             0x0cb0
#define IPF_GX_MMIO_CC0             0x0cc0
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
#define TEST_SAC_DEV                0
#define TEST_SAC_SECTID             0x80
#define TEST_PCI_DEV                2
#define TEST_PCI_IRQ                11
#define TEST_GXB_FN1_DEVICE_ID      0x84ea
#define TEST_GXB_FN2_DEVICE_ID      0x84e2
#define TEST_GXB_REVISION           0x04
#define TEST_I8042_KBD_IRQ           1
#define TEST_I8042_MOUSE_IRQ         12
#define TEST_I8042_DATA_PORT         0x60
#define TEST_I8042_STATUS_PORT       0x64
#define TEST_I8042_WRITE_MODE        0x60
#define TEST_I8042_SELF_TEST         0xaa
#define TEST_I8042_WRITE_OBUF        0xd2
#define TEST_I8042_WRITE_AUX_OBUF    0xd3
#define TEST_I8042_SELF_TEST_OK      0x55
#define TEST_I8042_MODE_KBD_IRQ      0x01
#define TEST_I8042_MODE_MOUSE_IRQ    0x02
#define TEST_I8042_STATUS_AUX_OBF    0x20
#define TEST_DMA_CHANNEL2_ADDRESS    0x04
#define TEST_DMA_MASK_READ           0x09
#define TEST_DMA_CLEAR_FLIP_FLOP     0x0c
#define TEST_DMA_WRITE_ALL_MASK      0x0f
#define TEST_DMA_CHANNEL2_PAGE       0x81
#define TEST_FDC_BASE                0x3f0
#define TEST_FDC_DOR                 (TEST_FDC_BASE + 2)
#define TEST_FDC_MSR                 (TEST_FDC_BASE + 4)
#define TEST_FDC_FIFO                (TEST_FDC_BASE + 5)
#define TEST_FDC_IRQ                 6
#define TEST_FDC_DOR_NRESET          0x04
#define TEST_FDC_DOR_DMA_IRQ_ENABLE  0x08
#define TEST_FDC_MSR_RQM             0x80
#define TEST_FDC_MSR_DIO             0x40
#define TEST_FDC_CMD_SENSE_INTERRUPT 0x08
#define TEST_CMOS_INDEX_PORT         0x70
#define TEST_CMOS_DATA_PORT          0x71
#define TEST_CMOS_FLOPPY_TYPE        0x10
#define TEST_CMOS_EQUIPMENT          0x14
#define TEST_PARALLEL_BASE          0x378
#define TEST_PARALLEL_DATA          (TEST_PARALLEL_BASE + 0)
#define TEST_PARALLEL_STATUS        (TEST_PARALLEL_BASE + 1)
#define TEST_PARALLEL_CONTROL       (TEST_PARALLEL_BASE + 2)
#define TEST_PARALLEL_IRQ           7
#define TEST_PARALLEL_STATUS_RESET  0xd9
#define TEST_PARALLEL_CONTROL_RESET 0xcc
#define TEST_PARALLEL_CONTROL_FIXED 0xc0
#define TEST_PARALLEL_CONTROL_INTEN 0x10
#define TEST_PARALLEL_CONTROL_SELECT 0x08
#define TEST_PARALLEL_CONTROL_INIT  0x04
#define TEST_PARALLEL_CONTROL_STROBE 0x01
#define TEST_PM_IO_BASE             0x0400
#define TEST_PM_TIMER_OFFSET        0x08
#define TEST_PM_TIMER_MASK          0x00ffffffU
#define TEST_E1000_MMIO_BASE        UINT64_C(0x10000000)
#define E1000_ICR                   0x00c0
#define E1000_ICS                   0x00c8
#define E1000_IMS                   0x00d0
#define E1000_IMC                   0x00d8
#define E1000_TEST_CAUSE            (1U << 0)
#define ACPI_POWER_BUTTON_STATUS    0x0100
#define ACPI_POWER_BUTTON_ENABLE    0x0100

static char *firmware_path;

static uint32_t pci_config_address_bus(unsigned int bus, unsigned int dev,
                                       unsigned int function,
                                       unsigned int offset)
{
    return 0x80000000U | (bus << 16) | (dev << 11) | (function << 8) |
           (offset & 0xfc);
}

static uint32_t pci_config_address(unsigned int dev, unsigned int function,
                                   unsigned int offset)
{
    return pci_config_address_bus(0, dev, function, offset);
}

static uint64_t legacy_io_address(unsigned int port)
{
    return IPF_LEGACY_IO_BASE + ((uint64_t)(port >> 2) << 12) + (port & 3);
}

static uint32_t pci_fw_config_readl(QTestState *qts, unsigned int bus,
                                    unsigned int dev, unsigned int function,
                                    unsigned int offset)
{
    qtest_writel(qts, legacy_io_address(0xcf8),
                 pci_config_address_bus(bus, dev, function, offset));
    return qtest_readl(qts, legacy_io_address(0xcfc));
}

static uint8_t pci_fw_config_readb(QTestState *qts, unsigned int bus,
                                   unsigned int dev, unsigned int function,
                                   unsigned int offset)
{
    qtest_writel(qts, legacy_io_address(0xcf8),
                 pci_config_address_bus(bus, dev, function, offset));
    return qtest_readb(qts, legacy_io_address(0xcfc + (offset & 3)));
}

static void pci_fw_config_writel(QTestState *qts, unsigned int bus,
                                 unsigned int dev, unsigned int function,
                                 unsigned int offset, uint32_t value)
{
    qtest_writel(qts, legacy_io_address(0xcf8),
                 pci_config_address_bus(bus, dev, function, offset));
    qtest_writel(qts, legacy_io_address(0xcfc), value);
}

static void pci_fw_config_writeb(QTestState *qts, unsigned int bus,
                                 unsigned int dev, unsigned int function,
                                 unsigned int offset, uint8_t value)
{
    qtest_writel(qts, legacy_io_address(0xcf8),
                 pci_config_address_bus(bus, dev, function, offset));
    qtest_writeb(qts, legacy_io_address(0xcfc + (offset & 3)), value);
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

static void test_qmp_target(void)
{
    QTestState *qts = ipf_qtest_start();
    QDict *response;
    QDict *target;
    QList *cpus;

    response = qtest_qmp(qts, "{'execute': 'query-target'}");
    g_assert_nonnull(response);
    g_assert_false(qdict_haskey(response, "error"));
    target = qdict_get_qdict(response, "return");
    g_assert_nonnull(target);
    g_assert_cmpstr(qdict_get_try_str(target, "arch"), ==, "ia64");
    qobject_unref(response);

    /* This used to abort in qapi_enum_parse() before producing a response. */
    response = qtest_qmp(qts, "{'execute': 'query-cpus-fast'}");
    g_assert_nonnull(response);
    g_assert_false(qdict_haskey(response, "error"));
    cpus = qdict_get_qlist(response, "return");
    g_assert_nonnull(cpus);
    g_assert_cmpuint(qlist_size(cpus), ==, 1);
    qobject_unref(response);

    qtest_quit(qts);
}

static void assert_pci_id(QTestState *qts, unsigned int function,
                          uint16_t device_id)
{
    uint32_t id = pci_config_readl(qts, IPF_PIIX_DEV, function, 0);

    g_assert_cmphex(id & 0xffff, ==, PCI_VENDOR_ID_INTEL);
    g_assert_cmphex(id >> 16, ==, device_id);
}

static void assert_piix4_pm_defaults(QTestState *qts)
{
    g_assert_cmphex(pci_config_readl(qts, IPF_PIIX_DEV, 3, 0x40) & 0xffc1,
                    ==, TEST_PM_IO_BASE | 1);
    g_assert_cmphex(pci_config_readb(qts, IPF_PIIX_DEV, 3, 0x80) & 1,
                    ==, 1);
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
    assert_piix4_pm_defaults(qts);

    uint32_t before = qtest_inl(qts, TEST_PM_IO_BASE +
                               TEST_PM_TIMER_OFFSET) &
                      TEST_PM_TIMER_MASK;
    qtest_clock_step(qts, 10 * 1000 * 1000);
    uint32_t after = qtest_inl(qts, TEST_PM_IO_BASE +
                              TEST_PM_TIMER_OFFSET) &
                     TEST_PM_TIMER_MASK;
    g_assert_cmpuint((after - before) & TEST_PM_TIMER_MASK, >, 0);

    qtest_quit(qts);
}

static uint16_t dma_channel2_address(QTestState *qts)
{
    uint16_t value;

    qtest_outb(qts, TEST_DMA_CLEAR_FLIP_FLOP, 0);
    value = qtest_inb(qts, TEST_DMA_CHANNEL2_ADDRESS);
    value |= (uint16_t)qtest_inb(qts, TEST_DMA_CHANNEL2_ADDRESS) << 8;
    return value;
}

static void test_isa_dma_contract(void)
{
    const uint16_t address = 0x1234;
    const uint8_t page = 0x5a;
    QTestState *qts = ipf_qtest_start();

    /* PIIX supplies the primary i8257 and its standard channel-2 ports. */
    g_assert_cmphex(qtest_inb(qts, TEST_DMA_MASK_READ) & 0x0f, ==, 0x0f);

    qtest_outb(qts, TEST_DMA_CLEAR_FLIP_FLOP, 0);
    qtest_outb(qts, TEST_DMA_CHANNEL2_ADDRESS, address & 0xff);
    qtest_outb(qts, TEST_DMA_CHANNEL2_ADDRESS, address >> 8);
    qtest_outb(qts, TEST_DMA_CHANNEL2_PAGE, page);

    g_assert_cmphex(dma_channel2_address(qts), ==, address);
    g_assert_cmphex(qtest_inb(qts, TEST_DMA_CHANNEL2_PAGE), ==, page);

    qtest_outb(qts, TEST_DMA_WRITE_ALL_MASK, 0);
    g_assert_cmphex(qtest_inb(qts, TEST_DMA_MASK_READ) & 0x0f, ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_inb(qts, TEST_DMA_MASK_READ) & 0x0f, ==, 0x0f);

    qtest_quit(qts);
}

static void test_piix4_pm_reset(void)
{
    const uint32_t alternate_base = 0x0800;
    QTestState *qts = ipf_qtest_start();

    assert_piix4_pm_defaults(qts);
    pci_config_writel(qts, IPF_PIIX_DEV, 3, 0x40,
                       alternate_base | 1);
    pci_config_writeb(qts, IPF_PIIX_DEV, 3, 0x80, 0);
    g_assert_cmphex(pci_config_readl(qts, IPF_PIIX_DEV, 3, 0x40) &
                    0xffc1, ==, alternate_base | 1);
    g_assert_cmphex(pci_config_readb(qts, IPF_PIIX_DEV, 3, 0x80) &
                    1, ==, 0);

    qtest_system_reset(qts);
    assert_piix4_pm_defaults(qts);

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

    assert_piix4_pm_defaults(qts);

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

static void assert_gxb_identity(QTestState *qts, unsigned int bus,
                                unsigned int function, uint16_t device_id)
{
    uint32_t id = pci_fw_config_readl(qts, bus, TEST_PCI_DEV, function,
                                      PCI_VENDOR_ID);
    uint32_t class_revision = pci_fw_config_readl(qts, bus, TEST_PCI_DEV,
                                                  function,
                                                  PCI_REVISION_ID);

    g_assert_cmphex(id & 0xffff, ==, PCI_VENDOR_ID_INTEL);
    g_assert_cmphex(id >> 16, ==, device_id);
    g_assert_cmphex(class_revision, ==,
                    (PCI_CLASS_BRIDGE_HOST << 16) | TEST_GXB_REVISION);
    g_assert_cmphex(pci_fw_config_readb(qts, bus, TEST_PCI_DEV, function,
                                       PCI_HEADER_TYPE),
                    ==, 0);
}

static void test_gxb_sparse_config_identity(void)
{
    QTestState *qts = ipf_qtest_start_args("-device e1000,addr=2.0");
    uint32_t real_id;

    /* Function 0 remains a real QOM PCI device and is forwarded unchanged. */
    real_id = pci_config_readl(qts, TEST_PCI_DEV, 0, PCI_VENDOR_ID);
    g_assert_cmphex(real_id & 0xffff, ==, PCI_VENDOR_ID_INTEL);
    g_assert_cmphex(real_id >> 16, ==, 0x100e);
    g_assert_cmphex(pci_fw_config_readl(qts, 0, TEST_PCI_DEV, 0,
                                       PCI_VENDOR_ID),
                    ==, real_id);

    assert_gxb_identity(qts, 0, 1, TEST_GXB_FN1_DEVICE_ID);
    assert_gxb_identity(qts, 0, 2, TEST_GXB_FN2_DEVICE_ID);
    assert_gxb_identity(qts, 0xff, 1, TEST_GXB_FN1_DEVICE_ID);
    assert_gxb_identity(qts, 0xff, 2, TEST_GXB_FN2_DEVICE_ID);

    /* The façade must not invent ordinary root-bus QOM functions. */
    g_assert_cmphex(pci_config_readl(qts, TEST_PCI_DEV, 1, PCI_VENDOR_ID),
                    ==, UINT32_MAX);
    g_assert_cmphex(pci_config_readl(qts, TEST_PCI_DEV, 2, PCI_VENDOR_ID),
                    ==, UINT32_MAX);
    g_assert_cmphex(pci_fw_config_readl(qts, 0xff, TEST_PCI_DEV, 3,
                                       PCI_VENDOR_ID),
                    ==, UINT32_MAX);

    /* Identity, class, revision, and header type are immutable. */
    pci_fw_config_writel(qts, 0, TEST_PCI_DEV, 1, PCI_VENDOR_ID, 0);
    pci_fw_config_writel(qts, 0, TEST_PCI_DEV, 1, PCI_REVISION_ID,
                         UINT32_MAX);
    pci_fw_config_writeb(qts, 0, TEST_PCI_DEV, 1, PCI_HEADER_TYPE, 0xff);
    assert_gxb_identity(qts, 0, 1, TEST_GXB_FN1_DEVICE_ID);

    qtest_quit(qts);
}

static void test_460gx_reset(void)
{
    const uint32_t cc0_value = 0x12340001;
    QTestState *qts = ipf_qtest_start_args("-device e1000,addr=2.0");

    g_assert_cmphex(pci_fw_config_readb(qts, 0xff, TEST_SAC_DEV, 0,
                                       TEST_SAC_SECTID),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),
                    ==, 0x80);
    g_assert_cmphex(qtest_readl(qts,
                               IPF_GX_MMIO_ALIAS_BASE + IPF_GX_MMIO_CB0),
                    ==, 0x80);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),
                    ==, 0x80);

    pci_fw_config_writeb(qts, 0xff, TEST_SAC_DEV, 0,
                         TEST_SAC_SECTID, 0x80);
    g_assert_cmphex(pci_fw_config_readb(qts, 0xff, TEST_SAC_DEV, 0,
                                       TEST_SAC_SECTID),
                    ==, 0x80);

    qtest_writel(qts, IPF_GX_MMIO_ALIAS_BASE + IPF_GX_MMIO_CB0, 1);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),
                    ==, 0x83);
    qtest_writel(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0, cc0_value);
    g_assert_cmphex(qtest_readl(qts,
                               IPF_GX_MMIO_ALIAS_BASE + IPF_GX_MMIO_CC0),
                    ==, cc0_value | 0x80);

    qtest_system_reset(qts);

    g_assert_cmphex(pci_fw_config_readb(qts, 0xff, TEST_SAC_DEV, 0,
                                       TEST_SAC_SECTID),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CB0),
                    ==, 0x80);
    g_assert_cmphex(qtest_readl(qts, IPF_GX_MMIO_BASE + IPF_GX_MMIO_CC0),
                    ==, 0x80);
    g_assert_cmphex(qtest_readl(qts,
                               IPF_GX_MMIO_ALIAS_BASE + IPF_GX_MMIO_CC0),
                    ==, 0x80);
    assert_gxb_identity(qts, 0xff, 1, TEST_GXB_FN1_DEVICE_ID);
    assert_gxb_identity(qts, 0xff, 2, TEST_GXB_FN2_DEVICE_ID);

    qtest_quit(qts);
}

static void program_iosapic_level_route(QTestState *qts,
                                         unsigned int pin, uint8_t vector)
{
    const uint32_t rte_low = IPF_IOSAPIC_RTE_BASE + pin * 2;

    iosapic_select(qts, rte_low + 1);
    iosapic_write_window(qts, 0);
    iosapic_select(qts, rte_low);
    iosapic_write_window(qts, vector | IPF_IOSAPIC_TRIGGER_LEVEL);
}

static void assert_iosapic_remote_irr(QTestState *qts, unsigned int pin,
                                      bool asserted)
{
    const uint32_t rte_low = IPF_IOSAPIC_RTE_BASE + pin * 2;
    uint32_t low;

    iosapic_select(qts, rte_low);
    low = iosapic_read_window(qts);
    g_assert_cmphex(low & IPF_IOSAPIC_REMOTE_IRR, ==,
                    asserted ? IPF_IOSAPIC_REMOTE_IRR : 0);
}

static uint8_t cmos_read(QTestState *qts, uint8_t index)
{
    qtest_outb(qts, TEST_CMOS_INDEX_PORT, index);
    return qtest_inb(qts, TEST_CMOS_DATA_PORT);
}

static void test_fdc_dma_irq_and_cmos(void)
{
    const uint8_t vector = 0x54;
    const uint8_t normal_dor = TEST_FDC_DOR_NRESET |
                               TEST_FDC_DOR_DMA_IRQ_ENABLE;
    QTestState *qts = ipf_qtest_start_args(
        "-drive if=floppy,file=null-co://,file.read-zeroes=on,"
        "format=raw,size=1440k");
    uint8_t msr;
    uint8_t status;
    uint8_t track;

    /* A 1.44MB drive is visible through both the controller and CMOS. */
    g_assert_cmphex(cmos_read(qts, TEST_CMOS_FLOPPY_TYPE), ==, 0x40);
    g_assert_cmphex(cmos_read(qts, TEST_CMOS_EQUIPMENT) & 0x47, ==, 0x07);

    g_assert_cmphex(qtest_inb(qts, TEST_FDC_DOR) & normal_dor, ==,
                    normal_dor);
    msr = qtest_inb(qts, TEST_FDC_MSR);
    g_assert_cmphex(msr & (TEST_FDC_MSR_RQM | TEST_FDC_MSR_DIO), ==,
                    TEST_FDC_MSR_RQM);

    program_iosapic_level_route(qts, TEST_FDC_IRQ, vector);

    /* Leaving and re-entering reset raises the standard IRQ6 indication. */
    qtest_outb(qts, TEST_FDC_DOR, 0);
    qtest_outb(qts, TEST_FDC_DOR, normal_dor);
    assert_iosapic_remote_irr(qts, TEST_FDC_IRQ, true);

    qtest_outb(qts, TEST_FDC_FIFO, TEST_FDC_CMD_SENSE_INTERRUPT);
    msr = qtest_inb(qts, TEST_FDC_MSR);
    g_assert_cmphex(msr & (TEST_FDC_MSR_RQM | TEST_FDC_MSR_DIO), ==,
                    TEST_FDC_MSR_RQM | TEST_FDC_MSR_DIO);
    status = qtest_inb(qts, TEST_FDC_FIFO);
    track = qtest_inb(qts, TEST_FDC_FIFO);
    g_assert_cmphex(status & 0xc0, ==, 0xc0);
    g_assert_cmphex(track, ==, 0);

    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_EOI, vector);
    assert_iosapic_remote_irr(qts, TEST_FDC_IRQ, false);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_inb(qts, TEST_FDC_DOR) & normal_dor, ==,
                    normal_dor);
    g_assert_cmphex(qtest_inb(qts, TEST_FDC_MSR) & TEST_FDC_MSR_RQM, ==,
                    TEST_FDC_MSR_RQM);

    qtest_quit(qts);
}

static void test_parallel_irq_and_reset(void)
{
    const uint8_t vector = 0x55;
    const uint8_t armed_control = TEST_PARALLEL_CONTROL_FIXED |
                                  TEST_PARALLEL_CONTROL_INTEN |
                                  TEST_PARALLEL_CONTROL_SELECT |
                                  TEST_PARALLEL_CONTROL_INIT |
                                  TEST_PARALLEL_CONTROL_STROBE;
    const uint8_t interrupt_control = armed_control &
                                      ~TEST_PARALLEL_CONTROL_STROBE;
    QTestState *qts = ipf_qtest_start_args("-parallel null");

    g_assert_cmphex(qtest_readb(qts,
                                legacy_io_address(TEST_PARALLEL_DATA)),
                    ==, 0xff);
    g_assert_cmphex(qtest_readb(qts,
                                legacy_io_address(TEST_PARALLEL_STATUS)),
                    ==, TEST_PARALLEL_STATUS_RESET);
    g_assert_cmphex(qtest_readb(qts,
                                legacy_io_address(TEST_PARALLEL_CONTROL)),
                    ==, TEST_PARALLEL_CONTROL_RESET);

    qtest_writeb(qts, legacy_io_address(TEST_PARALLEL_DATA), 0x5a);
    g_assert_cmphex(qtest_readb(qts,
                                legacy_io_address(TEST_PARALLEL_DATA)),
                    ==, 0x5a);

    program_iosapic_level_route(qts, TEST_PARALLEL_IRQ, vector);

    /* A strobe transition writes the byte; dropping strobe raises ACK IRQ7. */
    qtest_writeb(qts, legacy_io_address(TEST_PARALLEL_CONTROL),
                 armed_control);
    qtest_writeb(qts, legacy_io_address(TEST_PARALLEL_CONTROL),
                 interrupt_control);
    assert_iosapic_remote_irr(qts, TEST_PARALLEL_IRQ, true);

    /* Reading status acknowledges the device before the I/O SAPIC EOI. */
    qtest_readb(qts, legacy_io_address(TEST_PARALLEL_STATUS));
    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_EOI, vector);
    assert_iosapic_remote_irr(qts, TEST_PARALLEL_IRQ, false);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts,
                                legacy_io_address(TEST_PARALLEL_DATA)),
                    ==, 0xff);
    g_assert_cmphex(qtest_readb(qts,
                                legacy_io_address(TEST_PARALLEL_STATUS)),
                    ==, TEST_PARALLEL_STATUS_RESET);
    g_assert_cmphex(qtest_readb(qts,
                                legacy_io_address(TEST_PARALLEL_CONTROL)),
                    ==, TEST_PARALLEL_CONTROL_RESET);

    qtest_quit(qts);
}

static void test_i8042_irqs_reach_iosapic(void)
{
    const uint8_t kbd_vector = 0x52;
    const uint8_t mouse_vector = 0x53;
    const uint8_t kbd_data = 0x5a;
    const uint8_t mouse_data = 0xa5;
    QTestState *qts = ipf_qtest_start();

    /* Exercise the actual controller registers before testing its IRQ lines. */
    qtest_outb(qts, TEST_I8042_STATUS_PORT, TEST_I8042_SELF_TEST);
    g_assert_cmphex(qtest_inb(qts, TEST_I8042_DATA_PORT), ==,
                    TEST_I8042_SELF_TEST_OK);

    program_iosapic_level_route(qts, TEST_I8042_KBD_IRQ, kbd_vector);
    program_iosapic_level_route(qts, TEST_I8042_MOUSE_IRQ, mouse_vector);

    /* Enable both outputs and inject one byte through each controller path. */
    qtest_outb(qts, TEST_I8042_STATUS_PORT, TEST_I8042_WRITE_MODE);
    qtest_outb(qts, TEST_I8042_DATA_PORT,
               TEST_I8042_MODE_KBD_IRQ | TEST_I8042_MODE_MOUSE_IRQ);

    qtest_outb(qts, TEST_I8042_STATUS_PORT, TEST_I8042_WRITE_OBUF);
    qtest_outb(qts, TEST_I8042_DATA_PORT, kbd_data);
    assert_iosapic_remote_irr(qts, TEST_I8042_KBD_IRQ, true);
    g_assert_cmphex(qtest_inb(qts, TEST_I8042_DATA_PORT), ==, kbd_data);
    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_EOI, kbd_vector);
    assert_iosapic_remote_irr(qts, TEST_I8042_KBD_IRQ, false);

    qtest_outb(qts, TEST_I8042_STATUS_PORT, TEST_I8042_WRITE_AUX_OBUF);
    qtest_outb(qts, TEST_I8042_DATA_PORT, mouse_data);
    g_assert_cmphex(qtest_inb(qts, TEST_I8042_STATUS_PORT) &
                    TEST_I8042_STATUS_AUX_OBF,
                    ==, TEST_I8042_STATUS_AUX_OBF);
    assert_iosapic_remote_irr(qts, TEST_I8042_MOUSE_IRQ, true);
    g_assert_cmphex(qtest_inb(qts, TEST_I8042_DATA_PORT), ==, mouse_data);
    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_EOI, mouse_vector);
    assert_iosapic_remote_irr(qts, TEST_I8042_MOUSE_IRQ, false);

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

    qtest_add_func("/ia64/ipf/qmp-target", test_qmp_target);
    qtest_add_func("/ia64/ipf/piix4-functions", test_piix4_functions);
    qtest_add_func("/ia64/ipf/isa-dma", test_isa_dma_contract);
    qtest_add_func("/ia64/ipf/piix4-pm-reset",
                   test_piix4_pm_reset);
    qtest_add_func("/ia64/ipf/piix4-sci-iosapic",
                   test_piix4_sci_reaches_iosapic);
    qtest_add_func("/ia64/ipf/pci-intx-iosapic",
                   test_pci_intx_reaches_iosapic);
    qtest_add_func("/ia64/ipf/gxb-sparse-config",
                   test_gxb_sparse_config_identity);
    qtest_add_func("/ia64/ipf/460gx-reset", test_460gx_reset);
    qtest_add_func("/ia64/ipf/fdc-dma-iosapic-cmos",
                   test_fdc_dma_irq_and_cmos);
    qtest_add_func("/ia64/ipf/parallel-iosapic-reset",
                   test_parallel_irq_and_reset);
    qtest_add_func("/ia64/ipf/i8042-iosapic",
                   test_i8042_irqs_reach_iosapic);
    ret = g_test_run();

    unlink(firmware_path);
    g_free(firmware_path);
    return ret;
}
