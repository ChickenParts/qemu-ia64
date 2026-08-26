/*
 * QTest coverage for IA-64 IPF machine device topology.
 *
 * Copyright (c) 2026 ChickenParts contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "elf.h"

#define IPF_LEGACY_IO_BASE          UINT64_C(0xe0000000)
#define IPF_LEGACY_IO_SIZE          (64ULL * 1024 * 1024)
#define IPF_UART_BASE               UINT64_C(0xff5e0000)
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
#define TEST_SERIAL_COM1_BASE       0x3f8
#define TEST_SERIAL_COM2_BASE       0x2f8
#define TEST_SERIAL_COM1_IRQ        4
#define TEST_SERIAL_COM2_IRQ        3
#define TEST_SERIAL_IER             1
#define TEST_SERIAL_IIR             2
#define TEST_SERIAL_SCR             7
#define TEST_SERIAL_IER_THRI        0x02
#define TEST_SERIAL_IIR_ID          0x0e
#define TEST_SERIAL_IIR_THRI        0x02
#define TEST_PM_IO_BASE             0x0400
#define TEST_PM_TIMER_OFFSET        0x08
#define TEST_PM_TIMER_MASK          0x00ffffffU
#define TEST_STORAGE_SIZE           "16M"
#define TEST_E1000_MMIO_BASE        UINT64_C(0x10000000)
#define E1000_ICR                   0x00c0
#define E1000_ICS                   0x00c8
#define E1000_IMS                   0x00d0
#define E1000_IMC                   0x00d8
#define E1000_TEST_CAUSE            (1U << 0)
#define ACPI_POWER_BUTTON_STATUS    0x0100
#define ACPI_POWER_BUTTON_ENABLE    0x0100

#define IPF_BOOT_PARAM_ADDR         UINT64_C(0x8000)
#define IPF_BOOT_PARAM_MEMMAP       0x10
#define IPF_BOOT_PARAM_MEMMAP_SIZE  0x18
#define IPF_BOOT_PARAM_DESC_SIZE    0x20
#define TEST_EFI_IO_PORT_TYPE       12
#define TEST_EFI_MEMORY_UC          UINT64_C(1)
#define TEST_EFI_DESC_SIZE          40
#define TEST_EFI_DESC_PHYS          8
#define TEST_EFI_DESC_PAGES         24
#define TEST_EFI_DESC_ATTR          32

#define TEST_KERNEL_FILE_SIZE       0x5200
#define TEST_KERNEL_SEGMENT_OFFSET  0x1000
#define TEST_KERNEL_SEGMENT_SIZE    0x3000
#define TEST_KERNEL_LOAD_PA         UINT64_C(0x100000)
#define TEST_KERNEL_LOAD_VA         UINT64_C(0xa000000100000000)
#define TEST_KERNEL_IO_SPACE_PA     (TEST_KERNEL_LOAD_PA + 0x1000)
#define TEST_KERNEL_IO_SPACE_VA     (TEST_KERNEL_LOAD_VA + 0x1000)
#define TEST_KERNEL_IO_SLOT_PA      (TEST_KERNEL_IO_SPACE_PA + 0xff * 16)
#define TEST_KERNEL_IO_SLOT_OFFSET  \
    (TEST_KERNEL_SEGMENT_OFFSET + TEST_KERNEL_IO_SLOT_PA - TEST_KERNEL_LOAD_PA)
#define TEST_KERNEL_SYMTAB_OFFSET   0x4000
#define TEST_KERNEL_STRTAB_OFFSET   0x4040
#define TEST_KERNEL_SHSTRTAB_OFFSET 0x4060
#define TEST_KERNEL_SHOFF           0x5000
#define TEST_KERNEL_SENTINEL        0xa5
#define TEST_KERNEL_SENTINEL_SIZE   16

#define TEST_FIRMWARE_SIZE          (4 * 1024 * 1024)
#define TEST_FIRMWARE_BASE          UINT64_C(0xffc00000)
#define TEST_FIRMWARE_GP_TARGET     UINT64_C(0xffe30070)
#define TEST_FIRMWARE_GP_SENTINEL   UINT64_C(0xdeadbeef2badbeef)
#define TEST_FIRMWARE_STATUS_CALLER UINT64_C(0xffe00076)
#define TEST_FIRMWARE_STATUS_REPORT UINT64_C(0xffe011b6)
#define TEST_FIRMWARE_FIT_OFFSET    0x10000
#define TEST_FIRMWARE_FIT_SIZE      64
#define TEST_FIRMWARE_FV_OFFSET     0x30000
#define TEST_FIRMWARE_FV_SIZE       0x1000
#define TEST_FIRMWARE_FV_HEADER_LEN 0x48
#define TEST_FIRMWARE_PEI_FILE_SIZE 0x44
#define TEST_FIRMWARE_TE_SECTION_SIZE 0x2c
#define TEST_FIT_UNUSED_TYPE        0x7f

typedef struct QEMU_PACKED TestElf64Ehdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} TestElf64Ehdr;

typedef struct QEMU_PACKED TestElf64Phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} TestElf64Phdr;

typedef struct QEMU_PACKED TestElf64Shdr {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
} TestElf64Shdr;

typedef struct QEMU_PACKED TestElf64Sym {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
} TestElf64Sym;

G_STATIC_ASSERT(sizeof(TestElf64Ehdr) == 64);
G_STATIC_ASSERT(sizeof(TestElf64Phdr) == 56);
G_STATIC_ASSERT(sizeof(TestElf64Shdr) == 64);
G_STATIC_ASSERT(sizeof(TestElf64Sym) == 24);

static char *firmware_path;
static char *kernel_path;

static void build_test_firmware_fit(
    uint8_t fit[TEST_FIRMWARE_FIT_SIZE])
{
    memset(fit, 0, TEST_FIRMWARE_FIT_SIZE);
    memcpy(fit, "_FIT_   ", 8);
    fit[8] = TEST_FIRMWARE_FIT_SIZE / 16;
    for (unsigned int entry = 1;
         entry < TEST_FIRMWARE_FIT_SIZE / 16; entry++) {
        fit[entry * 16 + 14] = TEST_FIT_UNUSED_TYPE;
    }
}

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

static QTestState *ipf_qtest_start_kernel(void)
{
    return qtest_initf("-machine ipf,usb=on -m 64M -bios %s "
                       "-kernel %s -display none -nodefaults",
                       firmware_path, kernel_path);
}

static void test_direct_kernel_io_contract(void)
{
    QTestState *qts = ipf_qtest_start_kernel();
    uint64_t memmap = qtest_readq(qts, IPF_BOOT_PARAM_ADDR +
                                  IPF_BOOT_PARAM_MEMMAP);
    uint64_t memmap_size = qtest_readq(qts, IPF_BOOT_PARAM_ADDR +
                                       IPF_BOOT_PARAM_MEMMAP_SIZE);
    uint64_t desc_size = qtest_readq(qts, IPF_BOOT_PARAM_ADDR +
                                     IPF_BOOT_PARAM_DESC_SIZE);
    uint8_t sentinel[TEST_KERNEL_SENTINEL_SIZE];
    bool found = false;

    g_assert_cmpuint(desc_size, ==, TEST_EFI_DESC_SIZE);
    g_assert_cmpuint(memmap_size % desc_size, ==, 0);

    for (uint64_t offset = 0; offset < memmap_size; offset += desc_size) {
        uint64_t desc = memmap + offset;

        if (qtest_readl(qts, desc) != TEST_EFI_IO_PORT_TYPE) {
            continue;
        }
        g_assert_false(found);
        found = true;
        g_assert_cmphex(qtest_readq(qts, desc + TEST_EFI_DESC_PHYS), ==,
                        IPF_LEGACY_IO_BASE);
        g_assert_cmpuint(qtest_readq(qts, desc + TEST_EFI_DESC_PAGES), ==,
                         IPF_LEGACY_IO_SIZE / 4096);
        g_assert_cmphex(qtest_readq(qts, desc + TEST_EFI_DESC_ATTR), ==,
                        TEST_EFI_MEMORY_UC);
    }
    g_assert_true(found);

    /*
     * The test ELF deliberately exports an io_space symbol and fills the
     * former machine-patched slot with a sentinel.  Boot data must describe
     * hardware; machine initialization must not edit loaded kernel objects.
     */
    qtest_memread(qts, TEST_KERNEL_IO_SLOT_PA, sentinel, sizeof(sentinel));
    for (size_t i = 0; i < sizeof(sentinel); i++) {
        g_assert_cmphex(sentinel[i], ==, TEST_KERNEL_SENTINEL);
    }

    qtest_quit(qts);
}

