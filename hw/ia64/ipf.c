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
#include "hw/pci/pci_ids.h"
#include "hw/pci/pci_host.h"
#include "hw/southbridge/piix.h"
#include "qemu/cutils.h"
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
#include "hw/char/serial-mm.h"
#include "hw/irq.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "system/ioport.h"
#include "elf.h"
#include "hw/ia64/gfw.h"
#include "target/ia64/cpu.h"
#include "migration/vmstate.h"
#include "system/reset.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "chardev/char.h"
#include <ctype.h>

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
/*
 * Xen's IA-64 HVM builder enters guest firmware at a pseudo-reset entry point
 * within the GFW window (see xc_ia64_hvm_build.c). This is a region-encoded
 * address; our physical-mode TLB fill masks it into the 32-bit GFW window.
 */
#define IPF_GFW_ENTRY     0x80000000ffffffb0ULL

// XXX: Disable Wunused-variable and Wunused-parameter and Wunused-function
//      for this file.  We need to clean up the code and remove these pragmas.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"

/*
 * Firmware layout helpers.
 *
 * NOTE: These addresses are the traditional IA-64 guest firmware window used
 * by Xen/KVM GFW, and match the location of the IA-64 reset vector
 * (0xffff0000 == GFW_START + 0x00ff0000).
 */

struct IPFMachineState {
    MachineState parent;

    PCIBus *pcibus;
    I2CBus *smbus;
    IA64CPU *cpu;

    /*
     * If enabled, run the guest firmware first and hand off to the loaded
     * -kernel ELF once the firmware returns (Xen/KVM GFW style).
     */
    bool firmware_preboot;

    /* Primary UART is serial-mm at IPF_UART_BASE; also aliased to COM1 ioports. */
    SerialMM *uart_mm;
    MemoryRegion uart_ioport;
    MemoryRegion debugcon_e9;
    MemoryRegion debugcon_402;
    Chardev *debugcon_chr;
    bool debugcon_line_mode;
    size_t debugcon_line_len;
    char debugcon_line[512];
    bool debugcon_trace_once;
    bool debugcon_line_traced;

    MemoryRegion rom;
    MemoryRegion ram_low;
    MemoryRegion ram_high;
    MemoryRegion ram_slack;
    MemoryRegion fw_workram;
    MemoryRegion fw_workram_alias;
    MemoryRegion dmamem;
    MemoryRegion bmapm1;
    MemoryRegion bmapm2;
    MemoryRegion legacy_io_mmio;
    MemoryRegion acpi_pm_mmio;
    uint16_t acpi_pm1_evt_sts;
    uint16_t acpi_pm1_evt_en;
    uint16_t acpi_pm1_cnt;
    uint64_t acpi_pm_timer_start_ns;
    MemoryRegion iosapic_mmio;
    uint32_t iosapic_reg_select;
    uint32_t iosapic_reg[0x40];

    /* Lightweight debug watchpoints (see QEMU_IA64_WATCH_* env vars). */
    struct IpfTextWatch *text_watch[8];

    /* I/O tracing state (QEMU_IPF_TRACE_* env vars). */
    uint32_t trace_pci_cfgaddr;
};

#define TYPE_IPF_PC "ipf-pc"
OBJECT_DECLARE_SIMPLE_TYPE(IPFPC, IPF_PC)

struct IPFPC {
    SysBusDevice parent_obj;

    IA64CPU *cpu;
};

#define TYPE_IPF_PCI_HOST "ipf-pci-host"
OBJECT_DECLARE_SIMPLE_TYPE(IPFPCIHost, IPF_PCI_HOST)

struct IPFPCIHost {
    PCIHostState parent_obj;
};

#define TYPE_IPF_PCI_ROOT_DEVICE "ipf-pci-root"
OBJECT_DECLARE_SIMPLE_TYPE(IPFPCIRoot, IPF_PCI_ROOT_DEVICE)

struct IPFPCIRoot {
    PCIDevice parent_obj;
};

static uint64_t ipf_boot_ip;
static uint64_t ipf_boot_r28;
static uint64_t ipf_boot_r9;
static uint64_t ipf_boot_r10;
static uint64_t ipf_boot_ppi;
static uint64_t ipf_boot_findfv_stub;
static uint64_t ipf_boot_findfv_iface;
static uint64_t ipf_boot_secinfo_stub;
static uint64_t ipf_boot_memmap_stub;
static uint64_t ipf_boot_mem_size;
static uint64_t ipf_ram_size;
static uint64_t ipf_kernel_low;
static uint64_t ipf_kernel_high;
static uint64_t ipf_kernel_bias;
static uint64_t ipf_sym_io_space;
static uint64_t ipf_sym_ia64_bad_break;
static uint64_t ipf_sym_search_extable;
static uint64_t ipf_sym_stext;
static uint64_t ipf_sym_etext;
static uint64_t ipf_sym_console_owner;
static uint64_t ipf_sym_console_waiter;
static uint64_t ipf_sym_switch_mode_phys;
static uint64_t ipf_sym_switch_mode_virt;
static uint64_t ipf_sym_switch_mode_phys_size;
static uint64_t ipf_sym_switch_mode_virt_size;
static uint64_t ipf_sym_ia64_switch_to;
static uint64_t ipf_sym_ia64_switch_to_size;
static uint64_t ipf_sym_load_switch_stack;
static uint64_t ipf_sym_load_switch_stack_size;

static void ipf_kernel_sym_cb(const char *st_name, int st_info,
                              uint64_t st_value, uint64_t st_size)
{
    (void)st_info;
    (void)st_size;

    if (!ipf_sym_io_space && st_name && strcmp(st_name, "io_space") == 0) {
        ipf_sym_io_space = st_value;
    }
    if (!ipf_sym_ia64_bad_break && st_name &&
        strcmp(st_name, "ia64_bad_break") == 0) {
        ipf_sym_ia64_bad_break = st_value;
    }
    if (!ipf_sym_search_extable && st_name &&
        strcmp(st_name, "search_extable") == 0) {
        ipf_sym_search_extable = st_value;
    }
    if (!ipf_sym_stext && st_name && strcmp(st_name, "_stext") == 0) {
        ipf_sym_stext = st_value;
    }
    if (!ipf_sym_etext && st_name && strcmp(st_name, "_etext") == 0) {
        ipf_sym_etext = st_value;
    }
    if (!ipf_sym_console_owner && st_name &&
        strcmp(st_name, "console_owner") == 0) {
        ipf_sym_console_owner = st_value;
    }
    if (!ipf_sym_console_waiter && st_name &&
        strcmp(st_name, "console_waiter") == 0) {
        ipf_sym_console_waiter = st_value;
    }
    if (!ipf_sym_switch_mode_phys && st_name &&
        strcmp(st_name, "ia64_switch_mode_phys") == 0) {
        ipf_sym_switch_mode_phys = st_value;
        ipf_sym_switch_mode_phys_size = st_size;
    }
    if (!ipf_sym_switch_mode_virt && st_name &&
        strcmp(st_name, "ia64_switch_mode_virt") == 0) {
        ipf_sym_switch_mode_virt = st_value;
        ipf_sym_switch_mode_virt_size = st_size;
    }
    if (!ipf_sym_ia64_switch_to && st_name &&
        strcmp(st_name, "ia64_switch_to") == 0) {
        ipf_sym_ia64_switch_to = st_value;
        ipf_sym_ia64_switch_to_size = st_size;
    }
    if (!ipf_sym_load_switch_stack && st_name &&
        strcmp(st_name, "load_switch_stack") == 0) {
        ipf_sym_load_switch_stack = st_value;
        ipf_sym_load_switch_stack_size = st_size;
    }
}

static void ipf_patch_io_space(uint64_t io_space_va, uint64_t kernel_bias,
                               uint64_t ram_size)
{
    /*
     * Linux/ia64 early serial console uses io_space[] to translate the
     * simulator-style 32-bit I/O encoding (segment:offset) into a region-6
     * uncached physical mapping. On real platforms this is initialized by
     * firmware/platform code; for bringup, seed segment 0xff to the GFW window
     * so 0xff5e0000 becomes 0xc0000000ff5e0000 and hits our serial-mm UART.
     */
    if ((io_space_va >> 61) != 5) {
        DPRINTF("io_space: unexpected VA 0x%016" PRIx64 ", skipping\n",
                io_space_va);
        return;
    }

    uint64_t io_space_pa = io_space_va - kernel_bias;
    if (io_space_pa + 0x1000 > ram_size) {
        DPRINTF("io_space: PA 0x%016" PRIx64 " out of RAM, skipping\n",
                io_space_pa);
        return;
    }

    struct QEMU_PACKED {
        uint64_t base;
        uint32_t flags;
        uint32_t pad;
    } entry = {
        .base = (6ULL << 61) | 0x00000000ff000000ULL,
        .flags = 0,
        .pad = 0,
    };

    hwaddr slot = io_space_pa + 0xff * sizeof(entry);
    address_space_write(&address_space_memory, slot, MEMTXATTRS_UNSPECIFIED,
                        (const uint8_t *)&entry, sizeof(entry));
    DPRINTF("io_space: seeded seg 0xff at PA 0x%016" HWADDR_PRIx "\n", slot);
}

static void ipf_probe_percpu_segment(const char *kernel_filename, IA64CPU *cpu)
{
    CPUIA64State *env = &cpu->env;
    int fd = open(kernel_filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        return;
    }

    Elf64_Ehdr ehdr;
    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        close(fd);
        return;
    }
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr.e_phoff == 0 || ehdr.e_phnum == 0 ||
        ehdr.e_phentsize < sizeof(Elf64_Phdr)) {
        close(fd);
        return;
    }

    bool found = false;
    uint64_t best_vaddr = 0;
    uint64_t best_paddr = 0;
    uint64_t best_memsz = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        off_t off = ehdr.e_phoff + (off_t)i * ehdr.e_phentsize;
        if (pread(fd, &phdr, sizeof(phdr), off) != sizeof(phdr)) {
            break;
        }
        if (phdr.p_type != PT_LOAD) {
            continue;
        }
        if (!(phdr.p_flags & PF_W)) {
            continue;
        }
        if ((phdr.p_vaddr >> 61) != 7 || ((phdr.p_vaddr >> 60) & 1) == 0) {
            continue;
        }
        if (phdr.p_memsz == 0) {
            continue;
        }
        if (!found || phdr.p_vaddr > best_vaddr) {
            best_vaddr = phdr.p_vaddr;
            best_paddr = phdr.p_paddr;
            best_memsz = phdr.p_memsz;
            found = true;
        }
    }
    close(fd);

    if (!found) {
        return;
    }

    env->percpu_va_base = best_vaddr;
    env->percpu_pa_base = best_paddr;
    env->percpu_size = QEMU_ALIGN_UP(best_memsz, (1ULL << TARGET_PAGE_BITS));
    DPRINTF("percpu: VA=0x%016" PRIx64 " PA=0x%016" PRIx64 " size=0x%" PRIx64 "\n",
            env->percpu_va_base, env->percpu_pa_base, env->percpu_size);
}

#define FW_FILENAME "Flash.fd"

/* Leave a chunk of memory at the top of RAM for the BIOS ACPI tables.  */
#define ACPI_DATA_SIZE       0x10000

/* IA-64 simulator-style UART base (see Linux drivers/tty/serial/8250/8250_early.c). */
#define IPF_UART_BASE 0x00000000ff5e0000ULL

/* ACPI PM1/PMTMR register block (I/O port encoded to segment 0xff). */
/*
 * Keep below the loaded Flash.fd image (max 12MiB) and outside the region
 * used by the xenipf firmware for its early RSE backing store (0xff300000+).
 */
#define IPF_ACPI_PM_BASE 0x00000000ff100000ULL

/*
 * Legacy port-I/O space window.
 *
 * Linux/ia64 represents legacy I/O ports as memory accesses into a dedicated
 * "I/O port space" range described by EFI_MEMORY_MAPPED_IO_PORT_SPACE.
 *
 * Historically QEMU's IPF machine mapped this at 0xE0000000 for 64MB and
 * translated offsets back into ioport numbers.
 */
#define IPF_LEGACY_IO_BASE 0x00000000e0000000ULL
#define IPF_LEGACY_IO_SIZE (64ULL * 1024 * 1024)

/* IA-64 IOSAPIC base used by Linux/ia64 (see asm/iosapic.h). */
#define IPF_IOSAPIC_BASE 0x00000000fec00000ULL
#define IPF_IOSAPIC_SIZE 0x00001000ULL

#define IPF_IOSAPIC_REG_SELECT 0x0
#define IPF_IOSAPIC_WINDOW     0x10
#define IPF_IOSAPIC_EOI        0x40

/*
 * Xen-style VGA hole compensation.
 *
 * The Xen/KVM IA-64 guest firmware (and our HOB builder in hw/ia64/gfw.c)
 * accounts for the legacy VGA window at 0xa0000..0xc0000 by reporting RAM as
 * if it extends past the user-requested size by the hole size. When a VGA
 * device overlays that window, this extra region restores the effective RAM
 * capacity back to the requested value.
 */
#define IPF_VGA_HOLE_START 0x000a0000ULL
#define IPF_VGA_HOLE_SIZE  0x00020000ULL
#define IPF_FW_SLACK_SIZE  (64ULL << 20)

/*
 * Firmware work RAM.
 *
 * The xenipf firmware keeps some global data just above the 4GiB boundary
 * (e.g. GP values around 0x1000_0000_0xxx). Provide a small RAM window there
 * so PEI can initialize and dispatch modules without silently reading zeros
 * from unmapped space.
 */
#define IPF_FW_WORKRAM_BASE 0x0000000100000000ULL
#define IPF_FW_WORKRAM_SIZE (16ULL << 20)

#define IPF_FW_PEI_HANDOFF_BASE (IPF_FW_WORKRAM_BASE + 0x1000)
#define IPF_FW_PEI_PPI_BASE (IPF_FW_WORKRAM_BASE + 0x2000)
#define IPF_FW_PEI_STUB_BASE (IPF_FW_WORKRAM_BASE + 0x3000)
/*
 * xenipf SEC stack setup uses 0x04000000 as the temporary RAM base when
 * ar.k3 is 3. Keep the PEI temp RAM/HOB list in that window so PEI HOBs
 * don't clobber the Xen GFW HOB list at 0xff200000.
 */
#define IPF_FW_PEI_TEMP_BASE 0x0000000004000000ULL
#define IPF_FW_PEI_TEMP_SIZE (2ULL << 20)

#define IPF_IOSAPIC_VERSION_REG 0x1

typedef struct QEMU_PACKED {
    uint64_t signature;
    uint32_t type;
    uint32_t length;
} IPFGfwHobHeader;

typedef struct QEMU_PACKED {
    IPFGfwHobHeader header;
    uint64_t length;
    uint64_t cur_pos;
    uint64_t buf_size;
} IPFGfwHobInfo;

typedef struct QEMU_PACKED {
    uint64_t start;
    uint64_t size;
} IPFGfwHobMem;

enum {
    IPF_HOB_TYPE_INFO = 0,
    IPF_HOB_TYPE_TERMINAL,
    IPF_HOB_TYPE_MEM,
    IPF_HOB_TYPE_PAL_BUS_GET_FEATURES_DATA,
    IPF_HOB_TYPE_PAL_CACHE_SUMMARY,
    IPF_HOB_TYPE_PAL_MEM_ATTRIB,
    IPF_HOB_TYPE_PAL_CACHE_INFO,
    IPF_HOB_TYPE_PAL_CACHE_PROT_INFO,
    IPF_HOB_TYPE_PAL_DEBUG_INFO,
    IPF_HOB_TYPE_PAL_FIXED_ADDR,
    IPF_HOB_TYPE_PAL_FREQ_BASE,
    IPF_HOB_TYPE_PAL_FREQ_RATIOS,
    IPF_HOB_TYPE_PAL_HALT_INFO,
    IPF_HOB_TYPE_PAL_PERF_MON_INFO,
    IPF_HOB_TYPE_PAL_PROC_GET_FEATURES,
    IPF_HOB_TYPE_PAL_PTCE_INFO,
    IPF_HOB_TYPE_PAL_REGISTER_INFO,
    IPF_HOB_TYPE_PAL_RSE_INFO,
    IPF_HOB_TYPE_PAL_TEST_INFO,
    IPF_HOB_TYPE_PAL_VM_SUMMARY,
    IPF_HOB_TYPE_PAL_VM_INFO,
    IPF_HOB_TYPE_PAL_VM_PAGE_SIZE,
    IPF_HOB_TYPE_NR_VCPU,
    IPF_HOB_TYPE_NR_NVRAM,
    IPF_HOB_TYPE_MAX
};

static const char *ipf_gfw_hob_type_name(uint32_t type)
{
    switch (type) {
    case IPF_HOB_TYPE_INFO: return "INFO";
    case IPF_HOB_TYPE_TERMINAL: return "TERMINAL";
    case IPF_HOB_TYPE_MEM: return "MEM";
    case IPF_HOB_TYPE_PAL_BUS_GET_FEATURES_DATA: return "PAL_BUS_FEATURES";
    case IPF_HOB_TYPE_PAL_CACHE_SUMMARY: return "PAL_CACHE_SUMMARY";
    case IPF_HOB_TYPE_PAL_MEM_ATTRIB: return "PAL_MEM_ATTRIB";
    case IPF_HOB_TYPE_PAL_CACHE_INFO: return "PAL_CACHE_INFO";
    case IPF_HOB_TYPE_PAL_CACHE_PROT_INFO: return "PAL_CACHE_PROT_INFO";
    case IPF_HOB_TYPE_PAL_DEBUG_INFO: return "PAL_DEBUG_INFO";
    case IPF_HOB_TYPE_PAL_FIXED_ADDR: return "PAL_FIXED_ADDR";
    case IPF_HOB_TYPE_PAL_FREQ_BASE: return "PAL_FREQ_BASE";
    case IPF_HOB_TYPE_PAL_FREQ_RATIOS: return "PAL_FREQ_RATIOS";
    case IPF_HOB_TYPE_PAL_HALT_INFO: return "PAL_HALT_INFO";
    case IPF_HOB_TYPE_PAL_PERF_MON_INFO: return "PAL_PERF_MON_INFO";
    case IPF_HOB_TYPE_PAL_PROC_GET_FEATURES: return "PAL_PROC_FEATURES";
    case IPF_HOB_TYPE_PAL_PTCE_INFO: return "PAL_PTCE_INFO";
    case IPF_HOB_TYPE_PAL_REGISTER_INFO: return "PAL_REGISTER_INFO";
    case IPF_HOB_TYPE_PAL_RSE_INFO: return "PAL_RSE_INFO";
    case IPF_HOB_TYPE_PAL_TEST_INFO: return "PAL_TEST_INFO";
    case IPF_HOB_TYPE_PAL_VM_SUMMARY: return "PAL_VM_SUMMARY";
    case IPF_HOB_TYPE_PAL_VM_INFO: return "PAL_VM_INFO";
    case IPF_HOB_TYPE_PAL_VM_PAGE_SIZE: return "PAL_VM_PAGE_SIZE";
    case IPF_HOB_TYPE_NR_VCPU: return "NR_VCPU";
    case IPF_HOB_TYPE_NR_NVRAM: return "NR_NVRAM";
    case IPF_HOB_TYPE_MAX: return "MAX";
    default: return "UNKNOWN";
    }
}

