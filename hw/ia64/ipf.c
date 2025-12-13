/*
 * Itanium Platform Emulator derived from QEMU PC System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Copyright (c) 2007 Intel
 * Copyright (c) 2011 Prashant Vaibhav <qemu@mercurysquad.com>
 * Ported for IA64 Platform Zhang Xiantao <xiantao.zhang@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/loader.h"
#include "hw/i386/pc.h"
/* #include "fdc.h" */
#include "hw/pci/pci.h"
#include "hw/block/block.h"
#include "qemu/typedefs.h"
#include "hw/sysbus.h"
#include "qom/object.h"
//#include "hw/audio/audio.h"
//#include "hw/net/net.h"
//#include "smbus.h"
#include "hw/boards.h"
//#include "ia64intrin.h"
#include "hw/virtio/virtio-blk.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "elf.h"
#include "target/ia64/cpu.h"
#include "migration/vmstate.h"
#include "system/reset.h"

#define DEBUG_IPF
#ifdef DEBUG_IPF
#define DPRINTF(fmt, ...) \
    do { printf("IPF: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

#define TYPE_IPF_MACHINE MACHINE_TYPE_NAME("ipf")
OBJECT_DECLARE_SIMPLE_TYPE(IPFMachineState, IPF_MACHINE)


#define FIRMWARE_FILE    "Flash.fd"

// XXX: Disable Wunused-variable and Wunused-parameter and Wunused-function
//      for this file.  We need to clean up the code and remove these pragmas.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"

/*
 * Firmware layout helpers (simplified from historical ipf emulator code).
 */
#define GFW_SIZE        (16UL << 20)
#define GFW_START       ((4UL << 30) - GFW_SIZE)


struct IPFMachineState {
    MachineState parent;

    PCIBus *pcibus;
    I2CBus *smbus;

    MemoryRegion rom;
    MemoryRegion rom2;
    MemoryRegion dmamem;
    MemoryRegion bmapm1;
    MemoryRegion bmapm2;
};

#define TYPE_IPF_PC "ipf-pc"
OBJECT_DECLARE_SIMPLE_TYPE(IPFPC, IPF_PC)

struct IPFPC {
    SysBusDevice parent_obj;

    IA64CPU *cpu;
};

static uint64_t ipf_boot_ip;
static uint64_t ipf_boot_r28;
static uint64_t ipf_kernel_low;
static uint64_t ipf_kernel_high;
static uint64_t ipf_kernel_bias;

#define FW_FILENAME "Flash.fd"

/* Leave a chunk of memory at the top of RAM for the BIOS ACPI tables.  */
#define ACPI_DATA_SIZE       0x10000

#define MAX_IDE_BUS 2
#define MAX_IDE_DEVS 2
#if !defined(kvm_enabled)
#define kvm_enabled(x) (0)
#endif

//static ISADevice *rtc_state;
//static PCIDevice *i440fx_state;

extern void rtc_set_memory(ISADevice *dev, int addr, int val);

/*
 * Minimal boot parameter block expected by the IA-64 Linux/Xen entry code.
 * Mirrors xen/include/public/arch-ia64.h (subset).
 */
struct ia64_boot_param {
    uint64_t command_line;
    uint64_t efi_systab;
    uint64_t efi_memmap;
    uint64_t efi_memmap_size;
    uint64_t efi_memdesc_size;
    uint32_t efi_memdesc_version;
    struct {
        uint16_t num_cols;
        uint16_t num_rows;
        uint16_t orig_x;
        uint16_t orig_y;
    } console_info;
    uint64_t fpswa;
    uint64_t initrd_start;
    uint64_t initrd_size;
    uint64_t domain_start;
    uint64_t domain_size;
};

/* Simple layout for boot helper data in guest physical memory. */
#define IPF_BOOT_PARAM_ADDR  0x0000000000008000ULL
#define IPF_CMDLINE_ADDR     0x0000000000009000ULL
#define IPF_EFI_MEMMAP_ADDR  0x0000000000010000ULL
#define IPF_EFI_SYSTAB_ADDR  0x0000000000011000ULL
#define IPF_EFI_RUNTIME_ADDR 0x0000000000012000ULL
#define IPF_EFI_STUBS_ADDR   0x0000000000013000ULL

/* EFI table signatures (see Linux include/linux/efi.h). */
#define EFI_SYSTEM_TABLE_SIGNATURE      0x5453595320494249ULL /* "IBI SYST" */
#define EFI_RUNTIME_SERVICES_SIGNATURE  0x0565245354e5552ULL   /* "RUNTSERV" */
#define EFI_RUNTIME_SERVICES_REVISION   0x00010000U

/* Minimal EFI table header and runtime/system table layouts (64-bit). */
typedef struct QEMU_PACKED {
    uint64_t signature;
    uint32_t revision;
    uint32_t headersize;
    uint32_t crc32;
    uint32_t reserved;
} IPFEfiTableHdr;

typedef struct QEMU_PACKED {
    IPFEfiTableHdr hdr;
    uint64_t get_time;
    uint64_t set_time;
    uint64_t get_wakeup_time;
    uint64_t set_wakeup_time;
    uint64_t set_virtual_address_map;
    uint64_t convert_pointer;
    uint64_t get_variable;
    uint64_t get_next_variable;
    uint64_t set_variable;
    uint64_t get_next_high_mono_count;
    uint64_t reset_system;
    uint64_t update_capsule;
    uint64_t query_capsule_caps;
    uint64_t query_variable_info;
} IPFEfiRuntimeServices;

typedef struct QEMU_PACKED {
    IPFEfiTableHdr hdr;
    uint64_t fw_vendor;
    uint32_t fw_revision;
    uint32_t pad0;
    uint64_t con_in_handle;
    uint64_t con_in;
    uint64_t con_out_handle;
    uint64_t con_out;
    uint64_t stderr_handle;
    uint64_t stderr;
    uint64_t runtime;
    uint64_t boottime;
    uint64_t nr_tables;
    uint64_t tables;
} IPFEfiSystemTable;

/*
 * Minimal EFI memory descriptor (per UEFI spec) as expected by Linux/ia64.
 * Size must match ia64_boot_param->efi_memdesc_size.
 */
struct efi_memory_desc {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_addr;
    uint64_t virt_addr;
    uint64_t num_pages;
    uint64_t attribute;
};

/* EFI memory types (subset). */
#define EFI_RESERVED_TYPE            0
#define EFI_CONVENTIONAL_MEMORY      7

/*
 * Tiny EFI runtime service stubs (IA-64 machine code) placed in guest memory.
 *
 * - ipf_efi_set_virtual_address_map(): returns EFI_SUCCESS (0).
 * - ipf_efi_stub_unsupported(): returns EFI_UNSUPPORTED (0x8000..0003).
 *
 * Generated with ia64-suse-linux-as/ld/objcopy; see /tmp snippets in logs.
 */
static const uint8_t ipf_efi_stub_set_virtual_address_map[] = {
    0x11, 0x40, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x80, 0x08, 0x00, 0x84, 0x00,
};

static const uint8_t ipf_efi_stub_unsupported[] = {
    0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x31, 0x00, 0x00, 0x68,
    0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x80, 0x08, 0x00, 0x84, 0x00,
};

//static uint32_t ipf_to_legacy_io(target_phys_addr_t addr)
//{
//    return (uint32_t)(((addr&0x3ffffff) >> 12 << 2)|((addr) & 0x3));
//}
//
//static void ipf_legacy_io_writeb(void *opaque, target_phys_addr_t addr,
//				 uint32_t val) {
//    uint32_t port = ipf_to_legacy_io(addr);
//
//    cpu_outb(port, val);
//}
//
//static void ipf_legacy_io_writew(void *opaque, target_phys_addr_t addr,
//				 uint32_t val) {
//    uint32_t port = ipf_to_legacy_io(addr);
//
//    cpu_outw(port, val);
//}
//
//static void ipf_legacy_io_writel(void *opaque, target_phys_addr_t addr,
//				 uint32_t val) {
//    uint32_t port = ipf_to_legacy_io(addr);
//
//    cpu_outl(port, val);
//}
//
//static uint32_t ipf_legacy_io_readb(void *opaque, target_phys_addr_t addr)
//{
//    uint32_t port = ipf_to_legacy_io(addr);
//
//    return cpu_inb(port);
//}
//
//static uint32_t ipf_legacy_io_readw(void *opaque, target_phys_addr_t addr)
//{
//    uint32_t port = ipf_to_legacy_io(addr);
//
//    return cpu_inw(port);
//}
//
//static uint32_t ipf_legacy_io_readl(void *opaque, target_phys_addr_t addr)
//{
//    uint32_t port = ipf_to_legacy_io(addr);
//
//    return cpu_inl(port);
//}
//
//static CPUReadMemoryFunc *ipf_legacy_io_read[3] = {
//    ipf_legacy_io_readb,
//    ipf_legacy_io_readw,
//    ipf_legacy_io_readl,
//};
//
//static CPUWriteMemoryFunc *ipf_legacy_io_write[3] = {
//    ipf_legacy_io_writeb,
//    ipf_legacy_io_writew,
//    ipf_legacy_io_writel,
//};

//static void pic_irq_request(void *opaque, int irq, int level)
//{
//    fprintf(stderr,"pic_irq_request called!\n");
//}

/* PC cmos mappings */

#define REG_EQUIPMENT_BYTE          0x14

#if 0 /* pvaibhav: disabling floppy drive emulation */
static int cmos_get_fd_drive_type(int fd0)
{
    int val;

    switch (fd0) {
    case 0:
        /* 1.44 Mb 3"5 drive */
        val = 4;
        break;
    case 1:
        /* 2.88 Mb 3"5 drive */
        val = 5;
        break;
    case 2:
        /* 1.2 Mb 5"5 drive */
        val = 2;
        break;
    default:
        val = 0;
        break;
    }
    return val;
}
#endif

//static void cmos_init_hd(int type_ofs, int info_ofs, BlockDriverState *hd)
//{
//    ISADevice *s = rtc_state;
//    int cylinders, heads, sectors;
//
//    // FIXME: fix
//    //bdrv_get_geometry_hint(hd, &cylinders, &heads, &sectors);
//    rtc_set_memory(s, type_ofs, 47);
//    rtc_set_memory(s, info_ofs, cylinders);
//    rtc_set_memory(s, info_ofs + 1, cylinders >> 8);
//    rtc_set_memory(s, info_ofs + 2, heads);
//    rtc_set_memory(s, info_ofs + 3, 0xff);
//    rtc_set_memory(s, info_ofs + 4, 0xff);
//    rtc_set_memory(s, info_ofs + 5, 0xc0 | ((heads > 8) << 3));
//    rtc_set_memory(s, info_ofs + 6, cylinders);
//    rtc_set_memory(s, info_ofs + 7, cylinders >> 8);
//    rtc_set_memory(s, info_ofs + 8, sectors);
//}
//
///* convert boot_device letter to something recognizable by the bios */
//static int boot_device2nibble(char boot_device)
//{
//    switch(boot_device) {
//    case 'a':
//    case 'b':
//        return 0x01; /* floppy boot */
//    case 'c':
//        return 0x02; /* hard drive boot */
//    case 'd':
//        return 0x03; /* CD-ROM boot */
//    case 'n':
//        return 0x04; /* Network boot */
//    }
//    return 0;
//}

/* hd_table must contain 4 block drivers */
//static void cmos_init(ram_addr_t ram_size, ram_addr_t above_4g_mem_size,
//                      const char *boot_device, BlockDriverState **hd_table)
//{
//    ISADevice *s = rtc_state;
//    int nbds, bds[3] = { 0, };
//    int val;
//    /* int fd0, fd1, nb; */
//    int i;
//
//    /* various important CMOS locations needed by PC/Bochs bios */
//
//    /* memory size */
//    val = 640; /* base memory in K */
//    rtc_set_memory(s, 0x15, val);
//    rtc_set_memory(s, 0x16, val >> 8);
//
//    val = (ram_size / 1024) - 1024;
//    if (val > 65535)
//        val = 65535;
//    rtc_set_memory(s, 0x17, val);
//    rtc_set_memory(s, 0x18, val >> 8);
//    rtc_set_memory(s, 0x30, val);
//    rtc_set_memory(s, 0x31, val >> 8);
//
//    if (above_4g_mem_size) {
//        rtc_set_memory(s, 0x5b, (unsigned int)above_4g_mem_size >> 16);
//        rtc_set_memory(s, 0x5c, (unsigned int)above_4g_mem_size >> 24);
//        rtc_set_memory(s, 0x5d, above_4g_mem_size >> 32);
//    }
//    //rtc_set_memory(s, 0x5f, smp_cpus - 1);
//    // FIXME:
//    rtc_set_memory(s, 0x5f, 0);
//
//    if (ram_size > (16 * 1024 * 1024))
//        val = (ram_size / 65536) - ((16 * 1024 * 1024) / 65536);
//    else
//        val = 0;
//    if (val > 65535)
//        val = 65535;
//    rtc_set_memory(s, 0x34, val);
//    rtc_set_memory(s, 0x35, val >> 8);
//
//    /* set boot devices, and disable floppy signature check if requested */
//#define PC_MAX_BOOT_DEVICES 3
//    nbds = strlen(boot_device);
//
//    if (nbds > PC_MAX_BOOT_DEVICES) {
//        fprintf(stderr, "Too many boot devices for PC\n");
//        exit(1);
//    }
//
//    for (i = 0; i < nbds; i++) {
//        bds[i] = boot_device2nibble(boot_device[i]);
//        if (bds[i] == 0) {
//            fprintf(stderr, "Invalid boot device for PC: '%c'\n",
//                    boot_device[i]);
//            exit(1);
//        }
//    }
//
//    rtc_set_memory(s, 0x3d, (bds[1] << 4) | bds[0]);
//    // FIXME:
//    //rtc_set_memory(s, 0x38, (bds[2] << 4) | (fd_bootchk ?  0x0 : 0x1));
//    rtc_set_memory(s, 0x38, (bds[2] << 4));
//
//    /* floppy type */
//#if 0 /* pvaibhav : disabling floppy drive emulation */
//    fd0 = fdctrl_get_drive_type(floppy_controller, 0);
//    fd1 = fdctrl_get_drive_type(floppy_controller, 1);
//
//    val = (cmos_get_fd_drive_type(fd0) << 4) | cmos_get_fd_drive_type(fd1);
//    rtc_set_memory(s, 0x10, val);
//
//    val = 0;
//    nb = 0;
//    if (fd0 < 3)
//        nb++;
//    if (fd1 < 3)
//        nb++;
//
//    switch (nb) {
//    case 0:
//        break;
//    case 1:
//        val |= 0x01; /* 1 drive, ready for boot */
//        break;
//    case 2:
//        val |= 0x41; /* 2 drives, ready for boot */
//        break;
//    }
//#endif
//
//    val |= 0x02; /* FPU is there */
//    val |= 0x04; /* PS/2 mouse installed */
//    rtc_set_memory(s, REG_EQUIPMENT_BYTE, val);
//
//    /* hard drives */
//
//    rtc_set_memory(s, 0x12, (hd_table[0] ? 0xf0 : 0) | (hd_table[1] ? 0x0f : 0));
//    if (hd_table[0])
//        cmos_init_hd(0x19, 0x1b, hd_table[0]);
//    if (hd_table[1])
//        cmos_init_hd(0x1a, 0x24, hd_table[1]);
//
//    val = 0;
//    for (i = 0; i < 4; i++) {
//        if (hd_table[i]) {
//            //int cylinders, heads, sectors, translation;
//            /* NOTE: bdrv_get_geometry_hint() returns the physical
//               geometry.  It is always such that: 1 <= sects <= 63, 1
//               <= heads <= 16, 1 <= cylinders <= 16383. The BIOS
//               geometry can be different if a translation is done. */
//               // FIXME:
//            //translation = bdrv_get_translation_hint(hd_table[i]);
//            //if (translation == BIOS_ATA_TRANSLATION_AUTO) {
//            //    bdrv_get_geometry_hint(hd_table[i], &cylinders,
//            //                           &heads, &sectors);
//            //    if (cylinders <= 1024 && heads <= 16 && sectors <= 63) {
//            //        /* No translation. */
//            //        translation = 0;
//            //    } else {
//            //        /* LBA translation. */
//            //        translation = 1;
//            //    }
//            //} else {
//            //    translation--;
//            //}
//            //val |= translation << (i * 2);
//        }
//    }
//    rtc_set_memory(s, 0x39, val);
//}

//static void main_cpu_reset(void *opaque)
//{
//    CPUState *env = opaque;
//    cpu_reset(env);
//}

//static const int ide_iobase[2] = { 0x1f0, 0x170 };
//static const int ide_iobase2[2] = { 0x3f6, 0x376 };
//static const int ide_irq[2] = { 14, 15 };
//
//#define NE2000_NB_MAX 6
//
//static int ne2000_io[NE2000_NB_MAX] = { 0x300, 0x320, 0x340,
//                                        0x360, 0x280, 0x380 };
//static int ne2000_irq[NE2000_NB_MAX] = { 9, 10, 11, 3, 4, 5 };
//
//#define MAX_SERIAL_PORTS 4
//
//static int serial_io[MAX_SERIAL_PORTS] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8 };
//static int serial_irq[MAX_SERIAL_PORTS] = { 4, 3, 4, 3 };
//
//#define MAX_PARALLEL_PORTS 3
//
//static int parallel_io[MAX_PARALLEL_PORTS] = { 0x378, 0x278, 0x3bc };
//static int parallel_irq[MAX_PARALLEL_PORTS] = { 7, 7, 7 };

#ifdef HAS_AUDIO
static void audio_init (PCIBus *pci_bus, qemu_irq *pic)
{
    struct soundhw *c;
    int audio_enabled = 0;

    for (c = soundhw; !audio_enabled && c->name; ++c) {
        audio_enabled = c->enabled;
    }

    if (audio_enabled) {
        AudioState *s;

        s = AUD_init ();
        if (s) {
            for (c = soundhw; c->name; ++c) {
                if (c->enabled) {
                    if (c->isa) {
                        c->init.init_isa (s, pic);
                    } else {
                        if (pci_bus) {
                            c->init.init_pci (pci_bus, s);
                        }
                    }
                }
            }
        }
    }
}
#endif

#if 0
static void pc_init_ne2k_isa(NICInfo *nd, qemu_irq *pic)
{
    static int nb_ne2k = 0;

    if (nb_ne2k == NE2000_NB_MAX)
        return;

    isa_ne2000_init(ne2000_io[nb_ne2k], pic[ne2000_irq[nb_ne2k]], nd);
    nb_ne2k++;
}
#endif

static void ipf_pc_reset(DeviceState *dev)
{
    //IPFPC *s = IPF_PC(dev);

    ///* Set internal registers to initial values */
    ///*     0x0000XX00 << vital bits */
    //s->scr1 = 0x00011102;
    //s->scr2 = 0x00ff0c80;
    //s->old_scr2 = s->scr2;

    //s->rtc.status = 0x90;

    ///* Load RTC RAM - TODO: provide possibility to load contents from file */
    //memcpy(s->rtc.ram, rtc_ram2, 32);
}

static void ipf_pc_realize(DeviceState *dev, Error **errp)
{
    //IPFPC *s = IPF_PC(dev);
    //SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    //qdev_init_gpio_in(dev, next_irq, NEXT_NUM_IRQS);

    //memory_region_init_io(&s->mmiomem, OBJECT(s), &next_mmio_ops, s,
    //                      "next.mmio", 0xd0000);
    //memory_region_init_io(&s->scrmem, OBJECT(s), &next_scr_ops, s,
    //                      "next.scr", 0x20000);
    //sysbus_init_mmio(sbd, &s->mmiomem);
    //sysbus_init_mmio(sbd, &s->scrmem);
}



static const Property ipf_pc_properties[] = {
    DEFINE_PROP_LINK("cpu", IPFPC, cpu, TYPE_IA64_CPU, IA64CPU *),
};

static const VMStateDescription ipf_pc_vmstate = {
    .name = "ipf-pc",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void ipf_pc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Itanium Platform PC";
    dc->realize = ipf_pc_realize;
    //dc->reset = ipf_pc_reset;
    device_class_set_props(dc, ipf_pc_properties);
    dc->vmsd = &ipf_pc_vmstate;
}

static const TypeInfo ipf_pc_info = {
    .name = TYPE_IPF_PC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPFPC),
    .class_init = ipf_pc_class_init,
};

static void main_cpu_reset(void *opaque)
{
    CPUState *env = opaque;
    IA64CPU *cpu = IA64_CPU(env);
    CPUIA64State *s = &cpu->env;

    cpu_reset(env);
    DPRINTF("Reset CPU: boot_ip=0x%" PRIx64 " boot_r28=0x%" PRIx64 "\n",
            ipf_boot_ip, ipf_boot_r28);
    if (ipf_boot_ip) {
        s->ip = ipf_boot_ip;
    }
    if (ipf_boot_r28) {
        s->r[28] = ipf_boot_r28;
    }

    /*
     * Provide a small VHPT area in region 7 so Linux's early DTLB handler
     * doesn't try to probe address 0 when rr[0..5] enable VHPT.
     *
     * This is a pragmatic "firmware default": VF=1, VRN=7, SIZE=0 (32KB),
     * BASE=1MB, VE=1.
     */
    {
        uint64_t vhpt_base = 0x0000000000100000ULL;
        uint64_t pta = 0;
        pta |= (7ULL << 61);                /* VRN=7 */
        pta |= ((vhpt_base >> 15) & ((1ULL << 46) - 1)) << 15; /* BASE */
        pta |= (1ULL << 8);                 /* VF=1 */
        pta |= (0ULL << 2);                 /* SIZE=0 */
        pta |= 1ULL;                        /* VE=1 */
        s->cr[8] = pta;                     /* cr.pta */
    }
}

/* Itanium hardware initialisation */
static void ipf_init(MachineState *machine)
{
    IPFMachineState *m = IPF_MACHINE(machine);
    IA64CPU *cpu;
    CPUIA64State *env;
    MemoryRegion *sysmem = get_system_memory();
    const char *bios_name = machine->firmware ?: FIRMWARE_FILE;
    DeviceState *pcdev;
    uint64_t kernel_entry = 0, kernel_low = 0, kernel_high = 0;
    const char *kernel_filename = machine->kernel_filename;
    const char *kernel_cmdline_in = machine->kernel_cmdline ?: "";
    const char *kernel_cmdline;
    const char *initrd_filename = machine->initrd_filename;
    struct ia64_boot_param bp = { 0 };
    size_t cmdline_len;
    int64_t initrd_size = 0;
    uint64_t initrd_base = 0;
    uint64_t cmdline_addr = IPF_CMDLINE_ADDR;
    int64_t image_size;
    (void)m;
    /* Initialize the cpu core */
    cpu = IA64_CPU(cpu_create(machine->cpu_type));
    if (!cpu) {
        error_report("Unable to find ia64 CPU definition");
        exit(1);
    }
    env = &cpu->env;

    pcdev = qdev_new(TYPE_IPF_PC);
    object_property_set_link(OBJECT(pcdev), "cpu", OBJECT(cpu), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pcdev), &error_fatal);

    memory_region_add_subregion(sysmem, 0, machine->ram);

    /* Optional firmware load if provided. */
    image_size = get_image_size(bios_name);
    if (image_size > 0) {
        int64_t fw_offset = GFW_START + GFW_SIZE - image_size;
        memory_region_init_rom(&m->rom, NULL, "ipf.rom", GFW_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, GFW_START, &m->rom);
        if (load_image_targphys(bios_name, fw_offset, image_size) != image_size) {
            error_report("Unable to load firmware file '%s'", bios_name);
            exit(1);
        }
        DPRINTF("Loaded firmware '%s' at 0x%lx\n", bios_name, fw_offset);
    }

    if (!kernel_filename) {
        error_report("No -kernel specified for IPF");
        exit(1);
    }

    /*
     * Load the ELF kernel. The IA-64 ELF uses physical p_paddr; entry
     * point phys_start is a low PA (see System.map).
     */
    if (load_elf_ram_sym(kernel_filename, NULL, NULL, NULL,
                         &kernel_entry, &kernel_low, &kernel_high, NULL,
                         ELFDATA2LSB, EM_IA_64, 0, 0,
                         NULL, false, NULL) < 0) {
        error_report("Unable to load kernel '%s'", kernel_filename);
        exit(1);
    }
    (void)kernel_low;
    (void)kernel_high;
    ipf_kernel_low = kernel_low;
    ipf_kernel_high = kernel_high;
    ipf_kernel_bias = 0xa000000100000000ULL - kernel_low;

    /* Initrd placement (optional). */
    if (initrd_filename) {
        initrd_size = get_image_size(initrd_filename);
        if (initrd_size < 0) {
            error_report("Unable to get size of initrd '%s'", initrd_filename);
            exit(1);
        }
        /*
         * Place initrd high in RAM but below 1G to avoid firmware quirks.
         * Align down to 64K.
         */
        uint64_t limit = MIN(machine->ram_size, 1ULL << 30);
        if (limit < (uint64_t)initrd_size + 0x10000) {
            error_report("RAM too small for initrd");
            exit(1);
        }
        initrd_base = QEMU_ALIGN_DOWN(limit - initrd_size, 0x10000);
        if (load_image_targphys(initrd_filename, initrd_base, initrd_size) != initrd_size) {
            error_report("Unable to load initrd '%s'", initrd_filename);
            exit(1);
        }
        bp.initrd_start = initrd_base;
        bp.initrd_size = initrd_size;
    }

    /* Command line */
    kernel_cmdline = (*kernel_cmdline_in) ? kernel_cmdline_in : "earlyprintk";
    cmdline_len = strlen(kernel_cmdline) + 1;
    if (cmdline_len > 4096) {
        error_report("Kernel cmdline too long");
        exit(1);
    }
    address_space_write(&address_space_memory, cmdline_addr, MEMTXATTRS_UNSPECIFIED,
                        (const uint8_t *)kernel_cmdline, cmdline_len);
    bp.command_line = cmdline_addr;
    bp.console_info.num_cols = 80;
    bp.console_info.num_rows = 25;

    /*
     * Minimal EFI memory map:
     * - Reserve low memory containing boot params + EFI tables.
     * - Mark the rest as conventional RAM.
     *
     * IA-64 Linux expects the EFI system table to live in non-conventional
     * memory; otherwise it may be overwritten during early memblock init.
     */
    {
        uint64_t reserve_end = 0x0000000000020000ULL; /* 128KB */
        reserve_end = MIN(reserve_end, machine->ram_size);
        reserve_end = QEMU_ALIGN_UP(reserve_end, 4096);

        struct efi_memory_desc md[2] = { 0 };
        md[0].type = EFI_RESERVED_TYPE;
        md[0].phys_addr = 0;
        md[0].virt_addr = 0;
        md[0].num_pages = reserve_end / 4096;
        md[0].attribute = 0;

        md[1].type = EFI_CONVENTIONAL_MEMORY;
        md[1].phys_addr = reserve_end;
        md[1].virt_addr = 0;
        md[1].num_pages = (machine->ram_size - reserve_end) / 4096;
        md[1].attribute = 0;

        address_space_write(&address_space_memory, IPF_EFI_MEMMAP_ADDR,
                            MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)md, sizeof(md));
        bp.efi_memmap = IPF_EFI_MEMMAP_ADDR;
        bp.efi_memmap_size = sizeof(md);
        bp.efi_memdesc_size = sizeof(md[0]);
        bp.efi_memdesc_version = 1;
    }

    /*
     * Minimal EFI system + runtime services tables.
     *
     * IA-64 Linux requires a non-NULL EFI system table and will call into
     * runtime->set_virtual_address_map() during early boot.
     */
    {
        uint64_t stub_ok = IPF_EFI_STUBS_ADDR;
        uint64_t stub_unsupported = IPF_EFI_STUBS_ADDR +
                                    sizeof(ipf_efi_stub_set_virtual_address_map);

        address_space_write(&address_space_memory, stub_ok, MEMTXATTRS_UNSPECIFIED,
                            ipf_efi_stub_set_virtual_address_map,
                            sizeof(ipf_efi_stub_set_virtual_address_map));
        address_space_write(&address_space_memory, stub_unsupported, MEMTXATTRS_UNSPECIFIED,
                            ipf_efi_stub_unsupported,
                            sizeof(ipf_efi_stub_unsupported));

        IPFEfiRuntimeServices rt = { 0 };
        rt.hdr.signature = EFI_RUNTIME_SERVICES_SIGNATURE;
        rt.hdr.revision = EFI_RUNTIME_SERVICES_REVISION;
        rt.hdr.headersize = sizeof(rt);
        rt.set_virtual_address_map = stub_ok;
        rt.get_time = stub_unsupported;
        rt.set_time = stub_unsupported;
        rt.get_wakeup_time = stub_unsupported;
        rt.set_wakeup_time = stub_unsupported;
        rt.get_variable = stub_unsupported;
        rt.get_next_variable = stub_unsupported;
        rt.set_variable = stub_unsupported;
        rt.get_next_high_mono_count = stub_unsupported;
        rt.reset_system = stub_unsupported;
        rt.update_capsule = stub_unsupported;
        rt.query_capsule_caps = stub_unsupported;
        rt.query_variable_info = stub_unsupported;

        address_space_write(&address_space_memory, IPF_EFI_RUNTIME_ADDR,
                            MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)&rt, sizeof(rt));

        IPFEfiSystemTable st = { 0 };
        st.hdr.signature = EFI_SYSTEM_TABLE_SIGNATURE;
        st.hdr.revision = 0x00010000;
        st.hdr.headersize = sizeof(st);
        st.runtime = IPF_EFI_RUNTIME_ADDR;
        st.nr_tables = 0;
        st.tables = 0;

        address_space_write(&address_space_memory, IPF_EFI_SYSTAB_ADDR,
                            MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)&st, sizeof(st));

        bp.efi_systab = IPF_EFI_SYSTAB_ADDR;
    }

    address_space_write(&address_space_memory, IPF_BOOT_PARAM_ADDR, MEMTXATTRS_UNSPECIFIED,
                        (const uint8_t *)&bp, sizeof(bp));

    /*
     * Boot in physical mode at the ELF entry point. Linux head.S expects
     * execution reaches _start() in physical mode and will install TRs before
     * switching to virtual mode.
     */
    ipf_boot_ip = kernel_entry;
    ipf_boot_r28 = IPF_BOOT_PARAM_ADDR;
    env->ip = ipf_boot_ip;
    env->r[28] = ipf_boot_r28;
    DPRINTF("Kernel entry 0x%" PRIx64 " low=0x%" PRIx64 " high=0x%" PRIx64 "\n",
            kernel_entry, kernel_low, kernel_high);
    qemu_register_reset(main_cpu_reset, cpu);

    /*Register legacy io address space, size:64M*/
    //ipf_legacy_io_base = 0xE0000000;
    //ipf_legacy_io_mem = cpu_register_io_memory(ipf_legacy_io_read,
    //                                           ipf_legacy_io_write, NULL,
    //                                           DEVICE_LITTLE_ENDIAN);
    //cpu_register_physical_memory(ipf_legacy_io_base, 64*1024*1024,
    //                             ipf_legacy_io_mem);

    //cpu_irq = qemu_allocate_irqs(pic_irq_request, first_cpu, 1);
    //i8259 = kvm_i8259_init(cpu_irq[0]);

    //if (pci_enabled) {
    //    pci_bus = i440fx_init(&i440fx_state, i8259);
    //    piix3_devfn = piix3_init(pci_bus, -1);
    //} else {
    //    pci_bus = NULL;
    //}

    //if (cirrus_vga_enabled) {
    //    if (pci_enabled)
    //        pci_cirrus_vga_init(pci_bus);
    //    else
    //        isa_cirrus_vga_init();
    //} else {
    //    if (pci_enabled)
    //        pci_vga_init(pci_bus, 0, 0);
    //    else
    //        isa_vga_init();
    //}

    //rtc_state = rtc_init(0x70, i8259[8], 2000);

    //if (pci_enabled) {
    //    pic_set_alt_irq_func(isa_pic, NULL, NULL);
    //}

    //for(i = 0; i < MAX_SERIAL_PORTS; i++) {
    //    if (serial_hds[i]) {
    //        serial_init(serial_io[i], i8259[serial_irq[i]], 115200,
    //                    serial_hds[i]);
    //    }
    //}

    //for(i = 0; i < MAX_PARALLEL_PORTS; i++) {
    //    if (parallel_hds[i]) {
    //        parallel_init(parallel_io[i], i8259[parallel_irq[i]],
    //                      parallel_hds[i]);
    //    }
    //}

    //for(i = 0; i < nb_nics; i++) {
    //    NICInfo *nd = &nd_table[i];

    //    if (!pci_enabled || (nd->model && strcmp(nd->model, "ne2k_isa") == 0))
    //        pc_init_ne2k_isa(nd /*, i8259*/);
    //    else
    //        pci_nic_init(nd, "e1000", NULL);
    //}