static unsigned int count_pci_devices(QTestState *qts, uint16_t vendor_id,
                                      uint16_t device_id)
{
    unsigned int count = 0;

    for (unsigned int dev = 0; dev < 32; dev++) {
        uint32_t id = pci_config_readl(qts, dev, 0, PCI_VENDOR_ID);

        if ((id & 0xffff) == vendor_id && (id >> 16) == device_id) {
            count++;
            g_assert_cmphex(pci_config_readb(qts, dev, 0,
                                            PCI_INTERRUPT_PIN),
                            ==, 1);
        }
    }
    return count;
}

static void test_pci_storage_interfaces(void)
{
    QTestState *qts = ipf_qtest_start_args(
        "-drive if=scsi,bus=1,unit=0,file=null-co://,"
        "file.read-zeroes=on,format=raw,size=" TEST_STORAGE_SIZE " "
        "-drive if=virtio,file=null-co://,file.read-zeroes=on,"
        "format=raw,size=" TEST_STORAGE_SIZE);

    /* Sparse bus 1 still requires controllers for legacy SCSI buses 0 and 1. */
    g_assert_cmpuint(count_pci_devices(qts, PCI_VENDOR_ID_LSI_LOGIC,
                                      PCI_DEVICE_ID_LSI_53C895A),
                     ==, 2);

    /* if=virtio must resolve to this PCI machine's standard transport. */
    g_assert_cmpuint(count_pci_devices(qts, PCI_VENDOR_ID_REDHAT_QUMRANET,
                                      PCI_DEVICE_ID_VIRTIO_BLOCK),
                     ==, 1);

    qtest_quit(qts);
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

static void test_serial_irqs_aliases_and_reset(void)
{
    const uint8_t com1_vector = 0x56;
    const uint8_t com2_vector = 0x57;
    QTestState *qts = ipf_qtest_start_args("-serial null -serial null");
    uint8_t iir;

    /*
     * The platform MMIO UART and COM1 are two address views of the same
     * SerialState rather than independent devices.
     */
    qtest_writeb(qts, IPF_UART_BASE + TEST_SERIAL_SCR, 0x5a);
    g_assert_cmphex(qtest_readb(
                        qts,
                        legacy_io_address(TEST_SERIAL_COM1_BASE +
                                          TEST_SERIAL_SCR)),
                    ==, 0x5a);
    qtest_writeb(qts,
                 legacy_io_address(TEST_SERIAL_COM1_BASE + TEST_SERIAL_SCR),
                 0xa5);
    g_assert_cmphex(qtest_readb(qts, IPF_UART_BASE + TEST_SERIAL_SCR),
                    ==, 0xa5);

    program_iosapic_level_route(qts, TEST_SERIAL_COM1_IRQ, com1_vector);
    qtest_writeb(qts, IPF_UART_BASE + TEST_SERIAL_IER,
                 TEST_SERIAL_IER_THRI);
    assert_iosapic_remote_irr(qts, TEST_SERIAL_COM1_IRQ, true);
    iir = qtest_readb(qts, IPF_UART_BASE + TEST_SERIAL_IIR);
    g_assert_cmphex(iir & TEST_SERIAL_IIR_ID, ==, TEST_SERIAL_IIR_THRI);
    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_EOI, com1_vector);
    assert_iosapic_remote_irr(qts, TEST_SERIAL_COM1_IRQ, false);

    /* The second configured backend is the current ISA COM2 device. */
    qtest_writeb(qts,
                 legacy_io_address(TEST_SERIAL_COM2_BASE + TEST_SERIAL_SCR),
                 0x3c);
    g_assert_cmphex(qtest_readb(
                        qts,
                        legacy_io_address(TEST_SERIAL_COM2_BASE +
                                          TEST_SERIAL_SCR)),
                    ==, 0x3c);
    program_iosapic_level_route(qts, TEST_SERIAL_COM2_IRQ, com2_vector);
    qtest_writeb(qts,
                 legacy_io_address(TEST_SERIAL_COM2_BASE + TEST_SERIAL_IER),
                 TEST_SERIAL_IER_THRI);
    assert_iosapic_remote_irr(qts, TEST_SERIAL_COM2_IRQ, true);
    iir = qtest_readb(qts,
                      legacy_io_address(TEST_SERIAL_COM2_BASE +
                                        TEST_SERIAL_IIR));
    g_assert_cmphex(iir & TEST_SERIAL_IIR_ID, ==, TEST_SERIAL_IIR_THRI);
    qtest_writel(qts, IPF_IOSAPIC_BASE + IPF_IOSAPIC_EOI, com2_vector);
    assert_iosapic_remote_irr(qts, TEST_SERIAL_COM2_IRQ, false);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readb(qts, IPF_UART_BASE + TEST_SERIAL_SCR),
                    ==, 0);
    g_assert_cmphex(qtest_readb(
                        qts,
                        legacy_io_address(TEST_SERIAL_COM2_BASE +
                                          TEST_SERIAL_SCR)),
                    ==, 0);

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