static void ipf_dump_gfw_hob(const char *tag)
{
    const char *dump_env = getenv("QEMU_IPF_DUMP_HOB");
    if (!dump_env || !*dump_env) {
        return;
    }

    IPFGfwHobInfo info;
    if (address_space_read(&address_space_memory, GFW_HOB_START,
                           MEMTXATTRS_UNSPECIFIED, &info,
                           sizeof(info)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: HOB dump: read failed at 0x%016" PRIx64 "\n",
                      (uint64_t)GFW_HOB_START);
        return;
    }

    uint64_t sig = le64_to_cpu(info.header.signature);
    uint32_t type = le32_to_cpu(info.header.type);
    uint32_t hdr_len = le32_to_cpu(info.header.length);
    uint64_t hob_len = le64_to_cpu(info.length);
    uint64_t hob_buf_size = le64_to_cpu(info.buf_size);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IPF: HOB dump(%s): sig=%016" PRIx64 " type=%u len=%" PRIu64
                  " hdr_len=%u buf_size=%" PRIu64 " base=0x%016" PRIx64 "\n",
                  tag ? tag : "boot", sig, type, hob_len, hdr_len, hob_buf_size,
                  (uint64_t)GFW_HOB_START);

    if (sig != HOB_SIGNATURE || hob_len == 0 || hob_len > GFW_HOB_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: HOB dump: invalid header (sig/len)\n");
        return;
    }

    g_autofree uint8_t *buf = g_malloc((size_t)hob_len);
    if (address_space_read(&address_space_memory, GFW_HOB_START,
                           MEMTXATTRS_UNSPECIFIED, buf,
                           (size_t)hob_len) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: HOB dump: bulk read failed\n");
        return;
    }

    g_mkdir_with_parents("scratch/ia64_logs", 0755);
    char path[256];
    snprintf(path, sizeof(path),
             "scratch/ia64_logs/gfw_hob_%s.bin",
             tag ? tag : "boot");
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fwrite(buf, 1, (size_t)hob_len, fp);
        fclose(fp);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: HOB dump: wrote %s (%" PRIu64 " bytes)\n",
                      path, hob_len);
    }

    uint64_t off = 0;
    while (off + sizeof(IPFGfwHobHeader) <= hob_len) {
        const IPFGfwHobHeader *hdr = (const IPFGfwHobHeader *)(buf + off);
        uint64_t hs = le64_to_cpu(hdr->signature);
        uint32_t ht = le32_to_cpu(hdr->type);
        uint32_t hl = le32_to_cpu(hdr->length);
        if (hs != HOB_SIGNATURE || hl < sizeof(IPFGfwHobHeader)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IPF: HOB dump: bad entry off=0x%04" PRIx64
                          " sig=%016" PRIx64 " type=%u len=%u\n",
                          off, hs, ht, hl);
            break;
        }

        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: HOB[%02" PRIu64 "] type=%u (%s) len=%u\n",
                      off, ht, ipf_gfw_hob_type_name(ht), hl);

        if (ht == IPF_HOB_TYPE_MEM && hl >= sizeof(IPFGfwHobHeader) +
                                         sizeof(IPFGfwHobMem)) {
            const IPFGfwHobMem *mem = (const IPFGfwHobMem *)(buf + off +
                                                            sizeof(IPFGfwHobHeader));
            uint64_t start = le64_to_cpu(mem->start);
            uint64_t size = le64_to_cpu(mem->size);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IPF: HOB MEM start=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
                          start, size);
        } else if (ht == IPF_HOB_TYPE_NR_VCPU ||
                   ht == IPF_HOB_TYPE_NR_NVRAM ||
                   ht == IPF_HOB_TYPE_MAX) {
            if (hl >= sizeof(IPFGfwHobHeader) + sizeof(uint64_t)) {
                uint64_t val = ldq_le_p(buf + off + sizeof(IPFGfwHobHeader));
                qemu_log_mask(LOG_GUEST_ERROR,
                              "IPF: HOB %s value=0x%016" PRIx64 "\n",
                              ipf_gfw_hob_type_name(ht), val);
            }
        }

        off += hl;
        if (ht == IPF_HOB_TYPE_TERMINAL) {
            break;
        }
    }
}

static void ipf_log_dxe_status(IPFMachineState *m, const char *tag)
{
    if (!m || !m->cpu || !qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }
    CPUIA64State *env = &m->cpu->env;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "IPF: DXE_STATUS(%s) ip=%016" PRIx64 " psr=%016" PRIx64
                  " r8=%016" PRIx64 " r9=%016" PRIx64
                  " r10=%016" PRIx64 " r11=%016" PRIx64
                  " r28=%016" PRIx64 " r29=%016" PRIx64 "\n",
                  tag ? tag : "line",
                  env->ip, env->psr,
                  env->r[8], env->r[9], env->r[10], env->r[11],
                  env->r[28], env->r[29]);
}

static void ipf_fill_fw_window_erased(void)
{
    g_autofree uint8_t *buf = g_malloc(64 * 1024);
    memset(buf, 0xff, 64 * 1024);

    for (hwaddr off = 0; off < GFW_SIZE; off += 64 * 1024) {
        size_t len = MIN((size_t)(GFW_SIZE - off), (size_t)(64 * 1024));
        address_space_write(&address_space_memory, GFW_START + off,
                            MEMTXATTRS_UNSPECIFIED, buf, len);
    }
}

/*
 * Firmware call-gates / stubs.
 *
 * The xenipf/EDK firmware uses a status-code reporter very early during PEI
 * memory services init. In some builds the reporter function pointer and
 * CallerId GUID pointer are sourced from GP-relative indirections into the
 * variable FV region (0xffe0....). When those entries are left in the erased
 * (0xff) state, the firmware attempts to call through a NULL/garbage plabel
 * and crashes before it can initialize devices like VGA.
 *
 * Provide a tiny IA-64 stub function that executes break(0) so the existing
 * fw_break0 helper can consume the status-code arguments and return to b0.
 * Patch the early global pointers if they appear uninitialized.
 */

#define IPF_FW_STATUS_CALLER_ID_ADDR      0x00000000ffe00076ULL
#define IPF_FW_STATUS_REPORT_PLABEL_ADDR  0x00000000ffe011b6ULL

/* Firmware volume / file type values from EDK1 headers. */
#define EFI_FVH_SIGNATURE                  0x4856465fU /* "_FVH" */
#define EFI_FV_FILETYPE_PEI_CORE           0x04
#define EFI_FV_FILETYPE_DXE_CORE           0x05
#define EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE 0x0B
#define EFI_FFS_FILE_HEADER_SIZE           24
#define EFI_FFS_FILE_HEADER2_SIZE          32

static bool ipf_fw_scan_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *s = getenv("QEMU_IPF_FW_SCAN");
        enabled = (s && *s) ? 1 : 0;
    }
    return enabled;
}

static bool ipf_fw_pei_use_pi_handoff(void)
{
    static int use_pi = -1;
    if (use_pi == -1) {
        const char *s = getenv("QEMU_IPF_FW_PEI_PI");
        if (s && *s) {
            use_pi = (strcmp(s, "0") == 0 || strcmp(s, "false") == 0 ||
                      strcmp(s, "no") == 0) ? 0 : 1;
        } else {
            use_pi = 1;
        }
    }
    return use_pi;
}

static bool ipf_fw_is_erased(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0xff) {
            return false;
        }
    }
    return true;
}

static size_t ipf_fw_align_up(size_t val, size_t align)
{
    if (align == 0) {
        return val;
    }
    return (val + align - 1) & ~(align - 1);
}

static uint64_t ipf_fw_region8_addr(uint64_t phys)
{
    /*
     * The xenipf/EDK PEI core performs arithmetic on the handoff pointers.
     * Keep them in region 0 (physical) so shifts don't propagate sign bits.
     */
    static int region = -1;
    if (region == -1) {
        const char *s = getenv("QEMU_IPF_FW_REGION");
        if (s && *s) {
            region = (int)strtol(s, NULL, 0);
        } else {
            region = 0;
        }
        if (region < 0 || region > 7) {
            warn_report("IPF: invalid QEMU_IPF_FW_REGION=%d, using 0", region);
            region = 0;
        }
    }
    if (region == 0) {
        return phys;
    }
    return ((uint64_t)region << 61) | (phys & ((1ULL << 61) - 1));
}

static uint64_t ipf_fw_boot_r10_count(void)
{
    /*
     * The firmware stack/BSP setup at 0xffe2e630 walks forward from a fixed
     * base (0xff300000) in 128KiB steps using ar.k4 (boot r10). The firmware
     * uses 0x80 (16MiB/128KiB) in its later paths, so match that sizing.
     */
    const uint64_t stride = 0x20000ULL;
    if ((IPF_FW_WORKRAM_SIZE % stride) != 0) {
        error_report("IPF: fw-workram size not aligned to 128KiB");
        exit(EXIT_FAILURE);
    }
    const uint64_t count = IPF_FW_WORKRAM_SIZE / stride;
    if (count == 0) {
        error_report("IPF: fw-workram size too small for firmware stack");
        exit(EXIT_FAILURE);
    }
    return count;
}

static void ipf_fw_guid_to_str(char *out, size_t out_len, const uint8_t *guid)
{
    uint32_t d1 = ldl_le_p(&guid[0]);
    uint16_t d2 = lduw_le_p(&guid[4]);
    uint16_t d3 = lduw_le_p(&guid[6]);
    snprintf(out, out_len,
             "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             d1, d2, d3,
             guid[8], guid[9], guid[10], guid[11],
             guid[12], guid[13], guid[14], guid[15]);
}

static void ipf_fw_scan_fv_files(const uint8_t *buf, size_t size,
                                 size_t fv_base, size_t fv_len,
                                 size_t fv_hdr_len, hwaddr fw_base,
                                 hwaddr *min_phys, hwaddr *max_phys)
{
    size_t fv_end = fv_base + fv_len;
    size_t off = fv_base + fv_hdr_len;
    int files = 0;
    int dxe_cores = 0;
    int fv_images = 0;

    hwaddr fv_phys = fw_base + fv_base;
    hwaddr fv_phys_end = fv_phys + fv_len;
    if (min_phys && fv_phys < *min_phys) {
        *min_phys = fv_phys;
    }
    if (max_phys && fv_phys_end > *max_phys) {
        *max_phys = fv_phys_end;
    }

    while (off + EFI_FFS_FILE_HEADER_SIZE <= fv_end && off + 16 <= size) {
        const uint8_t *fh = &buf[off];
        if (ipf_fw_is_erased(fh, EFI_FFS_FILE_HEADER_SIZE)) {
            break;
        }

        uint8_t type = fh[18];
        uint32_t size24 = (uint32_t)fh[20] |
                          ((uint32_t)fh[21] << 8) |
                          ((uint32_t)fh[22] << 16);
        uint64_t fsize = size24;
        size_t hdr_size = EFI_FFS_FILE_HEADER_SIZE;

        if (size24 == 0xffffff) {
            if (EFI_FFS_FILE_HEADER2_SIZE > fv_end - off) {
                break;
            }
            fsize = ldq_le_p(&fh[24]);
            hdr_size = EFI_FFS_FILE_HEADER2_SIZE;
        }
        if (fsize < hdr_size || fsize > fv_end - off) {
            break;
        }

        if (type == EFI_FV_FILETYPE_DXE_CORE ||
            type == EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE) {
            char guid[48];
            ipf_fw_guid_to_str(guid, sizeof(guid), fh);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IPF: FW scan: FFS type=%02x size=0x%" PRIx64
                          " off=0x%zx phys=%016" HWADDR_PRIx " guid=%s\n",
                          type, fsize, off, fw_base + off, guid);
        }
        if (type == EFI_FV_FILETYPE_DXE_CORE) {
            dxe_cores++;
        } else if (type == EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE) {
            fv_images++;
        }

        files++;
        size_t advance = ipf_fw_align_up((size_t)fsize, 8);
        if (advance == 0 || advance > fv_end - off) {
            break;
        }
        off += advance;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IPF: FW scan: FV off=0x%zx phys=%016" HWADDR_PRIx
                  " len=0x%zx hdr=0x%zx files=%d dxe=%d fvimg=%d\n",
                  fv_base, fw_base + fv_base, fv_len, fv_hdr_len,
                  files, dxe_cores, fv_images);
}

static void ipf_fw_scan_firmware(const uint8_t *buf, size_t size,
                                 hwaddr fw_base)
{
    size_t fv_count = 0;
    hwaddr min_phys = UINT64_MAX;
    hwaddr max_phys = 0;
    for (size_t base = 0; base + 0x38 <= size; base += 0x10) {
        if (ldl_le_p(&buf[base + 0x28]) != EFI_FVH_SIGNATURE) {
            continue;
        }
        uint64_t fv_len = ldq_le_p(&buf[base + 0x20]);
        uint16_t hdr_len = lduw_le_p(&buf[base + 0x30]);
        if (fv_len < 0x38 || fv_len > (size - base)) {
            continue;
        }
        if (hdr_len < 0x38 || hdr_len > fv_len) {
            continue;
        }

        ipf_fw_scan_fv_files(buf, size, base, (size_t)fv_len,
                             (size_t)hdr_len, fw_base,
                             &min_phys, &max_phys);
        fv_count++;

        if (fv_len > 0x10) {
            base += (size_t)fv_len - 0x10;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IPF: FW scan: total FVs=%zu size=0x%zx base=%016" HWADDR_PRIx "\n",
                  fv_count, size, fw_base);
    if (fv_count && min_phys != UINT64_MAX) {
        hwaddr flash_lo = GFW_START;
        hwaddr flash_hi = GFW_START + GFW_SIZE;
        hwaddr max_phys_inc = max_phys ? (max_phys - 1) : max_phys;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: FW scan: FV phys range=%016" HWADDR_PRIx
                      "..%016" HWADDR_PRIx " flash=%016" HWADDR_PRIx
                      "..%016" HWADDR_PRIx "\n",
                      min_phys, max_phys_inc, flash_lo, flash_hi - 1);
        if (min_phys < flash_lo || max_phys > flash_hi) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "IPF: FW scan: FV range exceeds flash window\n");
        }
    }
}

static bool ipf_fw_find_dxe_core(const uint8_t *buf, size_t size,
                                 size_t *off_out, uint64_t *size_out)
{
    for (size_t base = 0; base + 0x38 <= size; base += 0x10) {
        if (ldl_le_p(&buf[base + 0x28]) != EFI_FVH_SIGNATURE) {
            continue;
        }
        uint64_t fv_len = ldq_le_p(&buf[base + 0x20]);
        uint16_t hdr_len = lduw_le_p(&buf[base + 0x30]);
        if (fv_len < 0x38 || fv_len > (size - base)) {
            continue;
        }
        if (hdr_len < 0x38 || hdr_len > fv_len) {
            continue;
        }

        size_t fv_end = base + (size_t)fv_len;
        size_t off = base + (size_t)hdr_len;
        while (off + EFI_FFS_FILE_HEADER_SIZE <= fv_end && off + 16 <= size) {
            const uint8_t *fh = &buf[off];
            if (ipf_fw_is_erased(fh, EFI_FFS_FILE_HEADER_SIZE)) {
                break;
            }

            uint8_t type = fh[18];
            uint32_t size24 = (uint32_t)fh[20] |
                              ((uint32_t)fh[21] << 8) |
                              ((uint32_t)fh[22] << 16);
            uint64_t fsize = size24;
            size_t hdr_size = EFI_FFS_FILE_HEADER_SIZE;

            if (size24 == 0xffffff) {
                if (EFI_FFS_FILE_HEADER2_SIZE > fv_end - off) {
                    break;
                }
                fsize = ldq_le_p(&fh[24]);
                hdr_size = EFI_FFS_FILE_HEADER2_SIZE;
            }
            if (fsize < hdr_size || fsize > fv_end - off) {
                break;
            }

            if (type == EFI_FV_FILETYPE_DXE_CORE) {
                if (off_out) {
                    *off_out = off;
                }
                if (size_out) {
                    *size_out = fsize;
                }
                return true;
            }

            size_t advance = ipf_fw_align_up((size_t)fsize, 8);
            if (advance == 0 || advance > fv_end - off) {
                break;
            }
            off += advance;
        }
    }
    return false;
}

static bool ipf_fw_find_pei_core_fv(const uint8_t *buf, size_t size,
                                    size_t *fv_off_out, uint64_t *fv_size_out)
{
    for (size_t base = 0; base + 0x38 <= size; base += 0x10) {
        if (ldl_le_p(&buf[base + 0x28]) != EFI_FVH_SIGNATURE) {
            continue;
        }
        uint64_t fv_len = ldq_le_p(&buf[base + 0x20]);
        uint16_t hdr_len = lduw_le_p(&buf[base + 0x30]);
        if (fv_len < 0x38 || fv_len > (size - base)) {
            continue;
        }
        if (hdr_len < 0x38 || hdr_len > fv_len) {
            continue;
        }

        size_t fv_end = base + (size_t)fv_len;
        size_t off = base + (size_t)hdr_len;
        while (off + EFI_FFS_FILE_HEADER_SIZE <= fv_end && off + 16 <= size) {
            const uint8_t *fh = &buf[off];
            if (ipf_fw_is_erased(fh, EFI_FFS_FILE_HEADER_SIZE)) {
                break;
            }

            uint8_t type = fh[18];
            uint32_t size24 = (uint32_t)fh[20] |
                              ((uint32_t)fh[21] << 8) |
                              ((uint32_t)fh[22] << 16);
            uint64_t fsize = size24;
            size_t hdr_size = EFI_FFS_FILE_HEADER_SIZE;

            if (size24 == 0xffffff) {
                if (EFI_FFS_FILE_HEADER2_SIZE > fv_end - off) {
                    break;
                }
                fsize = ldq_le_p(&fh[24]);
                hdr_size = EFI_FFS_FILE_HEADER2_SIZE;
            }
            if (fsize < hdr_size || fsize > fv_end - off) {
                break;
            }

            if (type == EFI_FV_FILETYPE_PEI_CORE) {
                if (fv_off_out) {
                    *fv_off_out = base;
                }
                if (fv_size_out) {
                    *fv_size_out = fv_len;
                }
                return true;
            }

            size_t advance = ipf_fw_align_up((size_t)fsize, 8);
            if (advance == 0 || advance > fv_end - off) {
                break;
            }
            off += advance;
        }
    }
    return false;
}

static void ipf_fw_dump_dxe_core(const uint8_t *buf, size_t size,
                                 hwaddr fw_base)
{
    size_t off = 0;
    uint64_t fsize = 0;
    if (!ipf_fw_find_dxe_core(buf, size, &off, &fsize)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: FW DXE dump: DXE core not found\n");
        return;
    }
    const uint8_t *fh = &buf[off];
    char guid[48];
    ipf_fw_guid_to_str(guid, sizeof(guid), fh);
    uint8_t type = fh[18];
    uint8_t attr = fh[19];
    uint8_t state = fh[23];
    uint32_t size24 = (uint32_t)fh[20] |
                      ((uint32_t)fh[21] << 8) |
                      ((uint32_t)fh[22] << 16);
    uint64_t ext_size = 0;
    bool ext = (size24 == 0xffffff);
    if (ext && off + EFI_FFS_FILE_HEADER2_SIZE <= size) {
        ext_size = ldq_le_p(&fh[24]);
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "IPF: FW DXE dump: off=0x%zx phys=%016" HWADDR_PRIx
                  " type=%02x attr=%02x state=%02x size=0x%" PRIx64
                  " ext=%d guid=%s\n",
                  off, fw_base + off, type, attr, state,
                  ext ? ext_size : (uint64_t)size24, ext ? 1 : 0, guid);

    g_mkdir_with_parents("scratch/ia64_logs", 0755);
    char path[256];
    snprintf(path, sizeof(path),
             "scratch/ia64_logs/fw_dxe_core_header_%016" HWADDR_PRIx ".bin",
             fw_base + off);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        size_t dump_len = MIN((size_t)64, size - off);
        fwrite(fh, 1, dump_len, fp);
        fclose(fp);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: FW DXE dump: wrote %s (%zu bytes)\n",
                      path, dump_len);
    }
}