//#undef USE_HYPERCALL  //Disable it now, need to implement later!
//#ifdef USE_HYPERCALL
//    //pci_hypercall_init(pci_bus);
//#endif

    //if (drive_get_max_bus(IF_IDE) >= MAX_IDE_BUS) {
    //    fprintf(stderr, "qemu: too many IDE bus\n");
    //    exit(1);
    //}

    //for(i = 0; i < MAX_IDE_BUS * MAX_IDE_DEVS; i++) {
    //    index = drive_get_index(IF_IDE, i / MAX_IDE_DEVS, i % MAX_IDE_DEVS);
	//if (index != -1)
	   // hd[i] = drives_table[index].bdrv;
	//else
	   // hd[i] = NULL;
    //}

    //if (pci_enabled) {
    //    pci_piix3_ide_init(pci_bus, hd, piix3_devfn + 1, i8259);
    //} else {
    //    for(i = 0; i < MAX_IDE_BUS; i++) {
    //        isa_ide_init(ide_iobase[i], ide_iobase2[i], i8259[ide_irq[i]],
	   //              hd[MAX_IDE_DEVS * i], hd[MAX_IDE_DEVS * i + 1]);
    //    }
    //}

    //i8042_init(i8259[1], i8259[12], 0x60);
    //DMA_init(0);