static void test_firmware_executable_state_is_unpatched(void)
{
    uint8_t expected_fit[TEST_FIRMWARE_FIT_SIZE];
    uint8_t actual_fit[TEST_FIRMWARE_FIT_SIZE];
    QTestState *qts = ipf_qtest_start();

    g_assert_cmphex(qtest_readq(qts, TEST_FIRMWARE_GP_TARGET),
                    ==, TEST_FIRMWARE_GP_SENTINEL);
    g_assert_cmphex(qtest_readq(qts, TEST_FIRMWARE_STATUS_CALLER),
                    ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(qts, TEST_FIRMWARE_STATUS_REPORT),
                    ==, UINT64_MAX);

    build_test_firmware_fit(expected_fit);
    qtest_memread(qts,
                  TEST_FIRMWARE_BASE + TEST_FIRMWARE_FIT_OFFSET,
                  actual_fit, sizeof(actual_fit));
    g_assert_cmpmem(actual_fit, sizeof(actual_fit),
                    expected_fit, sizeof(expected_fit));

    qtest_quit(qts);
}

static void create_test_kernel(void)
{
    static const char strtab[] = "\0io_space";
    static const char shstrtab[] =
        "\0.data\0.symtab\0.strtab\0.shstrtab";
    g_autofree uint8_t *image = g_malloc0(TEST_KERNEL_FILE_SIZE);
    g_autoptr(GError) error = NULL;
    TestElf64Ehdr *ehdr = (TestElf64Ehdr *)image;
    TestElf64Phdr *phdr = (TestElf64Phdr *)(image + sizeof(*ehdr));
    TestElf64Sym *symbols =
        (TestElf64Sym *)(image + TEST_KERNEL_SYMTAB_OFFSET);
    TestElf64Shdr *sections =
        (TestElf64Shdr *)(image + TEST_KERNEL_SHOFF);
    ssize_t written;
    int fd;

    memcpy(ehdr->ident, ELFMAG, SELFMAG);
    ehdr->ident[EI_CLASS] = ELFCLASS64;
    ehdr->ident[EI_DATA] = ELFDATA2LSB;
    ehdr->ident[EI_VERSION] = EV_CURRENT;
    ehdr->type = cpu_to_le16(ET_EXEC);
    ehdr->machine = cpu_to_le16(EM_IA_64);
    ehdr->version = cpu_to_le32(EV_CURRENT);
    ehdr->entry = cpu_to_le64(TEST_KERNEL_LOAD_PA);
    ehdr->phoff = cpu_to_le64(sizeof(*ehdr));
    ehdr->shoff = cpu_to_le64(TEST_KERNEL_SHOFF);
    ehdr->ehsize = cpu_to_le16(sizeof(*ehdr));
    ehdr->phentsize = cpu_to_le16(sizeof(*phdr));
    ehdr->phnum = cpu_to_le16(1);
    ehdr->shentsize = cpu_to_le16(sizeof(*sections));
    ehdr->shnum = cpu_to_le16(5);
    ehdr->shstrndx = cpu_to_le16(4);

    phdr->type = cpu_to_le32(PT_LOAD);
    phdr->flags = cpu_to_le32(PF_R | PF_W | PF_X);
    phdr->offset = cpu_to_le64(TEST_KERNEL_SEGMENT_OFFSET);
    phdr->vaddr = cpu_to_le64(TEST_KERNEL_LOAD_VA);
    phdr->paddr = cpu_to_le64(TEST_KERNEL_LOAD_PA);
    phdr->filesz = cpu_to_le64(TEST_KERNEL_SEGMENT_SIZE);
    phdr->memsz = cpu_to_le64(TEST_KERNEL_SEGMENT_SIZE);
    phdr->align = cpu_to_le64(0x1000);

    memset(image + TEST_KERNEL_IO_SLOT_OFFSET, TEST_KERNEL_SENTINEL,
           TEST_KERNEL_SENTINEL_SIZE);

    symbols[1].name = cpu_to_le32(1);
    symbols[1].info = ELF_ST_INFO(STB_GLOBAL, STT_OBJECT);
    symbols[1].shndx = cpu_to_le16(1);
    symbols[1].value = cpu_to_le64(TEST_KERNEL_IO_SPACE_VA);
    symbols[1].size = cpu_to_le64(0x1000);

    memcpy(image + TEST_KERNEL_STRTAB_OFFSET, strtab, sizeof(strtab));
    memcpy(image + TEST_KERNEL_SHSTRTAB_OFFSET, shstrtab, sizeof(shstrtab));

    sections[1].name = cpu_to_le32(1);
    sections[1].type = cpu_to_le32(SHT_PROGBITS);
    sections[1].flags = cpu_to_le64(SHF_ALLOC | SHF_WRITE);
    sections[1].addr = cpu_to_le64(TEST_KERNEL_LOAD_VA);
    sections[1].offset = cpu_to_le64(TEST_KERNEL_SEGMENT_OFFSET);
    sections[1].size = cpu_to_le64(TEST_KERNEL_SEGMENT_SIZE);
    sections[1].addralign = cpu_to_le64(0x1000);

    sections[2].name = cpu_to_le32(7);
    sections[2].type = cpu_to_le32(SHT_SYMTAB);
    sections[2].offset = cpu_to_le64(TEST_KERNEL_SYMTAB_OFFSET);
    sections[2].size = cpu_to_le64(2 * sizeof(*symbols));
    sections[2].link = cpu_to_le32(3);
    sections[2].info = cpu_to_le32(1);
    sections[2].addralign = cpu_to_le64(8);
    sections[2].entsize = cpu_to_le64(sizeof(*symbols));

    sections[3].name = cpu_to_le32(15);
    sections[3].type = cpu_to_le32(SHT_STRTAB);
    sections[3].offset = cpu_to_le64(TEST_KERNEL_STRTAB_OFFSET);
    sections[3].size = cpu_to_le64(sizeof(strtab));
    sections[3].addralign = cpu_to_le64(1);

    sections[4].name = cpu_to_le32(23);
    sections[4].type = cpu_to_le32(SHT_STRTAB);
    sections[4].offset = cpu_to_le64(TEST_KERNEL_SHSTRTAB_OFFSET);
    sections[4].size = cpu_to_le64(sizeof(shstrtab));
    sections[4].addralign = cpu_to_le64(1);

    fd = g_file_open_tmp("ia64-ipf-kernel-XXXXXX", &kernel_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    written = write(fd, image, TEST_KERNEL_FILE_SIZE);
    g_assert_cmpint(written, ==, TEST_KERNEL_FILE_SIZE);
    close(fd);
}

static void create_test_firmware(void)
{
    g_autofree uint8_t *image = g_malloc(TEST_FIRMWARE_SIZE);
    g_autoptr(GError) error = NULL;
    uint8_t *fv;
    uint8_t *file;
    uint8_t *section;
    ssize_t written;
    int fd;

    fd = g_file_open_tmp("ia64-ipf-qtest-XXXXXX", &firmware_path,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);

    memset(image, 0xff, TEST_FIRMWARE_SIZE);
    build_test_firmware_fit(image + TEST_FIRMWARE_FIT_OFFSET);
    stq_le_p(image + TEST_FIRMWARE_GP_TARGET - TEST_FIRMWARE_BASE,
             TEST_FIRMWARE_GP_SENTINEL);

    /*
     * Supply the minimum FV/PEI-core/TE shape that made the old FIT
     * rewrite path actionable.  It is test data and is not executed
     * under qtest.
     */
    fv = image + TEST_FIRMWARE_FV_OFFSET;
    stq_le_p(fv + 0x20, TEST_FIRMWARE_FV_SIZE);
    memcpy(fv + 0x28, "_FVH", 4);
    stw_le_p(fv + 0x30, TEST_FIRMWARE_FV_HEADER_LEN);

    file = fv + TEST_FIRMWARE_FV_HEADER_LEN;
    memset(file, 0, TEST_FIRMWARE_PEI_FILE_SIZE);
    file[18] = 0x04;
    file[20] = TEST_FIRMWARE_PEI_FILE_SIZE;
    file[23] = 0x07;

    section = file + 24;
    section[0] = TEST_FIRMWARE_TE_SECTION_SIZE;
    section[3] = 0x12;
    stw_le_p(section + 4, 0x5a56);
    stw_le_p(section + 10, 40);

    written = write(fd, image, TEST_FIRMWARE_SIZE);
    g_assert_cmpint(written, ==, TEST_FIRMWARE_SIZE);
    close(fd);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    create_test_firmware();
    create_test_kernel();

    qtest_add_func("/ia64/ipf/qmp-target", test_qmp_target);
    qtest_add_func("/ia64/ipf/direct-kernel-io-contract",
                   test_direct_kernel_io_contract);
    qtest_add_func(
        "/ia64/ipf/firmware-executable-state-unpatched",
        test_firmware_executable_state_is_unpatched);
    qtest_add_func("/ia64/ipf/pci-storage-interfaces",
                   test_pci_storage_interfaces);
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
    qtest_add_func("/ia64/ipf/serial-iosapic-aliases",
                   test_serial_irqs_aliases_and_reset);
    qtest_add_func("/ia64/ipf/i8042-iosapic",
                   test_i8042_irqs_reach_iosapic);
    ret = g_test_run();

    unlink(firmware_path);
    unlink(kernel_path);
    g_free(firmware_path);
    g_free(kernel_path);
    return ret;
}