static void ipf_patch_firmware_statuscode_callgate(void)
{
    /*
     * IA-64 bundle:
     *   [MII] break.m 0
     *         nop.i 0
     *         nop.i 0
     *
     * Assembled with ia64-suse-linux-as for reproducibility.
     */
    static const uint8_t break0_bundle[16] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    };

    uint64_t caller_id_raw = 0;
    MemTxResult caller_res =
        address_space_read(&address_space_memory, IPF_FW_STATUS_CALLER_ID_ADDR,
                           MEMTXATTRS_UNSPECIFIED, &caller_id_raw,
                           sizeof(caller_id_raw));
    uint64_t caller_id = le64_to_cpu(caller_id_raw);

    uint64_t report_plabel_ptr_raw = 0;
    MemTxResult report_res =
        address_space_read(&address_space_memory,
                           IPF_FW_STATUS_REPORT_PLABEL_ADDR,
                           MEMTXATTRS_UNSPECIFIED, &report_plabel_ptr_raw,
                           sizeof(report_plabel_ptr_raw));
    uint64_t report_plabel_ptr = le64_to_cpu(report_plabel_ptr_raw);

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: firmware status-code prepatch: caller_id=%016" PRIx64
                      " report_plabel_ptr=%016" PRIx64
                      " (caller_res=%d report_res=%d)\n",
                      caller_id, report_plabel_ptr, (int)caller_res,
                      (int)report_res);
    }

    if (caller_res != MEMTX_OK || report_res != MEMTX_OK) {
        return;
    }

    if (caller_id != UINT64_MAX && report_plabel_ptr != UINT64_MAX) {
        return;
    }

    /*
     * Reserve a small scratch area at the end of the firmware work RAM.
     * Keep it self-contained so we can patch the early indirections without
     * requiring symbol information from the guest firmware.
     */
    hwaddr scratch = (IPF_FW_WORKRAM_BASE + IPF_FW_WORKRAM_SIZE) - 0x1000;
    scratch &= ~0xFULL;

    hwaddr stub_code = scratch;
    hwaddr stub_plabel = scratch + 0x20;
    hwaddr stub_object = scratch + 0x40;

    /* Write stub code. */
    address_space_write(&address_space_memory, stub_code,
                        MEMTXATTRS_UNSPECIFIED, break0_bundle,
                        sizeof(break0_bundle));
    cpu_flush_icache_range(stub_code, sizeof(break0_bundle));

    /* Function descriptor (plabel): { entry, gp }. */
    struct QEMU_PACKED {
        uint64_t entry;
        uint64_t gp;
    } plabel = {
        .entry = cpu_to_le64((uint64_t)stub_code),
        .gp = cpu_to_le64(0),
    };
    address_space_write(&address_space_memory, stub_plabel,
                        MEMTXATTRS_UNSPECIFIED, (const uint8_t *)&plabel,
                        sizeof(plabel));

    /* Object with a function pointer at offset 0x70 (112). */
    uint8_t obj[0x80];
    memset(obj, 0, sizeof(obj));
    stq_le_p(&obj[0x70], (uint64_t)stub_plabel);
    address_space_write(&address_space_memory, stub_object,
                        MEMTXATTRS_UNSPECIFIED, obj, sizeof(obj));

    /*
     * Patch early globals:
     * - CallerId: default to NULL
     * - ReportStatusCode: point at our stub object
     *
     * Only patch if the firmware left them in the erased (all-0xff) state.
     */
    if (caller_id == UINT64_MAX) {
        uint64_t zero = 0;
        address_space_write(&address_space_memory, IPF_FW_STATUS_CALLER_ID_ADDR,
                            MEMTXATTRS_UNSPECIFIED, &zero, sizeof(zero));
    }

    if (report_plabel_ptr == UINT64_MAX) {
        uint64_t g = cpu_to_le64((uint64_t)stub_object);
        address_space_write(&address_space_memory,
                            IPF_FW_STATUS_REPORT_PLABEL_ADDR,
                            MEMTXATTRS_UNSPECIFIED, &g, sizeof(g));
    }

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: firmware status-code callgate patched: caller_id=%016" PRIx64
                      " report_plabel_ptr=%016" PRIx64 " stub_code=%016" HWADDR_PRIx
                      " stub_plabel=%016" HWADDR_PRIx " stub_object=%016" HWADDR_PRIx "\n",
                      caller_id, report_plabel_ptr,
                      stub_code, stub_plabel, stub_object);
    }
}

static void ipf_fw_setup_pei_handoff(const uint8_t *buf, size_t size,
                                     hwaddr fw_base)
{
    size_t fv_off = 0;
    uint64_t fv_len = 0;
    if (!ipf_fw_find_pei_core_fv(buf, size, &fv_off, &fv_len)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: PEI handoff: PEI core FV not found; using firmware base\n");
        fv_off = 0;
        fv_len = size;
    }

    const uint64_t bfv_phys = (uint64_t)fw_base + fv_off;
    const uint64_t bfv_size = fv_len;
    const uint64_t temp_phys = IPF_FW_PEI_TEMP_BASE;
    const uint64_t temp_size = IPF_FW_PEI_TEMP_SIZE;
    const uint64_t handoff_phys = ipf_fw_pei_use_pi_handoff() ?
                                  IPF_FW_PEI_HANDOFF_BASE :
                                  (temp_phys + 0x1000);
    const uint64_t ppi_phys = temp_phys;
    const uint64_t stub_phys = IPF_FW_PEI_STUB_BASE;
    const uint64_t plabel_phys = stub_phys + 0x20;
    const uint64_t ppi_iface_phys = stub_phys + 0x40;
    const uint64_t findfv_stub_phys = stub_phys + 0x60;
    const uint64_t findfv_plabel_phys = findfv_stub_phys + 0x20;
    const uint64_t findfv_iface_phys = findfv_stub_phys + 0x40;
    const uint64_t secinfo_stub_phys = findfv_stub_phys + 0x60;
    const uint64_t secinfo_plabel_phys = secinfo_stub_phys + 0x20;
    const uint64_t secinfo_iface_phys = secinfo_stub_phys + 0x40;
    const uint64_t memmap_stub_phys = secinfo_stub_phys + 0x60;
    const uint64_t memmap_plabel_phys = memmap_stub_phys + 0x20;
    const uint64_t memmap_iface_phys = memmap_stub_phys + 0x40;
    const uint64_t pei_temp_size = temp_size / 2;
    const uint64_t stack_size = temp_size - pei_temp_size;
    const uint64_t stack_base = temp_phys + temp_size;

    static const uint8_t status_stub[16] = {
        0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
        0x00, 0x00, 0x42, 0x80, 0x00, 0x00, 0x84, 0x00,
    };
    static const uint8_t status_guid[16] = {
        0xd3, 0x32, 0x98, 0x22, 0x30, 0x7a, 0x36, 0x4b,
        0xb8, 0x27, 0xf4, 0x0c, 0xb7, 0xd4, 0x54, 0x36,
    };
    static const uint8_t findfv_stub[16] = {
        0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x02, 0x00, 0x80, 0x08, 0x00, 0x84, 0x00,
    };
    static const uint8_t findfv_guid[16] = {
        0x12, 0x48, 0x16, 0x36, 0x23, 0xa0, 0xe5, 0x44,
        0xbd, 0x85, 0x05, 0xbf, 0x3c, 0x77, 0x00, 0xaa,
    };
    static const uint8_t secinfo_guid[16] = {
        0x35, 0x2b, 0x8c, 0x6f, 0xf4, 0xfe, 0x8d, 0x44,
        0x82, 0x56, 0xe1, 0x1b, 0x19, 0xd6, 0x10, 0x77,
    };
    static const uint8_t memmap_guid[16] = {
        0x8a, 0x59, 0xb1, 0xd0, 0x66, 0xee, 0xd5, 0x11,
        0xaf, 0x1d, 0x00, 0xa0, 0xc9, 0x44, 0xa0, 0x5b,
    };

    if (ipf_fw_pei_use_pi_handoff()) {
        /*
         * EFI_SEC_PEI_HAND_OFF (PI PEI core):
         *   0x00 DataSize (UINT16)
         *   0x08 BootFirmwareVolumeBase
         *   0x10 BootFirmwareVolumeSize
         *   0x18 TemporaryRamBase
         *   0x20 TemporaryRamSize
         *   0x28 PeiTemporaryRamBase
         *   0x30 PeiTemporaryRamSize
         *   0x38 StackBase
         *   0x40 StackSize
         */
        uint8_t handoff[0x48];
        memset(handoff, 0, sizeof(handoff));
        stw_le_p(&handoff[0], sizeof(handoff));
        stq_le_p(&handoff[8], ipf_fw_region8_addr(bfv_phys));
        stq_le_p(&handoff[16], bfv_size);
        stq_le_p(&handoff[24], ipf_fw_region8_addr(temp_phys));
        stq_le_p(&handoff[32], temp_size);
        stq_le_p(&handoff[40], ipf_fw_region8_addr(temp_phys));
        stq_le_p(&handoff[48], pei_temp_size);
        stq_le_p(&handoff[56], ipf_fw_region8_addr(stack_base - stack_size));
        stq_le_p(&handoff[64], stack_size);
        cpu_physical_memory_write(handoff_phys, handoff, sizeof(handoff));
    } else {
        /*
         * EFI_PEI_STARTUP_DESCRIPTOR (framework/Tiano PEI core):
         *   0x00 BootFirmwareVolume
         *   0x08 SizeOfCacheAsRam
         *   0x10 DispatchTable (PPI list)
         */
        uint8_t startup[0x18];
        memset(startup, 0, sizeof(startup));
        stq_le_p(&startup[0], ipf_fw_region8_addr(bfv_phys));
        stq_le_p(&startup[8], temp_size);
        stq_le_p(&startup[16], ipf_fw_region8_addr(ppi_phys));
        cpu_physical_memory_write(handoff_phys, startup, sizeof(startup));
    }

    struct QEMU_PACKED {
        uint64_t entry;
        uint64_t gp;
    } plabel = {
        .entry = cpu_to_le64(ipf_fw_region8_addr(stub_phys)),
        .gp = 0,
    };
    cpu_physical_memory_write(stub_phys, status_stub, sizeof(status_stub));
    cpu_physical_memory_write(plabel_phys, (const uint8_t *)&plabel, sizeof(plabel));
    cpu_flush_icache_range(stub_phys, sizeof(status_stub));

    struct QEMU_PACKED {
        uint64_t entry;
        uint64_t gp;
    } findfv_plabel = {
        .entry = cpu_to_le64(ipf_fw_region8_addr(findfv_stub_phys)),
        .gp = 0,
    };
    cpu_physical_memory_write(findfv_stub_phys, findfv_stub, sizeof(findfv_stub));
    cpu_physical_memory_write(findfv_plabel_phys,
                              (const uint8_t *)&findfv_plabel,
                              sizeof(findfv_plabel));
    cpu_flush_icache_range(findfv_stub_phys, sizeof(findfv_stub));

    struct QEMU_PACKED {
        uint64_t entry;
        uint64_t gp;
    } secinfo_plabel = {
        .entry = cpu_to_le64(ipf_fw_region8_addr(secinfo_stub_phys)),
        .gp = 0,
    };
    cpu_physical_memory_write(secinfo_stub_phys, findfv_stub,
                              sizeof(findfv_stub));
    cpu_physical_memory_write(secinfo_plabel_phys,
                              (const uint8_t *)&secinfo_plabel,
                              sizeof(secinfo_plabel));
    cpu_flush_icache_range(secinfo_stub_phys, sizeof(findfv_stub));

    struct QEMU_PACKED {
        uint64_t entry;
        uint64_t gp;
    } memmap_plabel = {
        .entry = cpu_to_le64(ipf_fw_region8_addr(memmap_stub_phys)),
        .gp = 0,
    };
    cpu_physical_memory_write(memmap_stub_phys, findfv_stub,
                              sizeof(findfv_stub));
    cpu_physical_memory_write(memmap_plabel_phys,
                              (const uint8_t *)&memmap_plabel,
                              sizeof(memmap_plabel));
    cpu_flush_icache_range(memmap_stub_phys, sizeof(findfv_stub));
    /*
     * PPI interface: a single function pointer (plabel) to the status hook.
     * The descriptor points at this struct, not at the plabel itself.
     */
    uint64_t status_iface = cpu_to_le64(ipf_fw_region8_addr(plabel_phys));
    cpu_physical_memory_write(ppi_iface_phys,
                              (const uint8_t *)&status_iface,
                              sizeof(status_iface));

    uint64_t findfv_iface = cpu_to_le64(ipf_fw_region8_addr(findfv_plabel_phys));
    cpu_physical_memory_write(findfv_iface_phys,
                              (const uint8_t *)&findfv_iface,
                              sizeof(findfv_iface));
    uint64_t secinfo_iface = cpu_to_le64(ipf_fw_region8_addr(secinfo_plabel_phys));
    cpu_physical_memory_write(secinfo_iface_phys,
                              (const uint8_t *)&secinfo_iface,
                              sizeof(secinfo_iface));
    uint64_t memmap_iface = cpu_to_le64(ipf_fw_region8_addr(memmap_plabel_phys));
    cpu_physical_memory_write(memmap_iface_phys,
                              (const uint8_t *)&memmap_iface,
                              sizeof(memmap_iface));

    const uint64_t status_guid_phys = ppi_phys + 0x60;
    const uint64_t findfv_guid_phys = ppi_phys + 0x70;
    const uint64_t secinfo_guid_phys = ppi_phys + 0x80;
    const uint64_t memmap_guid_phys = ppi_phys + 0x90;
    uint8_t ppi[0xa0];
    memset(ppi, 0, sizeof(ppi));
    uint64_t flags = 0x00000010ULL; /* PPI */
    stq_le_p(&ppi[0x00], flags);
    stq_le_p(&ppi[0x08], ipf_fw_region8_addr(status_guid_phys));
    stq_le_p(&ppi[0x10], ipf_fw_region8_addr(ppi_iface_phys));
    stq_le_p(&ppi[0x18], flags);
    stq_le_p(&ppi[0x20], ipf_fw_region8_addr(findfv_guid_phys));
    stq_le_p(&ppi[0x28], ipf_fw_region8_addr(findfv_iface_phys));
    stq_le_p(&ppi[0x30], flags);
    stq_le_p(&ppi[0x38], ipf_fw_region8_addr(secinfo_guid_phys));
    stq_le_p(&ppi[0x40], ipf_fw_region8_addr(secinfo_iface_phys));
    flags = 0x80000000ULL | 0x00000010ULL; /* TERMINATE_LIST | PPI */
    stq_le_p(&ppi[0x48], flags);
    stq_le_p(&ppi[0x50], ipf_fw_region8_addr(memmap_guid_phys));
    stq_le_p(&ppi[0x58], ipf_fw_region8_addr(memmap_iface_phys));
    memcpy(&ppi[0x60], status_guid, sizeof(status_guid));
    memcpy(&ppi[0x70], findfv_guid, sizeof(findfv_guid));
    memcpy(&ppi[0x80], secinfo_guid, sizeof(secinfo_guid));
    memcpy(&ppi[0x90], memmap_guid, sizeof(memmap_guid));
    cpu_physical_memory_write(ppi_phys, ppi, sizeof(ppi));

    ipf_boot_r9 = ipf_fw_region8_addr(handoff_phys);
    ipf_boot_ppi = ipf_fw_region8_addr(ppi_phys);
    ipf_boot_r10 = ipf_fw_boot_r10_count();
    ipf_boot_findfv_stub = ipf_fw_region8_addr(findfv_stub_phys);
    ipf_boot_findfv_iface = ipf_fw_region8_addr(findfv_iface_phys);
    ipf_boot_secinfo_stub = ipf_fw_region8_addr(secinfo_stub_phys);
    ipf_boot_memmap_stub = ipf_fw_region8_addr(memmap_stub_phys);
    DPRINTF("PEI startup: bfv=%016" PRIx64 " bfv_size=%" PRIu64
            " temp=%016" PRIx64 " tsize=%" PRIu64
            " pei_temp=%016" PRIx64 " pei_tsize=%" PRIu64
            " stack=%016" PRIx64 " ssize=%" PRIu64
            " ppi=%016" PRIx64 " r9=%016" PRIx64 " r10=%016" PRIx64 "\n",
            bfv_phys, bfv_size, temp_phys, temp_size,
            temp_phys, pei_temp_size,
            stack_base - stack_size, stack_size,
            ppi_phys, ipf_boot_r9, ipf_boot_r10);

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        const char *handoff_kind = ipf_fw_pei_use_pi_handoff() ? "sec" : "startup";
        qemu_log_mask(LOG_GUEST_ERROR,
                      "IPF: PEI startup (%s): bfv=%016" PRIx64
                      " bfv_size=%" PRIu64 " startup=%016" PRIx64
                      " ppi=%016" PRIx64 " temp=%016" PRIx64
                      " tsize=%" PRIu64 " pei_temp=%016" PRIx64
                      " pei_tsize=%" PRIu64 " stack=%016" PRIx64
                      " ssize=%" PRIu64 " r10=%016" PRIx64 "\n",
                      handoff_kind, bfv_phys, bfv_size, handoff_phys, ppi_phys,
                      temp_phys, temp_size, temp_phys, pei_temp_size,
                      stack_base - stack_size, stack_size, ipf_boot_r10);
    }
}

typedef struct IpfTextWatch {
    MemoryRegion mr;
    uint8_t *ram_ptr;
    hwaddr ram_base;
    hwaddr pa_base;
    IA64CPU *cpu;
    uint32_t read_count;
    uint32_t write_count;
} IpfTextWatch;

static uint64_t ipf_text_watch_read(void *opaque, hwaddr addr, unsigned size)
{
    IpfTextWatch *w = opaque;
    CPUIA64State *env = w->cpu ? &w->cpu->env : NULL;
    hwaddr base = (w->pa_base >= w->ram_base) ?
                  (w->pa_base - w->ram_base) : 0;
    uint8_t *p = w->ram_ptr + base + addr;
    hwaddr pa = w->pa_base + addr;
    uint64_t ret = 0;

    switch (size) {
    case 1:
        ret = ldub_p(p);
        break;
    case 2:
        ret = lduw_le_p(p);
        break;
    case 4:
        ret = ldl_le_p(p);
        break;
    case 8:
        ret = ldq_le_p(p);
        break;
    default:
        ret = 0;
        break;
    }

    static int watch_read_enabled = -1;
    if (watch_read_enabled == -1) {
        const char *s = getenv("QEMU_IA64_WATCH_READ");
        watch_read_enabled = (s && *s) ? 1 : 0;
    }
    if (!watch_read_enabled) {
        return ret;
    }

    static int watch_size_mask = -1;
    if (watch_size_mask == -1) {
        watch_size_mask = 0;
        const char *s = getenv("QEMU_IA64_WATCH_SIZE");
        if (s && *s) {
            watch_size_mask = atoi(s);
        }
        if (watch_size_mask < 0) {
            watch_size_mask = 0;
        }
    }
    if (watch_size_mask && (((unsigned)watch_size_mask & size) == 0)) {
        return ret;
    }

    static int watch_limit = -1;
    if (watch_limit == -1) {
        watch_limit = 64;
        const char *slim = getenv("QEMU_IA64_WATCH_LIMIT");
        if (slim && *slim) {
            watch_limit = atoi(slim);
        }
        if (watch_limit < 0) {
            watch_limit = 0;
        }
    }

    if (w->read_count < (unsigned)watch_limit) {
        fprintf(stderr,
                "IPF_TEXT_WATCH: read size=%u pa=%016" HWADDR_PRIx " data=%016" PRIx64
                " ip=%016" PRIx64 " psr=%016" PRIx64
                " r1=%016" PRIx64 " r2=%016" PRIx64 " r3=%016" PRIx64
                " r12=%016" PRIx64 " r13=%016" PRIx64
                " r24=%016" PRIx64 " r27=%016" PRIx64 " r28=%016" PRIx64 " r31=%016" PRIx64
                " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                " r52=%016" PRIx64 " r53=%016" PRIx64
                " b0=%016" PRIx64 " b6=%016" PRIx64 "\n",
                size, pa, ret,
                env ? env->ip : 0, env ? env->psr : 0,
                env ? env->r[1] : 0, env ? env->r[2] : 0, env ? env->r[3] : 0,
                env ? env->r[12] : 0, env ? env->r[13] : 0,
                env ? env->r[24] : 0, env ? env->r[27] : 0, env ? env->r[28] : 0,
                env ? env->r[31] : 0,
                env ? env->r[32] : 0, env ? env->r[33] : 0, env ? env->r[34] : 0,
                env ? env->r[52] : 0, env ? env->r[53] : 0,
                env ? env->b[0] : 0, env ? env->b[6] : 0);
        fflush(stderr);
    }
    w->read_count++;
    return ret;
}