//#ifdef HAS_AUDIO
//    audio_init(pci_enabled ? pci_bus : NULL, i8259);
//#endif
#if 0 /* pvaibhav: disable floppy emulation */
    for(i = 0; i < MAX_FD; i++) {
        index = drive_get_index(IF_FLOPPY, 0, i);
	if (index != -1)
	    fd[i] = drives_table[index].bdrv;
	else
	    fd[i] = NULL;
    }
    floppy_controller = fdctrl_init(i8259[6], 2, 0, 0x3f0, fd);
#endif
    //cmos_init(ram_size, above_4g_mem_size, boot_device, hd);

    //if (pci_enabled && usb_enabled) {
    //    usb_uhci_piix3_init(pci_bus, piix3_devfn + 2);
    //}

    //if (pci_enabled && acpi_enabled) {
    //    uint8_t *eeprom_buf = qemu_mallocz(8 * 256); /* XXX: make this persistent */
    //    i2c_bus *smbus;

    //    /* TODO: Populate SPD eeprom data.  */
    //    smbus = piix4_pm_init(pci_bus, piix3_devfn + 3, 0xb100, i8259[9]);
    //    for (i = 0; i < 8; i++) {
    //        DeviceState *eeprom;
    //        eeprom = qdev_create((BusState *)smbus, "smbus-eeprom");
    //        qdev_set_prop_int(eeprom, "address", 0x50 + i);
    //        qdev_set_prop_ptr(eeprom, "data", eeprom_buf + (i * 256));
    //        qdev_init(eeprom);
    //    }
    //}

    //if (i440fx_state) {
    //    i440fx_init_memory_mappings(i440fx_state);
    //}

    //if (pci_enabled) {
    //int max_bus;
    //    int bus;

    //    max_bus = drive_get_max_bus(IF_SCSI);
    //for (bus = 0; bus <= max_bus; bus++) {
    //        pci_create_simple(pci_bus, -1, "lsi53c895a");
    //    }
    //}
    ///* Add virtio block devices */
    //if (pci_enabled) {
    //int index;
    //int unit_id = 0;

	//while ((index = drive_get_index(IF_VIRTIO, 0, unit_id)) != -1) {
    //        pci_dev = pci_create("virtio-blk-pci",
    //                             drives_table[index].devaddr);
    //        qdev_init(&pci_dev->qdev);
	   // unit_id++;
	//}
    //}

    DPRINTF("IPF init done\n");
}
//
//static void ipf_init_pci(ram_addr_t ram_size,
//                         const char *boot_device, DisplayState *ds,
//                         const char *kernel_filename,
//                         const char *kernel_cmdline,
//                         const char *initrd_filename,
//                         const char *cpu_model)
//{
//    ipf_init1(ram_size, boot_device, ds, kernel_filename,
//              kernel_cmdline, initrd_filename, 1, cpu_model);
//}


