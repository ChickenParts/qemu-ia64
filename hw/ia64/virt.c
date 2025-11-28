/*
 * IA-64 Virt Machine
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/system.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "hw/cpu/cluster.h"
#include "target/ia64/cpu.h"

static void ia64_virt_init(MachineState *machine)
{
    MemoryRegion *ram = machine->ram;
    MemoryRegion *bios = g_new(MemoryRegion, 1);
    
    memory_region_add_subregion(get_system_memory(), 0, ram);
    
    if (machine->firmware) {
        char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        if (!filename) {
            error_report("Could not find bios file '%s'", machine->firmware);
            exit(1);
        }
        
        long size = get_image_size(filename);
        if (size < 0) {
            error_report("Could not get size of bios file '%s'", filename);
            exit(1);
        }
        
        memory_region_init_ram(bios, NULL, "ia64.bios", size, &error_fatal);
        memory_region_set_readonly(bios, true);
        memory_region_add_subregion(get_system_memory(), 0x100000000ULL - size, bios);
        
        if (load_image_targphys(filename, 0x100000000ULL - size, size) < 0) {
            error_report("Could not load bios file '%s'", filename);
            exit(1);
        }
        g_free(filename);
    }
    
    cpu_create(machine->cpu_type);
}

static void ia64_virt_machine_init(MachineClass *mc)
{
    mc->desc = "IA-64 Virtual Machine";
    mc->init = ia64_virt_init;
    mc->default_cpu_type = IA64_CPU_TYPE_NAME("itanium");
    mc->default_ram_id = "ram";
    mc->default_ram_size = 1 * GiB;
}

DEFINE_MACHINE("virt", ia64_virt_machine_init)