static void ipf_text_watch_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size)
{
    IpfTextWatch *w = opaque;
    CPUIA64State *env = w->cpu ? &w->cpu->env : NULL;
    hwaddr pa = w->pa_base + addr;
    hwaddr base = (w->pa_base >= w->ram_base) ?
                  (w->pa_base - w->ram_base) : 0;
    uint8_t *p = w->ram_ptr + base + addr;

    static int watch_limit = -1;
    if (watch_limit == -1) {
        watch_limit = 64;
        const char *slim = getenv("QEMU_IA64_WATCH_LIMIT");
        if (slim && *slim) {
            watch_limit = atoi(slim);
        }
        if (watch_limit < 0) {
            watch_limit = 0;
        }
    }

    static int watch_size_mask = -1;
    if (watch_size_mask == -1) {
        watch_size_mask = 0;
        const char *s = getenv("QEMU_IA64_WATCH_SIZE");
        if (s && *s) {
            watch_size_mask = atoi(s);
        }
        if (watch_size_mask < 0) {
            watch_size_mask = 0;
        }
    }

    if (!watch_size_mask || (((unsigned)watch_size_mask & size) != 0)) {
        if (w->write_count < (unsigned)watch_limit) {
            fprintf(stderr,
                    "IPF_TEXT_WATCH: write size=%u pa=%016" HWADDR_PRIx " data=%016" PRIx64
                    " ip=%016" PRIx64 " psr=%016" PRIx64
                    " r1=%016" PRIx64 " r2=%016" PRIx64 " r3=%016" PRIx64
                    " r12=%016" PRIx64 " r13=%016" PRIx64
                    " r24=%016" PRIx64 " r27=%016" PRIx64 " r28=%016" PRIx64 " r31=%016" PRIx64
                    " r32=%016" PRIx64 " r33=%016" PRIx64 " r34=%016" PRIx64
                    " r52=%016" PRIx64 " r53=%016" PRIx64
                    " b0=%016" PRIx64 " b6=%016" PRIx64 "\n",
                    size, pa, data,
                    env ? env->ip : 0, env ? env->psr : 0,
                    env ? env->r[1] : 0, env ? env->r[2] : 0, env ? env->r[3] : 0,
                    env ? env->r[12] : 0, env ? env->r[13] : 0,
                    env ? env->r[24] : 0, env ? env->r[27] : 0, env ? env->r[28] : 0,
                    env ? env->r[31] : 0,
                    env ? env->r[32] : 0, env ? env->r[33] : 0, env ? env->r[34] : 0,
                    env ? env->r[52] : 0, env ? env->r[53] : 0,
                    env ? env->b[0] : 0, env ? env->b[6] : 0);
            fflush(stderr);
        }
        w->write_count++;
    }

    /* Forward the write into underlying RAM. */
    switch (size) {
    case 1:
        stb_p(p, data);
        break;
    case 2:
        stw_le_p(p, data);
        break;
    case 4:
        stl_le_p(p, data);
        break;
    case 8:
        stq_le_p(p, data);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ipf_text_watch_ops = {
    .read = ipf_text_watch_read,
    .write = ipf_text_watch_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void ipf_add_text_watch(IPFMachineState *m, MemoryRegion *sysmem,
                               IA64CPU *cpu, MemoryRegion *ram,
                               hwaddr ram_base, hwaddr pa, hwaddr size,
                               const char *label)
{
    for (size_t i = 0; i < ARRAY_SIZE(m->text_watch); i++) {
        if (!m->text_watch[i]) {
            IpfTextWatch *w = g_new0(IpfTextWatch, 1);
            w->ram_ptr = memory_region_get_ram_ptr(ram);
            w->ram_base = ram_base;
            w->pa_base = pa;
            w->cpu = cpu;
            memory_region_init_io(&w->mr, OBJECT(m), &ipf_text_watch_ops, w,
                                  label, size);
            memory_region_add_subregion_overlap(sysmem, pa, &w->mr, 1000);
            m->text_watch[i] = w;
            fprintf(stderr,
                    "IPF_TEXT_WATCH: watching %s PA=%016" HWADDR_PRIx
                    " size=%" HWADDR_PRIx "\n",
                    label, pa, size);
            return;
        }
    }
    fprintf(stderr, "IPF_TEXT_WATCH: no free watch slots for %s\n", label);
}

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
#define IPF_EFI_CONFTAB_ADDR 0x0000000000014000ULL
#define IPF_EFI_PCDP_ADDR    0x0000000000015000ULL
#define IPF_EFI_VENDOR_ADDR  0x0000000000016000ULL

/* EFI table signatures (see Linux include/linux/efi.h). */
#define EFI_SYSTEM_TABLE_SIGNATURE      0x5453595320494249ULL /* "IBI SYST" */
#define EFI_RUNTIME_SERVICES_SIGNATURE  0x5652453544e5552ULL   /* "RUNTSERV" */
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

typedef struct QEMU_PACKED {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
} IPFEfiGuid;

typedef struct QEMU_PACKED {
    IPFEfiGuid guid;
    uint64_t table;
} IPFEfiConfigTable;

/*
 * Minimal HCDP/PCDP table describing a single MMIO 8250 UART.
 *
 * Linux uses the EFI config table entry HCDP_TABLE_GUID to locate this table
 * and will call setup_earlycon() based on its contents.
 */
typedef struct QEMU_PACKED {
    uint8_t space_id;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_width;
    uint64_t address;
} IPFAcpiGenericAddress;

typedef struct QEMU_PACKED {
    uint8_t type;
    uint8_t bits;
    uint8_t parity;
    uint8_t stop_bits;
    uint8_t pci_seg;
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;
    uint64_t baud;
    IPFAcpiGenericAddress addr;
    uint16_t pci_dev_id;
    uint16_t pci_vendor_id;
    uint32_t gsi;
    uint32_t clock_rate;
    uint8_t pci_prog_intfc;
    uint8_t flags;
    uint16_t conout_index;
    uint32_t reserved;
} IPFPcdpUart;

typedef struct QEMU_PACKED {
    uint8_t signature[4];
    uint32_t length;
    uint8_t rev;
    uint8_t chksum;
    uint8_t oemid[6];
    uint8_t oem_tabid[8];
    uint32_t oem_rev;
    uint8_t creator_id[4];
    uint32_t creator_rev;
    uint32_t num_uarts;
} IPFPcdpHdr;

typedef struct QEMU_PACKED {
    IPFPcdpHdr hdr;
    IPFPcdpUart uart0;
} IPFPcdpTable;

/*
 * Minimal SAL System Table (SST_) and entry-point descriptor.
 *
 * Linux/ia64 uses this to locate PAL and SAL procedure entry points.
 * We provide synthetic procedures that are emulated by the IA-64 TCG backend.
 */
typedef struct QEMU_PACKED {
    uint8_t signature[4]; /* "SST_" */
    uint32_t size;
    uint8_t sal_rev_minor;
    uint8_t sal_rev_major;
    uint16_t entry_count;
    uint8_t checksum;
    uint8_t reserved1[7];
    uint8_t sal_a_rev_minor;
    uint8_t sal_a_rev_major;
    uint8_t sal_b_rev_minor;
    uint8_t sal_b_rev_major;
    uint8_t oem_id[32];
    uint8_t product_id[32];
    uint8_t reserved2[8];
} IPFSalSystab;

typedef struct QEMU_PACKED {
    uint8_t type; /* 0 == SAL_DESC_ENTRY_POINT */
    uint8_t reserved1[7];
    uint64_t pal_proc;
    uint64_t sal_proc;
    uint64_t gp;
    uint8_t reserved2[16];
} IPFSalDescEntryPoint;

#define IPF_SAL_DESC_ENTRY_POINT 0

static uint8_t ipf_byte_checksum(const void *buf, size_t len);

static void ipf_write_sal_systab(void)
{
    IPFSalSystab sst = { 0 };
    IPFSalDescEntryPoint ep = { 0 };
    uint8_t buf[sizeof(sst) + sizeof(ep)] = { 0 };

    memcpy(sst.signature, "SST_", 4);
    sst.size = sizeof(buf);
    sst.sal_rev_major = 2;
    sst.sal_rev_minor = 0;
    sst.entry_count = 1;
    sst.sal_a_rev_major = 0;
    sst.sal_a_rev_minor = 0;
    sst.sal_b_rev_major = 0;
    sst.sal_b_rev_minor = 0;
    memcpy(sst.oem_id, "QEMU", 4);
    memcpy(sst.product_id, "QEMU-IPF", 7);
    sst.checksum = 0;

    ep.type = IPF_SAL_DESC_ENTRY_POINT;
    ep.pal_proc = IA64_IPF_FW_PAL_PROC_ADDR;
    ep.sal_proc = IA64_IPF_FW_SAL_PROC_ADDR;
    ep.gp = IA64_IPF_FW_SAL_GP_ADDR;

    memcpy(buf, &sst, sizeof(sst));
    memcpy(buf + sizeof(sst), &ep, sizeof(ep));
    buf[offsetof(IPFSalSystab, checksum)] = ipf_byte_checksum(buf, sizeof(buf));

    address_space_write(&address_space_memory, IA64_IPF_FW_SAL_SYSTAB_ADDR,
                        MEMTXATTRS_UNSPECIFIED, (const uint8_t *)buf, sizeof(buf));
}

/*
 * Minimal ACPI 2.0 tables for IA-64 Linux bringup.
 *
 * QEMU's IPF machine historically relied on guest firmware to provide ACPI
 * tables. When booting a kernel directly via -kernel, synthesize the minimum
 * required set via EFI config tables:
 *   - RSDP (ACPI 2.0) -> XSDT -> FADT + MADT (+ DSDT referenced by FADT)
 */
typedef struct QEMU_PACKED {
    uint8_t signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t oem_id[6];
    uint8_t oem_table_id[8];
    uint32_t oem_revision;
    uint8_t creator_id[4];
    uint32_t creator_revision;
} IPFAcpiTableHeader;

typedef struct QEMU_PACKED {
    uint8_t signature[8]; /* "RSD PTR " */
    uint8_t checksum;
    uint8_t oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} IPFAcpiRsdp;

typedef struct QEMU_PACKED {
    IPFAcpiTableHeader header;
    uint32_t address;
    uint32_t flags;
} IPFAcpiMadt;

typedef struct QEMU_PACKED {
    uint8_t type;
    uint8_t length;
} IPFAcpiSubtableHeader;

typedef struct QEMU_PACKED {
    IPFAcpiSubtableHeader header;
    uint8_t processor_id;
    uint8_t id;
    uint8_t eid;
    uint8_t reserved[3];
    uint32_t lapic_flags;
    uint32_t uid;
} IPFAcpiMadtLocalSapic;

typedef struct QEMU_PACKED {
    IPFAcpiSubtableHeader header;
    uint8_t id;
    uint8_t reserved;
    uint32_t global_irq_base;
    uint64_t address;
} IPFAcpiMadtIoSapic;

typedef struct QEMU_PACKED {
    IPFAcpiTableHeader header;
    uint32_t facs;
    uint32_t dsdt;
    uint8_t model;
    uint8_t preferred_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4_bios_request;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_block_length;
    uint8_t gpe1_block_length;
    uint8_t gpe1_base;
    uint8_t cst_control;
    uint16_t c2_latency;
    uint16_t c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    uint16_t boot_flags;
    uint8_t reserved;
    uint32_t flags;
    IPFAcpiGenericAddress reset_register;
    uint8_t reset_value;
    uint8_t reserved2[3];
    uint64_t Xfacs;
    uint64_t Xdsdt;
    IPFAcpiGenericAddress xpm1a_event_block;
    IPFAcpiGenericAddress xpm1b_event_block;
    IPFAcpiGenericAddress xpm1a_control_block;
    IPFAcpiGenericAddress xpm1b_control_block;
    IPFAcpiGenericAddress xpm2_control_block;
    IPFAcpiGenericAddress xpm_timer_block;
    IPFAcpiGenericAddress xgpe0_block;
    IPFAcpiGenericAddress xgpe1_block;
} IPFAcpiFadt;

/* GUIDs (match Linux include/linux/efi.h). */
static const IPFEfiGuid ipf_guid_sal_systab = {
    .data1 = 0xeb9d2d32,
    .data2 = 0x2d88,
    .data3 = 0x11d3,
    .data4 = { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d },
};

static const IPFEfiGuid ipf_guid_hcdp = {
    .data1 = 0xf951938d,
    .data2 = 0x620b,
    .data3 = 0x42ef,
    .data4 = { 0x82, 0x79, 0xa8, 0x4b, 0x79, 0x61, 0x78, 0x98 },
};

static const IPFEfiGuid ipf_guid_acpi_20 = {
    .data1 = 0x8868e871,
    .data2 = 0xe4f1,
    .data3 = 0x11d3,
    .data4 = { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 },
};

static uint8_t ipf_byte_checksum(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    uint8_t sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum += p[i];
    }
    return (uint8_t)(0 - sum);
}

static void ipf_acpi_init_header(IPFAcpiTableHeader *hdr, const char sig[4],
                                 uint32_t length, uint8_t revision)
{
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->signature, sig, 4);
    hdr->length = cpu_to_le32(length);
    hdr->revision = revision;
    hdr->checksum = 0;
    memcpy(hdr->oem_id, "QEMU  ", 6);
    memcpy(hdr->oem_table_id, "QEMU-IPF ", 8);
    hdr->oem_revision = cpu_to_le32(1);
    memcpy(hdr->creator_id, "QEMU", 4);
    hdr->creator_revision = cpu_to_le32(1);
}

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
#define EFI_LOADER_CODE              1
#define EFI_LOADER_DATA              2
#define EFI_MEMORY_MAPPED_IO         11
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE 12
#define EFI_CONVENTIONAL_MEMORY      7
#define EFI_PAL_CODE                 13

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
    const bool booting_firmware = (ipf_boot_ip == IPF_GFW_ENTRY);

    cpu_reset(env);
    DPRINTF("Reset CPU: boot_ip=0x%" PRIx64 " boot_r28=0x%" PRIx64
            " boot_r9=0x%" PRIx64 " boot_r10=0x%" PRIx64 "\n",
            ipf_boot_ip, ipf_boot_r28, ipf_boot_r9, ipf_boot_r10);
    if (ipf_boot_ip) {
        s->ip = ipf_boot_ip;
    }
    if (ipf_boot_r28) {
        s->r[28] = ipf_boot_r28;
    }
    if (booting_firmware) {
        s->r[9] = ipf_boot_r9;
        s->r[10] = ipf_boot_r10;
        s->r[33] = ipf_boot_r10;
        /*
         * Seed r34 with the PAL entry so the firmware stub can copy it into
         * ar.k5. fw_pei_entry_fix() clears r34 before PEI core to keep
         * OldCoreData NULL while preserving the PAL entry.
         */
        s->r[34] = IA64_IPF_FW_PAL_PROC_ADDR;
        s->ar[5] = IA64_IPF_FW_PAL_PROC_ADDR;
        s->fw_pei_handoff = ipf_boot_r9;
        s->fw_pei_ppi = ipf_boot_ppi;
        s->fw_pei_stack_count = ipf_boot_r10;
        s->fw_pei_findfv_stub = ipf_boot_findfv_stub;
        s->fw_pei_findfv_iface = ipf_boot_findfv_iface;
        s->fw_pei_secinfo_stub = ipf_boot_secinfo_stub;
        s->fw_pei_memmap_stub = ipf_boot_memmap_stub;
        s->fw_mem_size = ipf_boot_mem_size;
    } else {
        s->fw_pei_handoff = 0;
        s->fw_pei_ppi = 0;
        s->fw_pei_stack_count = 0;
        s->fw_pei_findfv_stub = 0;
        s->fw_pei_findfv_iface = 0;
        s->fw_pei_secinfo_stub = 0;
        s->fw_pei_memmap_stub = 0;
        s->fw_mem_size = 0;
    }
    /*
     * Seed ar.k0 (AR.KR0) with the legacy I/O port space base so that Linux
     * can find a sane default even if EFI doesn't describe the range.
     */
    s->ar[0] = IPF_LEGACY_IO_BASE;
    s->ar[IA64_AR_FPSR] = IA64_FPSR_DEFAULT;
    if (booting_firmware) {
        /*
         * xenipf SEC stack/BSP setup uses ar.k4 (boot r10) as a loop count
         * to size the temporary stack in 128KiB steps. Seed it so the stack
         * lands inside the fw-workram window.
         */
        /*
         * ar.k3 controls the xenipf SEC stack base; set to 3 so it uses
         * 0x04000000 and avoids overwriting the Xen GFW HOB list at 0xff200000.
         */
        s->ar[3] = 3; /* ar.k3 */
        s->ar[4] = ipf_boot_r10; /* ar.k4 */
        /*
         * EDK PAL call stubs fall back to ar.k5 when no PAL entry is passed.
         * Point it at the synthetic PAL entry so early firmware PAL calls
         * trap into QEMU instead of branching to 0.
         */
        s->ar[5] = IA64_IPF_FW_PAL_PROC_ADDR;
    }

    /*
     * Provide a deterministic initial stack pointer and RSE backing store in
     * physical mode.
     *
     * - Linux/ia64 expects firmware/bootloader to provide a valid r12.
     * - The Xen/KVM IA-64 guest firmware expects a valid stack too.
     *
     * Place it below the loaded kernel image when available; otherwise use
     * the top of guest RAM.
     */
    uint64_t stack_top = ipf_kernel_low ? ipf_kernel_low : ipf_ram_size;
    if (stack_top > (1ULL << 20)) {
        stack_top -= (1ULL << 20); /* 1MiB below base */
    } else {
        stack_top = (1ULL << 20);
    }
    stack_top &= ~0xFULL;
    s->r[12] = stack_top;

    /* Backing store grows upward; keep it below the memory stack. */
    uint64_t bspstore = (stack_top > (8ULL << 20)) ? (stack_top - (8ULL << 20))
                                                   : (2ULL << 20);
    bspstore &= ~0x7ULL;
    s->ar[IA64_AR_BSPSTORE] = bspstore;
    s->ar[IA64_AR_BSP] = bspstore;

    /*
     * Xen's IA-64 HVM builder provides a distinct bootstrap state for guest
     * firmware. Match it so the PEI dispatcher can run the BFV PEIMs.
     */
    if (booting_firmware) {
        s->psr = IA64_PSR_AC | IA64_PSR_BN;
        s->cr[21] = ((uint64_t)TARGET_PAGE_BITS << 2); /* cr.itir (ps) */
        s->cr[8] = (15ULL << 2);                       /* cr.pta */
    } else if (ipf_boot_ip) {
        /*
         * Seed cr.pta so Linux/ia64's early IVT itlb/dtlb miss handlers can
         * locate PTEs via cr.iha before ia64_mmu_init() programs the final
         * VMLPT layout.
         *
         * Model a CPU with a 61-bit implemented VA space and Linux/ia64 base
         * page size (TARGET_PAGE_BITS):
         *   vmlpt_bits = impl_va_bits - PAGE_SHIFT + pte_bits
         *            = 61 - TARGET_PAGE_BITS + 3
         *   pta_base   = 2^61 - 2^vmlpt_bits
         */
        const uint64_t impl_va_bits = 61;
        const uint64_t page_shift = TARGET_PAGE_BITS;
        const uint64_t pte_bits = 3;
        const uint64_t vmlpt_bits = impl_va_bits - page_shift + pte_bits;
        uint64_t pta_base = (1ULL << 61) - (1ULL << vmlpt_bits);

        uint64_t pta = 0;
        pta |= pta_base;
        pta |= (vmlpt_bits << 2); /* SIZE */
        /*
         * Keep PTA.VE=0 so early Linux uses the software TLB miss handlers
         * (itc.d/itc.i) instead of taking the VHPT translation vector (0)
         * when the VMLPT itself isn't mapped yet.
         */
        s->cr[8] = pta;           /* cr.pta */
    }
}