//#define IOAPIC_NUM_PINS2 48
//
//static int ioapic_irq_count[IOAPIC_NUM_PINS2];
//
//static int ioapic_map_irq(int devfn, int irq_num)
//{
//    int irq, dev;
//    dev = devfn >> 3;
//    irq = ((((dev << 2) + (dev >> 3) + irq_num) & 31) + 16);
//    return irq;
//}

/*
 * Dummy function to provide match for call from hw/apic.c
 */
void apic_set_irq_delivered(void);
void apic_set_irq_delivered(void) {
}

void ioapic_set_irq(void *opaque, int irq_num, int level);
void ioapic_set_irq(void *opaque, int irq_num, int level)
{
   // int vector, pic_ret;

   // PCIDevice *pci_dev = (PCIDevice *)opaque;
   // vector = ioapic_map_irq(pci_dev->devfn, irq_num);

   // if (level)
   //     ioapic_irq_count[vector] += 1;
   // else
   //     ioapic_irq_count[vector] -= 1;

   // if (kvm_enabled()) {
    //	if (kvm_set_irq(vector, ioapic_irq_count[vector] == 0, &pic_ret))
   //         if (pic_ret != 0)
   //             apic_set_irq_delivered();
	  //  return;
   // }
}

//int ipf_map_irq(PCIDevice *pci_dev, int irq_num);
//int ipf_map_irq(PCIDevice *pci_dev, int irq_num)
//{
//	return ioapic_map_irq(pci_dev->devfn, irq_num);
//}

static void ipf_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Itanium Platform";
    mc->init = ipf_init;
    mc->block_default_type = IF_IDE;
    mc->default_ram_size = 256 * 1024 * 1024;
    mc->default_ram_id = "ipf.ram";
    mc->default_cpu_type = IA64_CPU_TYPE_NAME("itanium");
    mc->is_default = true;
}


static const TypeInfo ipf_typeinfo = {
    .name = TYPE_IPF_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = ipf_machine_class_init,
    .instance_size = sizeof(IPFMachineState),
};

static void ipf_register_type(void)
{
    type_register_static(&ipf_typeinfo);
    type_register_static(&ipf_pc_info);

}

type_init(ipf_register_type)


// XXX: pop the pragmas
#pragma GCC diagnostic pop