static void ipf_uart_dummy_irq(void *opaque, int n, int level)
{
    /* Polled UART use (earlycon) does not require an interrupt controller. */
}

static uint64_t ipf_debugcon_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)size;
    return 0;
}

static inline hwaddr ipf_phys_mode_addr(uint64_t addr)
{
    uint64_t hi32 = addr & 0xffffffff00000000ULL;
    if (hi32 == 0 || hi32 == 0xffffffff00000000ULL) {
        return (hwaddr)(uint32_t)addr;
    }
    return (hwaddr)(addr & ((1ULL << 61) - 1));
}

static void ipf_debugcon_log_hob(IPFMachineState *m, const char *tag,
                                 const char *line)
{
    static int ctx_enabled = -1;
    if (!m || !m->cpu || !qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }

    CPUIA64State *env = &m->cpu->env;
    uint64_t ptrs[3] = { env->r[28], env->r[35], env->r[40] };
    uint64_t ptr_sigs[3] = { 0 };
    bool ptr_sig_ok[3] = { false, false, false };
    uint64_t lists[3] = { 0 };
    uint64_t list_sigs[3] = { 0 };
    bool list_sig_ok[3] = { false, false, false };

    for (size_t i = 0; i < 3; i++) {
        if (!ptrs[i]) {
            continue;
        }
        uint8_t tmp[8];
        hwaddr phys = ipf_phys_mode_addr(ptrs[i]);
        if (cpu_memory_rw_debug(env_cpu(env), phys, tmp, sizeof(tmp), false) == 0) {
            ptr_sigs[i] = ldq_le_p(tmp);
            ptr_sig_ok[i] = true;
        }
        if (cpu_memory_rw_debug(env_cpu(env), phys, tmp, sizeof(tmp), false) == 0) {
            lists[i] = ldq_le_p(tmp);
        }
        if (lists[i]) {
            phys = ipf_phys_mode_addr(lists[i]);
            if (cpu_memory_rw_debug(env_cpu(env), phys, tmp, sizeof(tmp), false) == 0) {
                list_sigs[i] = ldq_le_p(tmp);
                list_sig_ok[i] = true;
            }
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "FWDBG_HOB %s line=\"%s\" ip=%016" PRIx64
                  " b0=%016" PRIx64 " cfm=%016" PRIx64
                  " bsp=%016" PRIx64 " bspstore=%016" PRIx64
                  " r1=%016" PRIx64 " r12=%016" PRIx64
                  " r28=%016" PRIx64 " r35=%016" PRIx64 " r40=%016" PRIx64
                  " r32=%016" PRIx64 " r33=%016" PRIx64
                  " hob28=%016" PRIx64 "/%016" PRIx64 "/%d/%016" PRIx64 "/%d"
                  " hob35=%016" PRIx64 "/%016" PRIx64 "/%d/%016" PRIx64 "/%d"
                  " hob40=%016" PRIx64 "/%016" PRIx64 "/%d/%016" PRIx64 "/%d\n",
                  tag, line, env->ip, env->b[0], env->cfm,
                  env->ar[IA64_AR_BSP], env->ar[IA64_AR_BSPSTORE],
                  env->r[1], env->r[12], env->r[28], env->r[35], env->r[40],
                  env->r[32], env->r[33],
                  ptr_sigs[0], lists[0], list_sig_ok[0] ? 1 : 0, list_sigs[0],
                  ptr_sig_ok[0] ? 1 : 0,
                  ptr_sigs[1], lists[1], list_sig_ok[1] ? 1 : 0, list_sigs[1],
                  ptr_sig_ok[1] ? 1 : 0,
                  ptr_sigs[2], lists[2], list_sig_ok[2] ? 1 : 0, list_sigs[2],
                  ptr_sig_ok[2] ? 1 : 0);

    if (ctx_enabled == -1) {
        ctx_enabled = getenv("QEMU_IPF_DEBUGCON_CTX") ? 1 : 0;
    }
    if (ctx_enabled) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "FWDBG_CTX last_br from=%016" PRIx64 " to=%016" PRIx64
                      " kind=%" PRIu64 " insn=%011" PRIx64
                      " last_b0 pc=%016" PRIx64 " val=%016" PRIx64 " kind=%" PRIu64 "\n",
                      env->last_branch_from, env->last_branch_to,
                      env->last_branch_kind, env->last_branch_insn,
                      env->last_b0_write_pc, env->last_b0_write_val,
                      env->last_b0_write_kind & 0xff);
        for (int i = 0; i < 16; i++) {
            int idx = (env->b0_trace_idx + i) & 0xf;
            if (!env->b0_trace_pc[idx]) {
                continue;
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "FWDBG_CTX b0_trace[%02d] pc=%016" PRIx64
                          " val=%016" PRIx64 " kind=%" PRIu64
                          " insn=%011" PRIx64 "\n",
                          i, env->b0_trace_pc[idx], env->b0_trace_val[idx],
                          env->b0_trace_kind[idx] & 0xff,
                          env->b0_trace_insn[idx]);
        }
    }
}

static bool ipf_debugcon_line_accum(IPFMachineState *m, uint8_t ch)
{
    if (ch == '\r') {
        return false;
    }
    if (ch != '\n' && m->debugcon_line_len + 1 < sizeof(m->debugcon_line)) {
        m->debugcon_line[m->debugcon_line_len++] = ch;
        m->debugcon_line[m->debugcon_line_len] = '\0';
        return false;
    }
    return true;
}

static void ipf_debugcon_trace_line(IPFMachineState *m, const char *line,
                                    int log_to_qemu_log, int dxe_trace_enabled,
                                    int hob_on_assert_enabled)
{
    if (!log_to_qemu_log || !qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        return;
    }

    if (dxe_trace_enabled &&
        (strstr(line, "DXE") ||
         strstr(line, "Dxe") ||
         strstr(line, "Status") ||
         strstr(line, "ASSERT"))) {
        ipf_log_dxe_status(m, line);
    }
    if (strstr(line, "FakeMemMap") ||
        strstr(line, "hob signature") ||
        strstr(line, "HOB signature")) {
        ipf_debugcon_log_hob(m, "memmap", line);
    }

    if (!m->debugcon_trace_once &&
        (strstr(line, "AllocatePoolPages: failed") ||
         strstr(line, "AllocatePool: failed") ||
         strstr(line, "ASSERT"))) {
        m->debugcon_trace_once = true;
        if (hob_on_assert_enabled) {
            ipf_dump_gfw_hob("assert");
        }
        if (m->cpu) {
            CPUIA64State *env = &m->cpu->env;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "FWDBG_CTX last_br from=%016" PRIx64 " to=%016" PRIx64
                          " kind=%" PRIu64 " insn=%011" PRIx64
                          " last_b0 pc=%016" PRIx64 " val=%016" PRIx64 " kind=%" PRIu64 "\n",
                          env->last_branch_from, env->last_branch_to,
                          env->last_branch_kind, env->last_branch_insn,
                          env->last_b0_write_pc, env->last_b0_write_val,
                          env->last_b0_write_kind & 0xff);
            for (int i = 0; i < 16; i++) {
                int idx = (env->b0_trace_idx + i) & 0xf;
                if (!env->b0_trace_pc[idx]) {
                    continue;
                }
                qemu_log_mask(LOG_GUEST_ERROR,
                              "FWDBG_CTX b0_trace[%02d] pc=%016" PRIx64
                              " val=%016" PRIx64 " kind=%" PRIu64
                              " insn=%011" PRIx64 "\n",
                              i, env->b0_trace_pc[idx], env->b0_trace_val[idx],
                              env->b0_trace_kind[idx] & 0xff,
                              env->b0_trace_insn[idx]);
            }
        }
    }
}

static void ipf_debugcon_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    IPFMachineState *m = opaque;
    uint8_t ch = data & 0xff;
    static int log_to_qemu_log = -1;
    static int dxe_trace_enabled = -1;
    static int hob_on_assert_enabled = -1;

    if (size != 1) {
        return;
    }

    if (log_to_qemu_log == -1) {
        log_to_qemu_log = getenv("QEMU_IPF_DEBUGCON_QEMU_LOG") ? 1 : 0;
    }
    if (dxe_trace_enabled == -1) {
        dxe_trace_enabled = getenv("QEMU_IPF_DXE_TRACE") ? 1 : 0;
    }
    if (hob_on_assert_enabled == -1) {
        hob_on_assert_enabled = getenv("QEMU_IPF_DUMP_HOB_ON_ASSERT") ? 1 : 0;
    }

    bool line_complete = ipf_debugcon_line_accum(m, ch);
    if (!line_complete && !m->debugcon_line_traced) {
        if (strstr(m->debugcon_line, "DXE") ||
            strstr(m->debugcon_line, "Dxe") ||
            strstr(m->debugcon_line, "Status") ||
            strstr(m->debugcon_line, "ASSERT")) {
            ipf_debugcon_trace_line(m, m->debugcon_line, log_to_qemu_log,
                                    dxe_trace_enabled, hob_on_assert_enabled);
            m->debugcon_line_traced = true;
        }
    }

    if (m->debugcon_line_mode) {
        if (!line_complete) {
            return;
        }

        uint64_t ip = 0, psr = 0, b0 = 0;
        if (m->cpu) {
            CPUIA64State *env = &m->cpu->env;
            ip = env->ip;
            psr = env->psr;
            b0 = env->b[0];
        }

        if (m->debugcon_chr) {
            char line[1024];
            int n = snprintf(line, sizeof(line),
                             "FWDBG ip=%016" PRIx64 " psr=%016" PRIx64
                             " b0=%016" PRIx64 " %s\n",
                             ip, psr, b0, m->debugcon_line);
            if (n > 0) {
                qemu_chr_write_all(m->debugcon_chr, (const uint8_t *)line,
                                   MIN(n, (int)sizeof(line)));
            }
        }

        if (log_to_qemu_log && qemu_loglevel_mask(LOG_GUEST_ERROR)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "FWDBG ip=%016" PRIx64 " psr=%016" PRIx64 " b0=%016" PRIx64
                          " %s\n",
                          ip, psr, b0, m->debugcon_line);
        }
        ipf_debugcon_trace_line(m, m->debugcon_line, log_to_qemu_log,
                                dxe_trace_enabled, hob_on_assert_enabled);
        m->debugcon_line_len = 0;
        m->debugcon_line[0] = '\0';
        m->debugcon_line_traced = false;
        return;
    }

    if (line_complete) {
        ipf_debugcon_trace_line(m, m->debugcon_line, log_to_qemu_log,
                                dxe_trace_enabled, hob_on_assert_enabled);
        m->debugcon_line_len = 0;
        m->debugcon_line[0] = '\0';
        m->debugcon_line_traced = false;
    }

    if (m->debugcon_chr) {
        qemu_chr_write_all(m->debugcon_chr, &ch, 1);
        if (!log_to_qemu_log) {
            return;
        }
    }

    if (qemu_loglevel_mask(LOG_GUEST_ERROR)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%c", ch);
        return;
    }

    (void)addr;
    fputc(ch, stderr);
    fflush(stderr);
}

static const MemoryRegionOps ipf_debugcon_ops = {
    .read = ipf_debugcon_read,
    .write = ipf_debugcon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void ipf_init_debugcon(IPFMachineState *m)
{
    /*
     * Capture common firmware debug output ports:
     * - 0xe9: Bochs/QEMU debug port.
     * - 0x402: isa-debugcon default.
     *
     * Emit characters via LOG_GUEST_ERROR so scripts/run-ia64-firmware.sh can
     * capture them in its -D log.
     */
    memory_region_init_io(&m->debugcon_e9, OBJECT(m), &ipf_debugcon_ops, m,
                          "ipf.debugcon-e9", 1);
    memory_region_add_subregion(get_system_io(), 0xe9, &m->debugcon_e9);

    memory_region_init_io(&m->debugcon_402, OBJECT(m), &ipf_debugcon_ops, m,
                          "ipf.debugcon-402", 1);
    memory_region_add_subregion(get_system_io(), 0x402, &m->debugcon_402);

    m->debugcon_line_len = 0;
    m->debugcon_line[0] = '\0';
    m->debugcon_line_mode = getenv("QEMU_IPF_DEBUGCON_LINE") ? true : false;
    m->debugcon_trace_once = false;
    m->debugcon_line_traced = false;
    m->debugcon_chr = serial_hd(0);
}

static uint64_t ipf_uart_ioport_read(void *opaque, hwaddr addr, unsigned size)
{
    SerialMM *uart = opaque;
    SerialState *s = &uart->serial;

    if (size != 1 || addr >= 8) {
        return 0;
    }
    return serial_io_ops.read(s, addr, 1);
}

static void ipf_uart_ioport_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    SerialMM *uart = opaque;
    SerialState *s = &uart->serial;

    if (size != 1 || addr >= 8) {
        return;
    }
    serial_io_ops.write(s, addr, data & 0xff, 1);
}

static const MemoryRegionOps ipf_uart_ioport_ops = {
    .read = ipf_uart_ioport_read,
    .write = ipf_uart_ioport_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void ipf_init_uart(IPFMachineState *m, MemoryRegion *sysmem)
{
    Chardev *chr = serial_hd(0);
    if (!chr) {
        DPRINTF("UART: no serial backend (-serial), skipping\n");
        return;
    }

    qemu_irq irq = qemu_allocate_irq(ipf_uart_dummy_irq, NULL, 0);

    SerialMM *uart = SERIAL_MM(qdev_new(TYPE_SERIAL_MM));
    qdev_prop_set_uint8(DEVICE(uart), "regshift", 0);
    qdev_prop_set_uint32(DEVICE(uart), "baudbase", 115200);
    qdev_prop_set_chr(DEVICE(uart), "chardev", chr);
    qdev_prop_set_uint8(DEVICE(uart), "endianness", DEVICE_LITTLE_ENDIAN);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0, irq);
    m->uart_mm = uart;

    /* Overlay the UART on top of the GFW RAM window. */
    MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(uart), 0);
    memory_region_add_subregion_overlap(sysmem, IPF_UART_BASE, mr, 1);
    DPRINTF("UART: mapped serial-mm at 0x%016" PRIx64 "\n", (uint64_t)IPF_UART_BASE);

    /*
     * Guest firmware commonly uses legacy COM1 at I/O port 0x3f8. IA-64 uses
     * a sparse memory-mapped I/O port window, which we translate to ioport
     * numbers via cpu_inb/outb in ipf_legacy_io_{read,write}().
     *
     * Alias COM1 ioports to the same SerialState so firmware and kernel share
     * one serial backend and one log file.
     */
    memory_region_init_io(&m->uart_ioport, OBJECT(m),
                          &ipf_uart_ioport_ops, uart,
                          "ipf.uart-ioport", 8);
    memory_region_add_subregion(get_system_io(), 0x3f8, &m->uart_ioport);
    DPRINTF("UART: aliased COM1 ioports at 0x3f8\n");
}

static uint32_t ipf_to_legacy_io(hwaddr addr)
{
    /*
     * Convert an address within the legacy I/O port space window back into
     * a 16-bit ioport number.
     *
     * Linux/ia64 uses a sparse encoding where each group of 4 ports is
     * separated by 4KB (see arch/ia64/include/asm/io.h IO_SPACE_LIMIT).
     */
    return (uint32_t)(((addr & 0x3ffffff) >> 12 << 2) | (addr & 0x3));
}

static bool ipf_parse_range(const char *s, uint64_t *out_lo, uint64_t *out_hi)
{
    if (!s || !*s) {
        return false;
    }
    char *endp = NULL;
    uint64_t lo = strtoull(s, &endp, 0);
    if (!endp || endp == s) {
        return false;
    }

    while (*endp && (isspace((unsigned char)*endp) || *endp == ',')) {
        endp++;
    }
    uint64_t hi = lo;
    if (*endp) {
        if (*endp == '-' || *endp == ':') {
            endp++;
        } else if (endp[0] == '.' && endp[1] == '.') {
            endp += 2;
        }
        while (*endp && isspace((unsigned char)*endp)) {
            endp++;
        }
        if (*endp) {
            uint64_t tmp = strtoull(endp, NULL, 0);
            if (tmp < lo) {
                hi = lo;
                lo = tmp;
            } else {
                hi = tmp;
            }
        }
    }

    *out_lo = lo;
    *out_hi = hi;
    return true;
}

static void ipf_trace_ioport(IPFMachineState *m, bool is_write,
                             uint32_t port, unsigned size, uint32_t val)
{
    static int trace_pci = -1;
    static int trace_vga = -1;
    static int trace_post = -1;
    static int trace_ports = -1;
    static uint16_t trace_port_lo;
    static uint16_t trace_port_hi;
    static int trace_limit = -1;
    static int trace_count;

    if (trace_pci == -1) {
        trace_pci = getenv("QEMU_IPF_TRACE_PCI") ? 1 : 0;
        trace_vga = getenv("QEMU_IPF_TRACE_VGA") ? 1 : 0;
        trace_post = getenv("QEMU_IPF_TRACE_POST") ? 1 : 0;
        trace_ports = 0;
        const char *s = getenv("QEMU_IPF_TRACE_IOPORTS");
        if (s && *s) {
            uint64_t lo = 0, hi = 0;
            if (ipf_parse_range(s, &lo, &hi)) {
                trace_ports = 1;
                trace_port_lo = (uint16_t)(lo & 0xffff);
                trace_port_hi = (uint16_t)(hi & 0xffff);
            }
        }
        trace_limit = 256;
        const char *limit = getenv("QEMU_IPF_TRACE_LIMIT");
        if (limit && *limit) {
            trace_limit = atoi(limit);
        }
        if (trace_limit < 0) {
            trace_limit = 0;
        }
    }

    if (trace_count >= trace_limit) {
        return;
    }

    if (trace_pci && (port == 0xcf8 || port == 0xcfc)) {
        trace_count++;
        if (port == 0xcf8 && is_write && size == 4) {
            m->trace_pci_cfgaddr = val;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ipf pci cfgaddr %s size=%u val=%08x\n",
                          is_write ? "wr" : "rd", size, val);
            return;
        }

        uint32_t cfgaddr = m->trace_pci_cfgaddr;
        uint8_t bus = (cfgaddr >> 16) & 0xff;
        uint8_t dev = (cfgaddr >> 11) & 0x1f;
        uint8_t func = (cfgaddr >> 8) & 0x7;
        uint16_t reg = (cfgaddr & 0xfc);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ipf pci cfgdata %s bus=%u dev=%u fn=%u reg=0x%02x size=%u val=%08x\n",
                      is_write ? "wr" : "rd", bus, dev, func, reg, size, val);
        return;
    }

    if (trace_vga && port >= 0x3b0 && port <= 0x3df) {
        trace_count++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ipf vga ioport %s port=0x%04x size=%u val=%08x\n",
                      is_write ? "wr" : "rd", port, size, val);
        return;
    }

    if (trace_post && (port == 0x80 || port == 0x84)) {
        trace_count++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ipf post ioport %s port=0x%04x size=%u val=%08x\n",
                      is_write ? "wr" : "rd", port, size, val);
        return;
    }

    if (trace_ports && port >= trace_port_lo && port <= trace_port_hi) {
        trace_count++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ipf ioport %s port=0x%04x size=%u val=%08x\n",
                      is_write ? "wr" : "rd", port, size, val);
        return;
    }
}

static void ipf_trace_mmio(const char *dev, hwaddr addr, unsigned size,
                           uint64_t data, bool is_write)
{
    static int trace_mmio = -1;
    static int trace_mmio_reads = -1;
    static bool trace_mmio_all;
    static uint64_t trace_mmio_lo;
    static uint64_t trace_mmio_hi;
    static int trace_mmio_limit = -1;
    static int trace_mmio_count;

    if (trace_mmio == -1) {
        trace_mmio = 0;
        trace_mmio_all = false;
        trace_mmio_reads = getenv("QEMU_IPF_TRACE_MMIO_READ") ? 1 : 0;
        const char *s = getenv("QEMU_IPF_TRACE_MMIO");
        if (s && *s) {
            if (!strcmp(s, "0") || !strcmp(s, "off") ||
                !strcmp(s, "false") || !strcmp(s, "no")) {
                trace_mmio = 0;
            } else {
                trace_mmio = 1;
                trace_mmio_all = true;
                uint64_t lo = 0, hi = 0;
                if (ipf_parse_range(s, &lo, &hi)) {
                    trace_mmio_all = false;
                    trace_mmio_lo = lo;
                    trace_mmio_hi = hi;
                }
            }
        }
        trace_mmio_limit = 256;
        const char *l = getenv("QEMU_IPF_TRACE_MMIO_LIMIT");
        if (l && *l) {
            trace_mmio_limit = atoi(l);
        }
        if (trace_mmio_limit < 0) {
            trace_mmio_limit = 0;
        }
    }

    if (!trace_mmio) {
        return;
    }
    if (!is_write && !trace_mmio_reads) {
        return;
    }
    if (!trace_mmio_all &&
        (addr < trace_mmio_lo || addr > trace_mmio_hi)) {
        return;
    }
    if (trace_mmio_limit != 0 && trace_mmio_count >= trace_mmio_limit) {
        return;
    }
    trace_mmio_count++;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "ipf mmio %s dev=%s addr=0x%016" PRIx64
                  " size=%u val=0x%08" PRIx64 "\n",
                  is_write ? "wr" : "rd",
                  dev ? dev : "unknown", (uint64_t)addr, size, data);
}

static uint64_t ipf_legacy_io_read(void *opaque, hwaddr addr, unsigned size)
{
    IPFMachineState *m = opaque;
    uint32_t port = ipf_to_legacy_io(addr);
    uint32_t val = 0;

    switch (size) {
    case 1:
        val = cpu_inb(port);
        break;
    case 2:
        val = cpu_inw(port);
        break;
    case 4:
        val = cpu_inl(port);
        break;
    default:
        val = 0;
        break;
    }
    ipf_trace_ioport(m, false, port, size, val);
    ipf_trace_mmio("legacy-io", IPF_LEGACY_IO_BASE + addr, size, val, false);
    return val;
}

static void ipf_legacy_io_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    IPFMachineState *m = opaque;
    uint32_t port = ipf_to_legacy_io(addr);

    ipf_trace_ioport(m, true, port, size, (uint32_t)data);
    ipf_trace_mmio("legacy-io", IPF_LEGACY_IO_BASE + addr, size, data, true);
    switch (size) {
    case 1:
        cpu_outb(port, data);
        break;
    case 2:
        cpu_outw(port, data);
        break;
    case 4:
        cpu_outl(port, data);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ipf_legacy_io_ops = {
    .read = ipf_legacy_io_read,
    .write = ipf_legacy_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void ipf_init_legacy_io(IPFMachineState *m, MemoryRegion *sysmem)
{
    memory_region_init_io(&m->legacy_io_mmio, OBJECT(m), &ipf_legacy_io_ops,
                          m, "ipf.legacy-io", IPF_LEGACY_IO_SIZE);
    memory_region_add_subregion(sysmem, IPF_LEGACY_IO_BASE, &m->legacy_io_mmio);
    DPRINTF("LEGACY-IO: mapped at 0x%016" PRIx64 " (size=0x%" PRIx64 ")\n",
            (uint64_t)IPF_LEGACY_IO_BASE, (uint64_t)IPF_LEGACY_IO_SIZE);
}

static const char *ipf_pcihost_root_bus_path(PCIHostState *host_bridge,
                                             PCIBus *rootbus)
{
    (void)host_bridge;
    (void)rootbus;
    return "0000:00";
}

static void ipf_pcihost_initfn(Object *obj)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(obj);
    memory_region_init_io(&phb->conf_mem, obj, &pci_host_conf_le_ops, phb,
                          "pci-conf-idx", 4);
    memory_region_init_io(&phb->data_mem, obj, &pci_host_data_le_ops, phb,
                          "pci-conf-data", 4);
}

static void ipf_pcihost_realize(DeviceState *dev, Error **errp)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /*
     * PCI config mechanism #1 (PC-style): cfgaddr/cfgdata I/O ports.
     *
     * IA-64 guests access I/O ports through the sparse legacy I/O MMIO window
     * (see ipf_legacy_io_{read,write}()).
     */
    memory_region_add_subregion(get_system_io(), 0xcf8, &phb->conf_mem);
    memory_region_add_subregion(get_system_io(), 0xcfc, &phb->data_mem);
    sysbus_init_ioports(sbd, 0xcf8, 4);
    sysbus_init_ioports(sbd, 0xcfc, 4);

    phb->bus = pci_root_bus_new(dev, "pci", get_system_memory(),
                                get_system_io(), 0, TYPE_PCI_BUS);

    DPRINTF("PCI: mapped cfgaddr 0xcf8 cfgdata 0xcfc\n");
    (void)errp;
}

static void ipf_pcihost_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);

    dc->realize = ipf_pcihost_realize;
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->fw_name = "pci";

    hc->root_bus_path = ipf_pcihost_root_bus_path;

    (void)data;
}

static const TypeInfo ipf_pcihost_info = {
    .name = TYPE_IPF_PCI_HOST,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(IPFPCIHost),
    .instance_init = ipf_pcihost_initfn,
    .class_init = ipf_pcihost_class_init,
};

static const VMStateDescription ipf_pci_root_vmstate = {
    .name = "ipf-pci-root",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, IPFPCIRoot),
        VMSTATE_END_OF_LIST()
    },
};

static void ipf_pci_root_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->desc = "IPF PCI host bridge (PCI-facing)";
    dc->vmsd = &ipf_pci_root_vmstate;
    dc->user_creatable = false;

    /*
     * The xenipf guest firmware expects a conventional host bridge at 00:00.0.
     * Vendor/device IDs are not currently used by our platform code; pick an
     * Intel 82441FX-compatible host bridge identity and class code.
     */
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82441;
    k->revision = 0;
    k->class_id = PCI_CLASS_BRIDGE_HOST;

    (void)data;
}

static const TypeInfo ipf_pci_root_info = {
    .name = TYPE_IPF_PCI_ROOT_DEVICE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(IPFPCIRoot),
    .class_init = ipf_pci_root_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ipf_init_pci(IPFMachineState *m)
{
    DeviceState *pcihost = qdev_new(TYPE_IPF_PCI_HOST);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pcihost), &error_fatal);
    m->pcibus = PCI_HOST_BRIDGE(pcihost)->bus;

    /*
     * Ensure 00:00.0 is a host bridge device so guest firmware enumeration
     * doesn't mis-identify the first device (e.g. VGA) as the host bridge.
     */
    pci_create_simple(m->pcibus, PCI_DEVFN(0, 0), TYPE_IPF_PCI_ROOT_DEVICE);
}

static void ipf_init_southbridge(IPFMachineState *m)
{
    /*
     * Original IPF hardware used an Intel PCI-to-ISA southbridge.
     * Provide a PIIX4-compatible device so firmware can discover it.
     */
    PCIDevice *piix = pci_new_multifunction(PCI_DEVFN(1, 0), TYPE_PIIX4_PCI_DEVICE);
    pci_realize_and_unref(piix, m->pcibus, &error_fatal);
}

static uint64_t ipf_acpi_pm_read(void *opaque, hwaddr addr, unsigned size)
{
    IPFMachineState *m = opaque;
    uint64_t val = 0;

    switch (addr) {
    case 0x00:
    case 0x01: {
        uint16_t v = m->acpi_pm1_evt_sts;
        val = (v >> (8 * (addr & 1))) & ((1ULL << (size * 8)) - 1);
        break;
    }
    case 0x02:
    case 0x03: {
        uint16_t v = m->acpi_pm1_evt_en;
        val = (v >> (8 * (addr & 1))) & ((1ULL << (size * 8)) - 1);
        break;
    }
    case 0x04:
    case 0x05: {
        uint16_t v = m->acpi_pm1_cnt;
        val = (v >> (8 * (addr & 1))) & ((1ULL << (size * 8)) - 1);
        break;
    }
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0b: {
        /*
         * ACPI PM timer: 24-bit free-running counter at 3.579545 MHz.
         * Return the low bits; higher bits are reserved.
         */
        uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - m->acpi_pm_timer_start_ns;
        uint32_t ticks = (uint32_t)muldiv64(ns, 3579545, NANOSECONDS_PER_SECOND);
        uint32_t v = ticks & 0x00ffffffU;
        unsigned shift = (addr & 3) * 8;
        val = (v >> shift) & ((1ULL << (size * 8)) - 1);
        break;
    }
    default:
        val = 0;
        break;
    }
    ipf_trace_mmio("acpi-pm", IPF_ACPI_PM_BASE + addr, size, val, false);
    return val;
}

static void ipf_acpi_pm_write(void *opaque, hwaddr addr, uint64_t data,
                              unsigned size)
{
    IPFMachineState *m = opaque;
    uint64_t mask = (size >= 8) ? UINT64_MAX : ((1ULL << (size * 8)) - 1);
    uint64_t val = data & mask;

    ipf_trace_mmio("acpi-pm", IPF_ACPI_PM_BASE + addr, size, data, true);
    switch (addr) {
    case 0x00:
    case 0x01: {
        unsigned shift = (addr & 1) * 8;
        uint16_t v = (uint16_t)(val << shift);
        /* Writing 1 clears status bits (ACPI). */
        m->acpi_pm1_evt_sts &= ~v;
        break;
    }
    case 0x02:
    case 0x03: {
        unsigned shift = (addr & 1) * 8;
        uint16_t v = (uint16_t)(val << shift);
        uint16_t cur = m->acpi_pm1_evt_en;
        cur &= ~(0xffu << shift);
        cur |= v;
        m->acpi_pm1_evt_en = cur;
        break;
    }
    case 0x04:
    case 0x05: {
        unsigned shift = (addr & 1) * 8;
        uint16_t v = (uint16_t)(val << shift);
        uint16_t cur = m->acpi_pm1_cnt;
        cur &= ~(0xffu << shift);
        cur |= v;
        m->acpi_pm1_cnt = cur;
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps ipf_acpi_pm_ops = {
    .read = ipf_acpi_pm_read,
    .write = ipf_acpi_pm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void ipf_init_acpi_pm(IPFMachineState *m, MemoryRegion *sysmem)
{
    m->acpi_pm1_evt_sts = 0;
    m->acpi_pm1_evt_en = 0;
    m->acpi_pm1_cnt = 0;
    m->acpi_pm_timer_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    memory_region_init_io(&m->acpi_pm_mmio, OBJECT(m), &ipf_acpi_pm_ops, m,
                          "ipf.acpi-pm", 0x1000);
    memory_region_add_subregion_overlap(sysmem, IPF_ACPI_PM_BASE,
                                        &m->acpi_pm_mmio, 1);
    DPRINTF("ACPI-PM: mapped at 0x%016" PRIx64 "\n", (uint64_t)IPF_ACPI_PM_BASE);
}

static uint64_t ipf_iosapic_read(void *opaque, hwaddr addr, unsigned size)
{
    IPFMachineState *m = opaque;
    uint32_t val = 0;

    switch (addr) {
    case IPF_IOSAPIC_REG_SELECT:
        val = m->iosapic_reg_select;
        break;
    case IPF_IOSAPIC_WINDOW: {
        uint32_t sel = m->iosapic_reg_select & 0xff;
        if (sel == IPF_IOSAPIC_VERSION_REG) {
            val = 0x000f0020U; /* 16 redirection entries, version 0x20 */
        } else if (sel < ARRAY_SIZE(m->iosapic_reg)) {
            val = m->iosapic_reg[sel];
        }
        break;
    }
    case IPF_IOSAPIC_EOI:
        val = 0;
        break;
    default:
        val = 0;
        break;
    }

    ipf_trace_mmio("iosapic", IPF_IOSAPIC_BASE + addr, size, val, false);
    return val;
}

static void ipf_iosapic_write(void *opaque, hwaddr addr, uint64_t data,
                              unsigned size)
{
    IPFMachineState *m = opaque;
    uint32_t val = (uint32_t)data;

    ipf_trace_mmio("iosapic", IPF_IOSAPIC_BASE + addr, size, data, true);
    switch (addr) {
    case IPF_IOSAPIC_REG_SELECT:
        m->iosapic_reg_select = val;
        break;
    case IPF_IOSAPIC_WINDOW: {
        uint32_t sel = m->iosapic_reg_select & 0xff;
        if (sel < ARRAY_SIZE(m->iosapic_reg)) {
            m->iosapic_reg[sel] = val;
        }
        break;
    }
    case IPF_IOSAPIC_EOI:
        /* Ignore end-of-interrupt for now. */
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ipf_iosapic_ops = {
    .read = ipf_iosapic_read,
    .write = ipf_iosapic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void ipf_init_iosapic(IPFMachineState *m, MemoryRegion *sysmem)
{
    /*
     * Provide a minimal IOSAPIC register window so Linux can program interrupt
     * routing based on MADT IO_SAPIC entries.
     */
    m->iosapic_reg_select = 0;
    memset(m->iosapic_reg, 0, sizeof(m->iosapic_reg));

    for (int i = 0; i < 16; i++) {
        /* Mask all entries by default. */
        m->iosapic_reg[0x10 + i * 2] = 1U << 16;
        m->iosapic_reg[0x11 + i * 2] = 0;
    }

    memory_region_init_io(&m->iosapic_mmio, OBJECT(m), &ipf_iosapic_ops, m,
                          "ipf.iosapic", IPF_IOSAPIC_SIZE);
    memory_region_add_subregion(sysmem, IPF_IOSAPIC_BASE, &m->iosapic_mmio);
    DPRINTF("IOSAPIC: mapped at 0x%016" PRIx64 "\n", (uint64_t)IPF_IOSAPIC_BASE);
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
    bool run_firmware = (!kernel_filename) || m->firmware_preboot;
    /* Initialize the cpu core */
    cpu = IA64_CPU(cpu_create(machine->cpu_type));
    if (!cpu) {
        error_report("Unable to find ia64 CPU definition");
        exit(1);
    }
    env = &cpu->env;
    m->cpu = cpu;

    pcdev = qdev_new(TYPE_IPF_PC);
    object_property_set_link(OBJECT(pcdev), "cpu", OBJECT(cpu), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pcdev), &error_fatal);

    /*
     * Xenipf firmware (and our GFW HOB builder) expect a legacy VGA hole at
     * 0xa0000..0xc0000. Present RAM as discontiguous around that window so the
     * top of RAM appears as ram_size + hole_size without increasing the total
     * RAM bytes provided by -m.
     */
    if (run_firmware && machine->ram_size > IPF_VGA_HOLE_START) {
        hwaddr low_size = MIN((hwaddr)machine->ram_size, (hwaddr)IPF_VGA_HOLE_START);
        hwaddr high_size = (hwaddr)machine->ram_size - low_size;

        memory_region_init_alias(&m->ram_low, OBJECT(machine), "ipf.ram.low",
                                 machine->ram, 0, low_size);
        memory_region_add_subregion(sysmem, 0, &m->ram_low);

        if (high_size) {
            memory_region_init_alias(&m->ram_high, OBJECT(machine), "ipf.ram.high",
                                     machine->ram, low_size, high_size);
            memory_region_add_subregion(sysmem, IPF_VGA_HOLE_START + IPF_VGA_HOLE_SIZE,
                                        &m->ram_high);
        }
    } else {
        memory_region_add_subregion(sysmem, 0, machine->ram);
    }
    ipf_ram_size = machine->ram_size;

    /*
     * Firmware slack RAM.
     *
     * The xenipf/EDK firmware sometimes locates relocated PEI/DXE images such
     * that the module-global pointer (r1/gp) and its small-data/GOT area end
     * up slightly above the top of guest RAM as reported through Xen's HOB
     * list (ram_size + VGA hole). Provide a small scratch region above RAM so
     * these accesses hit real memory rather than an unmapped hole.
     *
     * This region is intentionally not included in the guest memory map that
     * the firmware reports to the OS.
     */
    if (run_firmware) {
        hwaddr ram_top = (hwaddr)machine->ram_size;
        if (machine->ram_size > IPF_VGA_HOLE_START) {
            ram_top += IPF_VGA_HOLE_SIZE;
        }
        memory_region_init_ram(&m->ram_slack, NULL, "ipf.ram.slack",
                               IPF_FW_SLACK_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, ram_top, &m->ram_slack);
        DPRINTF("FW slack RAM: mapped at 0x%016" PRIx64 " size=%" PRIu64 "\n",
                (uint64_t)ram_top, (uint64_t)IPF_FW_SLACK_SIZE);
    }

    /*
     * Map the GFW window at the top of 32-bit physical space.
     *
     * This region is used by IA-64 guest firmware (GFW) as well as by the
     * Linux kernel for some early/legacy absolute addresses in the top 4GB.
     */
    memory_region_init_ram(&m->rom, NULL, "ipf.gfw", GFW_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, GFW_START, &m->rom);

    /* Small scratch RAM above 4GiB for xenipf firmware global data. */
    memory_region_init_ram(&m->fw_workram, NULL, "ipf.fw-workram",
                           IPF_FW_WORKRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, IPF_FW_WORKRAM_BASE, &m->fw_workram);
    DPRINTF("FW work RAM: mapped at 0x%016" PRIx64 " size=%" PRIu64 "\n",
            (uint64_t)IPF_FW_WORKRAM_BASE, (uint64_t)IPF_FW_WORKRAM_SIZE);
    /*
     * EDK firmware sometimes flips bit 63 on temporary RAM pointers when
     * switching to cache mode. Provide an alias so those region-4 addresses
     * still resolve to the same backing RAM.
     */
    memory_region_init_alias(&m->fw_workram_alias, OBJECT(machine),
                             "ipf.fw-workram.alias", &m->fw_workram, 0,
                             IPF_FW_WORKRAM_SIZE);
    memory_region_add_subregion(sysmem,
                                IPF_FW_WORKRAM_BASE | (1ULL << 63),
                                &m->fw_workram_alias);

    ipf_init_uart(m, sysmem);
    ipf_init_debugcon(m);
    ipf_init_legacy_io(m, sysmem);
    ipf_init_iosapic(m, sysmem);
    /*
     * Provide the ACPI PM1/PMTMR register block for both firmware and direct
     * -kernel boots.
     *
     * xenipf/EDK uses the ACPI PM timer for delays very early; leaving it
     * unmapped can stall firmware progress under TCG.
     */
    ipf_init_acpi_pm(m, sysmem);

    /* Optional firmware load if provided. */
    image_size = get_image_size(bios_name);
    if (image_size > 0) {
        ipf_fill_fw_window_erased();
        hwaddr fw_offset = GFW_START + GFW_SIZE - image_size;
        g_autofree uint8_t *buf = g_malloc((size_t)image_size);
        if (load_image_size(bios_name, buf, (size_t)image_size) != image_size) {
            error_report("Unable to read firmware file '%s'", bios_name);
            exit(1);
        }
        if (ipf_fw_scan_enabled()) {
            ipf_fw_scan_firmware(buf, (size_t)image_size, fw_offset);
        }
        address_space_write(&address_space_memory, fw_offset,
                            MEMTXATTRS_UNSPECIFIED, buf, (size_t)image_size);
        cpu_flush_icache_range(fw_offset, (size_t)image_size);
        DPRINTF("Loaded firmware '%s' at 0x%lx\n", bios_name, fw_offset);
        if (run_firmware) {
            ipf_fw_setup_pei_handoff(buf, (size_t)image_size, fw_offset);
        }
        if (run_firmware) {
            const char *dxe_dump = getenv("QEMU_IPF_FW_DXE_DUMP");
            if (dxe_dump && *dxe_dump) {
                ipf_fw_dump_dxe_core(buf, (size_t)image_size, fw_offset);
            }

            const char *watch_dxe = getenv("QEMU_IPF_FW_WATCH_DXE");
            if (watch_dxe && *watch_dxe) {
                size_t off = 0;
                uint64_t fsize = 0;
                if (ipf_fw_find_dxe_core(buf, (size_t)image_size, &off, &fsize)) {
                    if (fsize > 0) {
                        ipf_add_text_watch(m, sysmem, cpu, &m->rom, GFW_START,
                                           fw_offset + (hwaddr)off,
                                           (hwaddr)fsize, "fw_dxe_core");
                    }
                } else {
                    fprintf(stderr, "IPF_TEXT_WATCH: DXE core not found\n");
                }
            }

            const char *watch_range = getenv("QEMU_IPF_FW_WATCH_RANGE");
            if (watch_range && *watch_range) {
                uint64_t lo = 0;
                uint64_t hi = 0;
                if (ipf_parse_range(watch_range, &lo, &hi)) {
                    uint64_t size64 = (hi >= lo) ? (hi - lo + 1) : 0;
                    hwaddr pa = (hwaddr)lo;
                    if (lo < (uint64_t)image_size && hi < (uint64_t)image_size) {
                        pa = fw_offset + (hwaddr)lo;
                    }
                    uint64_t end = (uint64_t)pa + size64;
                    if (size64 == 0 || end < (uint64_t)pa) {
                        fprintf(stderr,
                                "IPF_TEXT_WATCH: invalid fw range size\n");
                    } else if (pa < GFW_START || end > (GFW_START + GFW_SIZE)) {
                        fprintf(stderr,
                                "IPF_TEXT_WATCH: fw range outside GFW window\n");
                    } else {
                        ipf_add_text_watch(m, sysmem, cpu, &m->rom, GFW_START,
                                           pa, (hwaddr)size64, "fw_range");
                    }
                } else {
                    fprintf(stderr,
                            "IPF_TEXT_WATCH: invalid QEMU_IPF_FW_WATCH_RANGE\n");
                }
            }
        }

        if (ipf_gfw_build_hob(machine->ram_size, machine->smp.cpus,
                              NVRAM_START) < 0) {
            error_report("Unable to build GFW HOB list");
            exit(1);
        }
        ipf_boot_mem_size = machine->ram_size;
        if (run_firmware) {
            ipf_dump_gfw_hob("boot");
        }

        /*
         * Provide a minimal SAL system table at a fixed low physical address.
         *
         * Only do this for direct -kernel boots; real firmware should supply
         * its own SAL table and EFI configuration entries.
         */
        if (!run_firmware) {
            ipf_write_sal_systab();
        }

        if (run_firmware) {
            ipf_patch_firmware_statuscode_callgate();
        }
    }

    if (run_firmware) {
        ipf_init_pci(m);
        ipf_init_southbridge(m);
        /*
         * Attach a PCI VGA device so the guest firmware can present a UI.
         * This honors the user's -vga selection (e.g. std/cirrus/virtio).
         */
        pci_vga_init(m->pcibus);
    }

    if (!kernel_filename) {
        if (image_size <= 0) {
            error_report("IPF requires -kernel or a valid -bios");
            exit(1);
        }
        /* Firmware-only boot: enter Xen/KVM guest firmware entry point. */
        ipf_boot_ip = IPF_GFW_ENTRY;
        ipf_boot_r28 = GFW_HOB_START;
        qemu_register_reset(main_cpu_reset, cpu);
        return;
    }

    /*
     * Load the ELF kernel. The IA-64 ELF uses physical p_paddr; entry
     * point phys_start is a low PA (see System.map).
     */
    ipf_sym_io_space = 0;
    ipf_sym_ia64_bad_break = 0;
    ipf_sym_search_extable = 0;
    ipf_sym_stext = 0;
    ipf_sym_etext = 0;
    ipf_sym_console_owner = 0;
    ipf_sym_console_waiter = 0;
    ipf_sym_switch_mode_phys = 0;
    ipf_sym_switch_mode_virt = 0;
    ipf_sym_switch_mode_phys_size = 0;
    ipf_sym_switch_mode_virt_size = 0;
    ipf_sym_ia64_switch_to = 0;
    ipf_sym_ia64_switch_to_size = 0;
    ipf_sym_load_switch_stack = 0;
    ipf_sym_load_switch_stack_size = 0;
    if (load_elf_ram_sym(kernel_filename, NULL, NULL, NULL,
                         &kernel_entry, &kernel_low, &kernel_high, NULL,
                         ELFDATA2LSB, EM_IA_64, 0, 0,
                         NULL, false, ipf_kernel_sym_cb) < 0) {
        error_report("Unable to load kernel '%s'", kernel_filename);
        exit(1);
    }
    (void)kernel_low;
    (void)kernel_high;
    ipf_kernel_low = kernel_low;
    ipf_kernel_high = kernel_high;
    ipf_kernel_bias = 0xa000000100000000ULL - kernel_low;
    env->kernel_stext = ipf_sym_stext;
    env->kernel_etext = ipf_sym_etext;
    env->kernel_bias = ipf_kernel_bias;
    env->dbg_console_owner_va = ipf_sym_console_owner;
    env->dbg_console_waiter_va = ipf_sym_console_waiter;
    env->dbg_switch_mode_phys_va = ipf_sym_switch_mode_phys;
    env->dbg_switch_mode_virt_va = ipf_sym_switch_mode_virt;
    env->dbg_switch_mode_phys_size = ipf_sym_switch_mode_phys_size;
    env->dbg_switch_mode_virt_size = ipf_sym_switch_mode_virt_size;
    env->dbg_ia64_switch_to_va = ipf_sym_ia64_switch_to;
    env->dbg_ia64_switch_to_size = ipf_sym_ia64_switch_to_size;
    env->dbg_load_switch_stack_va = ipf_sym_load_switch_stack;
    env->dbg_load_switch_stack_size = ipf_sym_load_switch_stack_size;
    ipf_probe_percpu_segment(kernel_filename, cpu);
    {
        /*
         * Bringup sanity: console_srcu.percpu_ref (used in __srcu_read_unlock)
         * should come from the kernel .data image and must not be zero.
         */
        const uint64_t console_srcu_va = 0xa000000101f57678ULL;
        const uint64_t field_va = console_srcu_va + 8;
        const hwaddr field_pa = field_va - ipf_kernel_bias;
        uint64_t val = 0;
        address_space_read(&address_space_memory, field_pa,
                           MEMTXATTRS_UNSPECIFIED, &val, sizeof(val));
        DPRINTF("console_srcu+8: PA=0x%016" HWADDR_PRIx " val=%016" PRIx64 "\n",
                field_pa, (uint64_t)le64_to_cpu(val));
    }
    if (ipf_sym_io_space) {
        ipf_patch_io_space(ipf_sym_io_space, ipf_kernel_bias,
                           machine->ram_size);
    }
    const char *watch = getenv("QEMU_IA64_WATCH_TEXT");
    if (watch && *watch) {
        if ((strcmp(watch, "1") == 0) ||
            (strcmp(watch, "ia64_bad_break") == 0) ||
            (strcmp(watch, "all") == 0)) {
            if (ipf_sym_ia64_bad_break) {
                ipf_add_text_watch(m, sysmem, cpu, machine->ram, 0,
                                   ipf_sym_ia64_bad_break - ipf_kernel_bias,
                                   0x20, "ia64_bad_break");
            } else {
                fprintf(stderr,
                        "IPF_TEXT_WATCH: symbol ia64_bad_break not found\n");
            }
        }
        if ((strcmp(watch, "search_extable") == 0) ||
            (strcmp(watch, "all") == 0)) {
            if (ipf_sym_search_extable) {
                ipf_add_text_watch(m, sysmem, cpu, machine->ram, 0,
                                   ipf_sym_search_extable - ipf_kernel_bias,
                                   0x20, "search_extable");
            } else {
                fprintf(stderr,
                        "IPF_TEXT_WATCH: symbol search_extable not found\n");
            }
        }
    }

    /* Optional RAM watchpoints for bringup debugging. */
    const char *watch_data = getenv("QEMU_IA64_WATCH_DATA");
    const char *watch_data2 = getenv("QEMU_IA64_WATCH_DATA2");
    const struct {
        const char *env;
        const char *label;
    } data_watches[] = {
        { watch_data, "data_watch1" },
        { watch_data2, "data_watch2" },
    };

    for (size_t wi = 0; wi < ARRAY_SIZE(data_watches); wi++) {
        const char *w = data_watches[wi].env;
        if (!w || !*w) {
            continue;
        }
        hwaddr size = 0x20;
        hwaddr pa = 0;
        if (strcmp(w, "console_srcu") == 0 || strcmp(w, "console_srcu+8") == 0) {
            const uint64_t console_srcu_va = 0xa000000101f57678ULL;
            pa = (console_srcu_va + 8) - ipf_kernel_bias;
            ipf_add_text_watch(m, sysmem, cpu, machine->ram, 0, pa, size,
                               data_watches[wi].label);
        } else if (strcmp(w, "console_owner") == 0) {
            if (!ipf_sym_console_owner) {
                fprintf(stderr,
                        "IPF_TEXT_WATCH: symbol console_owner not found\n");
                continue;
            }
            pa = ipf_sym_console_owner - ipf_kernel_bias;
            size = 8;
            ipf_add_text_watch(m, sysmem, cpu, machine->ram, 0, pa, size,
                               "console_owner");
        } else if (strcmp(w, "console_waiter") == 0) {
            if (!ipf_sym_console_waiter) {
                fprintf(stderr,
                        "IPF_TEXT_WATCH: symbol console_waiter not found\n");
                continue;
            }
            pa = ipf_sym_console_waiter - ipf_kernel_bias;
            size = 1;
            ipf_add_text_watch(m, sysmem, cpu, machine->ram, 0, pa, size,
                               "console_waiter");
        } else {
            char *endp = NULL;
            pa = (hwaddr)strtoull(w, &endp, 0);
            if (endp && endp != w) {
                ipf_add_text_watch(m, sysmem, cpu, machine->ram, 0, pa, size,
                                   data_watches[wi].label);
            }
        }
    }

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
        const uint64_t page_size = 4096;
        const uint64_t wb = (1ULL << 3); /* EFI_MEMORY_WB */
        const uint64_t uc = (1ULL << 0); /* EFI_MEMORY_UC */

        /* Reserve low memory containing boot params + EFI tables + SAL/PAL stubs. */
        uint64_t reserve_end = 0x0000000000020000ULL; /* 128KB */
        reserve_end = MAX(reserve_end, IA64_IPF_FW_SAL_GP_ADDR + page_size);
        reserve_end = MIN(reserve_end, machine->ram_size);
        reserve_end = QEMU_ALIGN_UP(reserve_end, page_size);

        uint64_t pal_start = QEMU_ALIGN_DOWN(IA64_IPF_FW_PAL_PROC_ADDR, page_size);
        uint64_t pal_end =
            QEMU_ALIGN_UP(IA64_IPF_FW_PAL_PROC_ADDR + IA64_IPF_FW_PAL_SIZE, page_size);

        /* Reserve the loaded kernel image so bootmem/memblock don't clobber it. */
        uint64_t kern_start = QEMU_ALIGN_DOWN(kernel_low, page_size);
        uint64_t kern_end = QEMU_ALIGN_UP(kernel_high, page_size);
        kern_end = MIN(kern_end, machine->ram_size);

        /* Reserve initrd as well (if present). */
        uint64_t initrd_start = 0;
        uint64_t initrd_end = 0;
        if (initrd_size > 0) {
            initrd_start = QEMU_ALIGN_DOWN(initrd_base, page_size);
            initrd_end = QEMU_ALIGN_UP(initrd_base + initrd_size, page_size);
            initrd_end = MIN(initrd_end, machine->ram_size);
        }

        struct Range {
            uint64_t start;
            uint64_t end;
            uint32_t type;
        } ranges[8];
        size_t nr = 0;

        /*
         * Low-memory layout:
         * - EFI/boot param data lives in EFI_RESERVED_TYPE.
         * - PAL entry is advertised via a dedicated EFI_PAL_CODE descriptor so
         *   Linux installs an ITR mapping for safe PAL calls in virtual mode.
         */
        if (reserve_end > 0) {
            uint64_t lo0 = 0;
            uint64_t lo1 = MIN(pal_start, reserve_end);
            uint64_t lo2 = MIN(pal_end, reserve_end);
            if (lo1 > lo0) {
                ranges[nr++] = (struct Range){ .start = lo0, .end = lo1,
                                               .type = EFI_RESERVED_TYPE };
            }
            if (lo2 > lo1) {
                ranges[nr++] = (struct Range){ .start = lo1, .end = lo2,
                                               .type = EFI_PAL_CODE };
            }
            if (reserve_end > lo2) {
                ranges[nr++] = (struct Range){ .start = lo2, .end = reserve_end,
                                               .type = EFI_RESERVED_TYPE };
            }
        }

        if (kern_end > kern_start) {
            ranges[nr++] = (struct Range){
                .start = kern_start,
                .end = kern_end,
                .type = EFI_RESERVED_TYPE,
            };
        }
        if (initrd_end > initrd_start) {
            ranges[nr++] = (struct Range){
                .start = initrd_start,
                .end = initrd_end,
                .type = EFI_RESERVED_TYPE,
            };
        }

        /* Sort small range list by start. */
        for (size_t i = 0; i + 1 < nr; i++) {
            for (size_t j = i + 1; j < nr; j++) {
                if (ranges[j].start < ranges[i].start) {
                    struct Range tmp = ranges[i];
                    ranges[i] = ranges[j];
                    ranges[j] = tmp;
                }
            }
        }

        /* Merge overlapping ranges of the same type. */
        struct Range merged[8];
        size_t nm = 0;
        for (size_t i = 0; i < nr; i++) {
            struct Range r = ranges[i];
            if (r.end <= r.start) {
                continue;
            }
            if (r.start >= machine->ram_size) {
                continue;
            }
            r.end = MIN(r.end, machine->ram_size);
            if (nm == 0) {
                merged[nm++] = r;
                continue;
            }
            struct Range *last = &merged[nm - 1];
            if (r.start <= last->end && r.type == last->type) {
                last->end = MAX(last->end, r.end);
            } else {
                if (r.start < last->end) {
                    /* Overlap across different types should not happen. Clamp. */
                    r.start = last->end;
                }
                if (r.end > r.start) {
                    merged[nm++] = r;
                }
            }
        }

        /* Emit EFI descriptors alternating conventional and reserved segments. */
        struct efi_memory_desc md[16] = { 0 };
        size_t nd = 0;
        uint64_t cur = 0;
        for (size_t i = 0; i < nm; i++) {
            uint64_t start = merged[i].start;
            uint64_t end = merged[i].end;
            if (cur < start) {
                md[nd++] = (struct efi_memory_desc){
                    .type = EFI_CONVENTIONAL_MEMORY,
                    .phys_addr = cur,
                    .virt_addr = 0,
                    .num_pages = (start - cur) / page_size,
                    .attribute = wb,
                };
            }
            if (end > start) {
                md[nd++] = (struct efi_memory_desc){
                    .type = merged[i].type,
                    .phys_addr = start,
                    .virt_addr = 0,
                    .num_pages = (end - start) / page_size,
                    .attribute = wb,
                };
            }
            cur = end;
        }
        if (cur < machine->ram_size) {
            md[nd++] = (struct efi_memory_desc){
                .type = EFI_CONVENTIONAL_MEMORY,
                .phys_addr = cur,
                .virt_addr = 0,
                .num_pages = (machine->ram_size - cur) / page_size,
                .attribute = wb,
            };
        }

        /*
         * Describe legacy port-I/O space so Linux doesn't fall back to AR.KR0
         * (which defaults to 0 and would alias low RAM).
         */
        md[nd++] = (struct efi_memory_desc){
            .type = EFI_MEMORY_MAPPED_IO_PORT_SPACE,
            .phys_addr = IPF_LEGACY_IO_BASE,
            .virt_addr = 0,
            .num_pages = IPF_LEGACY_IO_SIZE / page_size,
            .attribute = uc,
        };

        address_space_write(&address_space_memory, IPF_EFI_MEMMAP_ADDR,
                            MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)md, sizeof(md[0]) * nd);
        bp.efi_memmap = IPF_EFI_MEMMAP_ADDR;
        bp.efi_memmap_size = sizeof(md[0]) * nd;
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
        /*
         * IA-64 uses function descriptors: a function pointer is a pointer to
         * a 16-byte descriptor { entry, gp }. Linux's efi_call_phys() will
         * load both qwords and branch to entry with r1=gp.
         *
         * Therefore, runtime service fields must point at descriptors, not
         * directly at the code.
         */
        struct QEMU_PACKED IPFEfiFuncDesc {
            uint64_t entry;
            uint64_t gp;
        };

        /* Linux __va() uses region 7 as the direct physical map. */
        const uint64_t rgn7_base = 7ULL << 61;

        uint64_t stub_ok_code = IPF_EFI_STUBS_ADDR;
        uint64_t stub_unsupported_code = stub_ok_code +
                                         sizeof(ipf_efi_stub_set_virtual_address_map);
        uint64_t fdesc_base = stub_unsupported_code +
                              sizeof(ipf_efi_stub_unsupported);
        fdesc_base = QEMU_ALIGN_UP(fdesc_base, 16);
        uint64_t stub_ok_desc = fdesc_base;
        uint64_t stub_unsupported_desc = fdesc_base + sizeof(struct IPFEfiFuncDesc);
        uint64_t stub_ok_code_va = rgn7_base | stub_ok_code;
        uint64_t stub_unsupported_code_va = rgn7_base | stub_unsupported_code;
        uint64_t stub_ok_desc_va = rgn7_base | stub_ok_desc;
        uint64_t stub_unsupported_desc_va = rgn7_base | stub_unsupported_desc;

        address_space_write(&address_space_memory, stub_ok_code, MEMTXATTRS_UNSPECIFIED,
                            ipf_efi_stub_set_virtual_address_map,
                            sizeof(ipf_efi_stub_set_virtual_address_map));
        address_space_write(&address_space_memory, stub_unsupported_code, MEMTXATTRS_UNSPECIFIED,
                            ipf_efi_stub_unsupported,
                            sizeof(ipf_efi_stub_unsupported));

        struct IPFEfiFuncDesc ok_desc = { .entry = stub_ok_code_va, .gp = 0 };
        struct IPFEfiFuncDesc unsup_desc = { .entry = stub_unsupported_code_va, .gp = 0 };
        address_space_write(&address_space_memory, stub_ok_desc, MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)&ok_desc, sizeof(ok_desc));
        address_space_write(&address_space_memory, stub_unsupported_desc, MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)&unsup_desc, sizeof(unsup_desc));

        IPFEfiRuntimeServices rt = { 0 };
        rt.hdr.signature = EFI_RUNTIME_SERVICES_SIGNATURE;
        rt.hdr.revision = EFI_RUNTIME_SERVICES_REVISION;
        rt.hdr.headersize = sizeof(rt);
        rt.set_virtual_address_map = stub_ok_desc_va;
        rt.get_time = stub_unsupported_desc_va;
        rt.set_time = stub_unsupported_desc_va;
        rt.get_wakeup_time = stub_unsupported_desc_va;
        rt.set_wakeup_time = stub_unsupported_desc_va;
        rt.get_variable = stub_unsupported_desc_va;
        rt.get_next_variable = stub_unsupported_desc_va;
        rt.set_variable = stub_unsupported_desc_va;
        rt.get_next_high_mono_count = stub_unsupported_desc_va;
        rt.reset_system = stub_unsupported_desc_va;
        rt.update_capsule = stub_unsupported_desc_va;
        rt.query_capsule_caps = stub_unsupported_desc_va;
        rt.query_variable_info = stub_unsupported_desc_va;

        address_space_write(&address_space_memory, IPF_EFI_RUNTIME_ADDR,
                            MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)&rt, sizeof(rt));

        /* Vendor string (UCS-2/UTF-16) for efi_systab_report_header(). */
        static const uint16_t vendor[] = { 'Q', 'E', 'M', 'U', 0 };
        address_space_write(&address_space_memory, IPF_EFI_VENDOR_ADDR,
                            MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)vendor, sizeof(vendor));

        /* HCDP/PCDP table with one primary MMIO UART at 0xff5e0000. */
        IPFPcdpTable pcdp = { 0 };
        memcpy(pcdp.hdr.signature, "PCDP", 4);
        pcdp.hdr.rev = 3; /* PCDP v2.0 */
        pcdp.hdr.num_uarts = 1;
        pcdp.uart0.type = 0; /* PCDP_CONSOLE_UART */
        pcdp.uart0.bits = 8;
        pcdp.uart0.parity = 0;
        pcdp.uart0.stop_bits = 1;
        pcdp.uart0.baud = 115200;
        pcdp.uart0.addr.space_id = 0; /* ACPI_ADR_SPACE_SYSTEM_MEMORY */
        pcdp.uart0.addr.bit_width = 8;
        pcdp.uart0.addr.bit_offset = 0;
        pcdp.uart0.addr.access_width = 1;
        pcdp.uart0.addr.address = IPF_UART_BASE;
        pcdp.uart0.flags = (1U << 2); /* PCDP_UART_PRIMARY_CONSOLE */
        pcdp.hdr.length = sizeof(pcdp);
        pcdp.hdr.chksum = ipf_byte_checksum(&pcdp, sizeof(pcdp));
        address_space_write(&address_space_memory, IPF_EFI_PCDP_ADDR,
                            MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)&pcdp, sizeof(pcdp));

        /* SAL system table (SST_) with an entrypoint descriptor for PAL+SAL. */
        ipf_write_sal_systab();

        /* ACPI 2.0 system description tables. */
        hwaddr acpi_rsdp_addr = 0;
        {
            /*
             * Place ACPI tables in reserved low memory, after the synthetic
             * SAL/PAL stubs.
             */
            hwaddr cur = IA64_IPF_FW_SAL_GP_ADDR + 0x1000;
            cur = QEMU_ALIGN_UP(cur, 16);

            hwaddr rsdp_addr = cur;
            cur = QEMU_ALIGN_UP(cur + sizeof(IPFAcpiRsdp), 16);

            hwaddr rsdt_addr = cur;
            uint8_t rsdt_buf[sizeof(IPFAcpiTableHeader) + 2 * sizeof(uint32_t)] = { 0 };
            cur = QEMU_ALIGN_UP(cur + sizeof(rsdt_buf), 16);

            hwaddr xsdt_addr = cur;
            uint8_t xsdt_buf[sizeof(IPFAcpiTableHeader) + 2 * sizeof(uint64_t)] = { 0 };
            cur = QEMU_ALIGN_UP(cur + sizeof(xsdt_buf), 16);

            hwaddr fadt_addr = cur;
            IPFAcpiFadt fadt = { 0 };
            cur = QEMU_ALIGN_UP(cur + sizeof(fadt), 16);

            /*
             * DSDT AML:
             * - Scope(\_SB) { Device(COM0) { _HID="PNP0501" ... Memory32Fixed(0xff5e0000, 8) } }
             *
             * This allows Linux's PNP/8250 code to enumerate the serial-mm UART
             * and enables a real tty console (ttyS0) when requested via
             * "console=ttyS0".
             *
             * Generated with iasl from:
             *   Scope(\\_SB){Device(COM0){Name(_HID,"PNP0501") Name(_UID,0)
             *     Method(_STA){Return(0x0f)}
             *     Name(_CRS,ResourceTemplate(){Memory32Fixed(ReadWrite,0xFF5E0000,8)})}}
             */
            static const uint8_t dsdt_aml[] = {
                0x10, 0x42, 0x04, 0x5f, 0x53, 0x42, 0x5f, 0x5b, 0x82, 0x3a, 0x43, 0x4f,
                0x4d, 0x30, 0x08, 0x5f, 0x48, 0x49, 0x44, 0x0d, 0x50, 0x4e, 0x50, 0x30,
                0x35, 0x30, 0x31, 0x00, 0x08, 0x5f, 0x55, 0x49, 0x44, 0x00, 0x14, 0x09,
                0x5f, 0x53, 0x54, 0x41, 0x00, 0xa4, 0x0a, 0x0f, 0x08, 0x5f, 0x43, 0x52,
                0x53, 0x11, 0x11, 0x0a, 0x0e, 0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x5e,
                0xff, 0x08, 0x00, 0x00, 0x00, 0x79, 0x00,
            };
            hwaddr dsdt_addr = cur;
            uint8_t dsdt_buf[sizeof(IPFAcpiTableHeader) + sizeof(dsdt_aml)] = { 0 };
            cur = QEMU_ALIGN_UP(cur + sizeof(dsdt_buf), 16);

            hwaddr madt_addr = cur;
            uint8_t madt_buf[sizeof(IPFAcpiMadt) +
                             sizeof(IPFAcpiMadtLocalSapic) +
                             sizeof(IPFAcpiMadtIoSapic)] = { 0 };
            cur = QEMU_ALIGN_UP(cur + sizeof(madt_buf), 16);

            if (cur > 0x0000000000020000ULL || cur > machine->ram_size) {
                error_report("RAM too small for IA-64 ACPI tables");
                exit(1);
            }

            /* DSDT */
            {
                IPFAcpiTableHeader *hdr = (IPFAcpiTableHeader *)dsdt_buf;
                ipf_acpi_init_header(hdr, "DSDT", sizeof(dsdt_buf), 2);
                memcpy(dsdt_buf + sizeof(*hdr), dsdt_aml, sizeof(dsdt_aml));
                hdr->checksum = ipf_byte_checksum(dsdt_buf, sizeof(dsdt_buf));
                address_space_write(&address_space_memory, dsdt_addr,
                                    MEMTXATTRS_UNSPECIFIED,
                                    dsdt_buf, sizeof(dsdt_buf));
            }

            /* FADT (revision 3 required by Linux/ia64) */
            ipf_acpi_init_header(&fadt.header, "FACP", sizeof(fadt), 3);
            fadt.dsdt = cpu_to_le32((uint32_t)dsdt_addr);
            fadt.Xdsdt = cpu_to_le64(dsdt_addr);
            fadt.sci_interrupt = cpu_to_le16(9);
            /*
             * Fixed-feature register blocks.
             *
             * On IA-64, Linux expects these blocks to be described via Generic
             * Address Structures when they are MMIO.  The legacy 32-bit fields
             * are defined as System I/O port addresses; avoid populating them
             * with MMIO addresses above 64K.
             */
            fadt.pm1a_event_block = 0;
            fadt.pm1_event_length = 4;
            fadt.pm1a_control_block = 0;
            fadt.pm1_control_length = 2;
            fadt.pm_timer_block = 0;
            fadt.pm_timer_length = 4;

            /* Extended (GAS) equivalents. */
            fadt.xpm1a_event_block.space_id = 0; /* ACPI_ADR_SPACE_SYSTEM_MEMORY */
            fadt.xpm1a_event_block.bit_width = 32;
            fadt.xpm1a_event_block.access_width = 0;
            fadt.xpm1a_event_block.address = cpu_to_le64(IPF_ACPI_PM_BASE);
            fadt.xpm1a_control_block.space_id = 0;
            fadt.xpm1a_control_block.bit_width = 16;
            fadt.xpm1a_control_block.access_width = 0;
            fadt.xpm1a_control_block.address = cpu_to_le64(IPF_ACPI_PM_BASE + 0x04);
            fadt.xpm_timer_block.space_id = 0;
            fadt.xpm_timer_block.bit_width = 32;
            fadt.xpm_timer_block.access_width = 0;
            fadt.xpm_timer_block.address = cpu_to_le64(IPF_ACPI_PM_BASE + 0x08);
            fadt.header.checksum = ipf_byte_checksum(&fadt, sizeof(fadt));
            address_space_write(&address_space_memory, fadt_addr,
                                MEMTXATTRS_UNSPECIFIED,
                                (const uint8_t *)&fadt, sizeof(fadt));

            /* MADT ("APIC") with one Local SAPIC CPU and one IOSAPIC. */
            {
                IPFAcpiMadt madt = { 0 };
                IPFAcpiMadtLocalSapic lsapic = { 0 };
                IPFAcpiMadtIoSapic iosapic = { 0 };

                ipf_acpi_init_header(&madt.header, "APIC", sizeof(madt_buf), 2);
                madt.address = cpu_to_le32(0xfee00000U); /* IA64_IPI_DEFAULT_BASE_ADDR */
                madt.flags = cpu_to_le32(1);             /* ACPI_MADT_PCAT_COMPAT */

                lsapic.header.type = 7; /* ACPI_MADT_TYPE_LOCAL_SAPIC */
                lsapic.header.length = sizeof(lsapic);
                lsapic.processor_id = 0;
                lsapic.id = 0;
                lsapic.eid = 0;
                lsapic.lapic_flags = cpu_to_le32(1); /* ACPI_MADT_ENABLED */
                lsapic.uid = cpu_to_le32(0);

                iosapic.header.type = 6; /* ACPI_MADT_TYPE_IO_SAPIC */
                iosapic.header.length = sizeof(iosapic);
                iosapic.id = 0;
                iosapic.global_irq_base = cpu_to_le32(0);
                iosapic.address = cpu_to_le64(IPF_IOSAPIC_BASE);

                memcpy(madt_buf, &madt, sizeof(madt));
                memcpy(madt_buf + sizeof(madt), &lsapic, sizeof(lsapic));
                memcpy(madt_buf + sizeof(madt) + sizeof(lsapic),
                       &iosapic, sizeof(iosapic));
                ((IPFAcpiMadt *)madt_buf)->header.checksum =
                    ipf_byte_checksum(madt_buf, sizeof(madt_buf));
                address_space_write(&address_space_memory, madt_addr,
                                    MEMTXATTRS_UNSPECIFIED,
                                    madt_buf, sizeof(madt_buf));
            }

            /* XSDT (FADT + MADT). */
            {
                IPFAcpiTableHeader *hdr = (IPFAcpiTableHeader *)xsdt_buf;
                ipf_acpi_init_header(hdr, "XSDT", sizeof(xsdt_buf), 1);
                uint64_t ent;
                ent = cpu_to_le64(fadt_addr);
                memcpy(xsdt_buf + sizeof(*hdr) + 0 * sizeof(ent), &ent, sizeof(ent));
                ent = cpu_to_le64(madt_addr);
                memcpy(xsdt_buf + sizeof(*hdr) + 1 * sizeof(ent), &ent, sizeof(ent));
                hdr->checksum = ipf_byte_checksum(xsdt_buf, sizeof(xsdt_buf));
                address_space_write(&address_space_memory, xsdt_addr,
                                    MEMTXATTRS_UNSPECIFIED,
                                    xsdt_buf, sizeof(xsdt_buf));
            }

            /* RSDT (FADT + MADT) for compatibility. */
            {
                IPFAcpiTableHeader *hdr = (IPFAcpiTableHeader *)rsdt_buf;
                ipf_acpi_init_header(hdr, "RSDT", sizeof(rsdt_buf), 1);
                uint32_t ent32;
                ent32 = cpu_to_le32((uint32_t)fadt_addr);
                memcpy(rsdt_buf + sizeof(*hdr) + 0 * sizeof(ent32), &ent32, sizeof(ent32));
                ent32 = cpu_to_le32((uint32_t)madt_addr);
                memcpy(rsdt_buf + sizeof(*hdr) + 1 * sizeof(ent32), &ent32, sizeof(ent32));
                hdr->checksum = ipf_byte_checksum(rsdt_buf, sizeof(rsdt_buf));
                address_space_write(&address_space_memory, rsdt_addr,
                                    MEMTXATTRS_UNSPECIFIED,
                                    rsdt_buf, sizeof(rsdt_buf));
            }

            /* RSDP (ACPI 2.0). */
            {
                IPFAcpiRsdp rsdp = { 0 };
                memcpy(rsdp.signature, "RSD PTR ", 8);
                memcpy(rsdp.oem_id, "QEMU  ", 6);
                rsdp.revision = 2;
                rsdp.rsdt_address = cpu_to_le32((uint32_t)rsdt_addr);
                rsdp.length = cpu_to_le32(sizeof(rsdp));
                rsdp.xsdt_address = cpu_to_le64(xsdt_addr);
                rsdp.checksum = 0;
                rsdp.extended_checksum = 0;
                rsdp.checksum = ipf_byte_checksum(&rsdp, 20);
                rsdp.extended_checksum = ipf_byte_checksum(&rsdp, sizeof(rsdp));
                address_space_write(&address_space_memory, rsdp_addr,
                                    MEMTXATTRS_UNSPECIFIED,
                                    (const uint8_t *)&rsdp, sizeof(rsdp));
            }

            acpi_rsdp_addr = rsdp_addr;
        }

        /* EFI configuration tables (HCDP + SAL + ACPI). */
        IPFEfiConfigTable conf[3] = { 0 };
        conf[0].guid = ipf_guid_hcdp;
        conf[0].table = IPF_EFI_PCDP_ADDR;
        conf[1].guid = ipf_guid_sal_systab;
        conf[1].table = IA64_IPF_FW_SAL_SYSTAB_ADDR;
        conf[2].guid = ipf_guid_acpi_20;
        conf[2].table = acpi_rsdp_addr;
        address_space_write(&address_space_memory, IPF_EFI_CONFTAB_ADDR,
                            MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)conf, sizeof(conf));

        IPFEfiSystemTable st = { 0 };
        st.hdr.signature = EFI_SYSTEM_TABLE_SIGNATURE;
        st.hdr.revision = 0x00010000;
        st.hdr.headersize = sizeof(st);
        st.fw_vendor = IPF_EFI_VENDOR_ADDR;
        st.fw_revision = 1;
        st.runtime = IPF_EFI_RUNTIME_ADDR;
        st.nr_tables = ARRAY_SIZE(conf);
        st.tables = IPF_EFI_CONFTAB_ADDR;

        address_space_write(&address_space_memory, IPF_EFI_SYSTAB_ADDR,
                            MEMTXATTRS_UNSPECIFIED,
                            (const uint8_t *)&st, sizeof(st));

        bp.efi_systab = IPF_EFI_SYSTAB_ADDR;
    }

    address_space_write(&address_space_memory, IPF_BOOT_PARAM_ADDR, MEMTXATTRS_UNSPECIFIED,
                        (const uint8_t *)&bp, sizeof(bp));

    /*
     * Boot in physical mode.
     *
     * Default: enter the ELF entry point directly (classic -kernel boot).
     * Optional: run guest firmware first and hand off to the kernel once the
     * firmware returns (Xen/KVM GFW-style preboot).
     */
    ipf_boot_r28 = IPF_BOOT_PARAM_ADDR;
    if (m->firmware_preboot) {
        if (image_size <= 0) {
            error_report("IPF firmware-preboot requires a valid -bios");
            exit(1);
        }
        env->fw_preboot_active = 1;
        env->fw_preboot_ip = kernel_entry;
        env->fw_preboot_r28 = ipf_boot_r28;
        env->fw_preboot_kernel_low = kernel_low;
        ipf_boot_r28 = GFW_HOB_START;
        ipf_boot_ip = IPF_GFW_ENTRY;
        DPRINTF("Firmware-preboot enabled: will hand off to kernel entry 0x%" PRIx64 "\n",
                kernel_entry);
    } else {
        ipf_boot_ip = kernel_entry;
        env->ip = ipf_boot_ip;
        env->r[28] = ipf_boot_r28;
    }
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

static bool ipf_machine_get_firmware_preboot(Object *obj, Error **errp)
{
    (void)errp;
    return IPF_MACHINE(obj)->firmware_preboot;
}

static void ipf_machine_set_firmware_preboot(Object *obj, bool value,
                                              Error **errp)
{
    (void)errp;
    IPF_MACHINE(obj)->firmware_preboot = value;
}

static void ipf_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Itanium Platform";
    mc->init = ipf_init;
    mc->block_default_type = IF_IDE;
    mc->default_ram_size = 256 * 1024 * 1024;
    mc->default_ram_id = "ipf.ram";
    mc->default_cpu_type = IA64_CPU_TYPE_NAME("itanium");
    /*
     * Xen/EDK guest firmware expects a plain VGA-compatible device. Prefer the
     * standard VGA model over cirrus unless the user overrides with -vga.
     */
    mc->default_display = "std";
    mc->is_default = true;

    object_class_property_add_bool(oc, "firmware-preboot",
                                   ipf_machine_get_firmware_preboot,
                                   ipf_machine_set_firmware_preboot);
    object_class_property_set_description(
        oc, "firmware-preboot",
        "Run guest firmware first and then boot -kernel (GFW handoff)");
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
    type_register_static(&ipf_pcihost_info);
    type_register_static(&ipf_pci_root_info);

}

type_init(ipf_register_type)


// XXX: pop the pragmas
#pragma GCC diagnostic pop
