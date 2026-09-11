# IA-64 HOB/PEI hook map

Generated from the `ia64-rooster-bringup` branch.  Entries are source locations, not a recommendation to keep firmware-specific compatibility switches.

## `hw/ia64/ipf.c`

| Line | Enclosing function | Match |
|---:|---|---|
| 337 | `OBJECT_DECLARE_SIMPLE_TYPE` | `static uint64_t ipf_boot_findfv_stub;` |
| 338 | `OBJECT_DECLARE_SIMPLE_TYPE` | `static uint64_t ipf_boot_findfv_iface;` |
| 510 | `DPRINTF` | `* The Xen/KVM IA-64 guest firmware (and our HOB builder in hw/ia64/gfw.c)` |
| 536 | `DPRINTF` | `* ar.k3 is 3. Keep the PEI temp RAM/HOB list in that window so PEI HOBs` |
| 537 | `DPRINTF` | `* don't clobber the Xen GFW HOB list at 0xff200000.` |
| 546 | `DPRINTF` | `} IPFGfwHobHeader;` |
| 549 | `DPRINTF` | `IPFGfwHobHeader header;` |
| 553 | `DPRINTF` | `} IPFGfwHobInfo;` |
| 558 | `DPRINTF` | `} IPFGfwHobMem;` |
| 561 | `DPRINTF` | `IPF_HOB_TYPE_INFO = 0,` |
| 562 | `DPRINTF` | `IPF_HOB_TYPE_TERMINAL,` |
| 563 | `DPRINTF` | `IPF_HOB_TYPE_MEM,` |
| 564 | `DPRINTF` | `IPF_HOB_TYPE_PAL_BUS_GET_FEATURES_DATA,` |
| 565 | `DPRINTF` | `IPF_HOB_TYPE_PAL_CACHE_SUMMARY,` |
| 566 | `DPRINTF` | `IPF_HOB_TYPE_PAL_MEM_ATTRIB,` |
| 567 | `DPRINTF` | `IPF_HOB_TYPE_PAL_CACHE_INFO,` |
| 568 | `DPRINTF` | `IPF_HOB_TYPE_PAL_CACHE_PROT_INFO,` |
| 569 | `DPRINTF` | `IPF_HOB_TYPE_PAL_DEBUG_INFO,` |
| 570 | `DPRINTF` | `IPF_HOB_TYPE_PAL_FIXED_ADDR,` |
| 571 | `DPRINTF` | `IPF_HOB_TYPE_PAL_FREQ_BASE,` |
| 572 | `DPRINTF` | `IPF_HOB_TYPE_PAL_FREQ_RATIOS,` |
| 573 | `DPRINTF` | `IPF_HOB_TYPE_PAL_HALT_INFO,` |
| 574 | `DPRINTF` | `IPF_HOB_TYPE_PAL_PERF_MON_INFO,` |
| 575 | `DPRINTF` | `IPF_HOB_TYPE_PAL_PROC_GET_FEATURES,` |
| 576 | `DPRINTF` | `IPF_HOB_TYPE_PAL_PTCE_INFO,` |
| 577 | `DPRINTF` | `IPF_HOB_TYPE_PAL_REGISTER_INFO,` |
| 578 | `DPRINTF` | `IPF_HOB_TYPE_PAL_RSE_INFO,` |
| 579 | `DPRINTF` | `IPF_HOB_TYPE_PAL_TEST_INFO,` |
| 580 | `DPRINTF` | `IPF_HOB_TYPE_PAL_VM_SUMMARY,` |
| 581 | `DPRINTF` | `IPF_HOB_TYPE_PAL_VM_INFO,` |
| 582 | `DPRINTF` | `IPF_HOB_TYPE_PAL_VM_PAGE_SIZE,` |
| 583 | `DPRINTF` | `IPF_HOB_TYPE_NR_VCPU,` |
| 584 | `DPRINTF` | `IPF_HOB_TYPE_NR_NVRAM,` |
| 585 | `DPRINTF` | `IPF_HOB_TYPE_MAX` |
| 588 | `DPRINTF` | `static const char *ipf_gfw_hob_type_name(uint32_t type)` |
| 591 | `switch` | `case IPF_HOB_TYPE_INFO: return "INFO";` |
| 592 | `switch` | `case IPF_HOB_TYPE_TERMINAL: return "TERMINAL";` |
| 593 | `switch` | `case IPF_HOB_TYPE_MEM: return "MEM";` |
| 594 | `switch` | `case IPF_HOB_TYPE_PAL_BUS_GET_FEATURES_DATA: return "PAL_BUS_FEATURES";` |
| 595 | `switch` | `case IPF_HOB_TYPE_PAL_CACHE_SUMMARY: return "PAL_CACHE_SUMMARY";` |
| 596 | `switch` | `case IPF_HOB_TYPE_PAL_MEM_ATTRIB: return "PAL_MEM_ATTRIB";` |
| 597 | `switch` | `case IPF_HOB_TYPE_PAL_CACHE_INFO: return "PAL_CACHE_INFO";` |
| 598 | `switch` | `case IPF_HOB_TYPE_PAL_CACHE_PROT_INFO: return "PAL_CACHE_PROT_INFO";` |
| 599 | `switch` | `case IPF_HOB_TYPE_PAL_DEBUG_INFO: return "PAL_DEBUG_INFO";` |
| 600 | `switch` | `case IPF_HOB_TYPE_PAL_FIXED_ADDR: return "PAL_FIXED_ADDR";` |
| 601 | `switch` | `case IPF_HOB_TYPE_PAL_FREQ_BASE: return "PAL_FREQ_BASE";` |
| 602 | `switch` | `case IPF_HOB_TYPE_PAL_FREQ_RATIOS: return "PAL_FREQ_RATIOS";` |
| 603 | `switch` | `case IPF_HOB_TYPE_PAL_HALT_INFO: return "PAL_HALT_INFO";` |
| 604 | `switch` | `case IPF_HOB_TYPE_PAL_PERF_MON_INFO: return "PAL_PERF_MON_INFO";` |
| 605 | `switch` | `case IPF_HOB_TYPE_PAL_PROC_GET_FEATURES: return "PAL_PROC_FEATURES";` |
| 606 | `switch` | `case IPF_HOB_TYPE_PAL_PTCE_INFO: return "PAL_PTCE_INFO";` |
| 607 | `switch` | `case IPF_HOB_TYPE_PAL_REGISTER_INFO: return "PAL_REGISTER_INFO";` |
| 608 | `switch` | `case IPF_HOB_TYPE_PAL_RSE_INFO: return "PAL_RSE_INFO";` |
| 609 | `switch` | `case IPF_HOB_TYPE_PAL_TEST_INFO: return "PAL_TEST_INFO";` |
| 610 | `switch` | `case IPF_HOB_TYPE_PAL_VM_SUMMARY: return "PAL_VM_SUMMARY";` |
| 611 | `switch` | `case IPF_HOB_TYPE_PAL_VM_INFO: return "PAL_VM_INFO";` |
| 612 | `switch` | `case IPF_HOB_TYPE_PAL_VM_PAGE_SIZE: return "PAL_VM_PAGE_SIZE";` |
| 613 | `switch` | `case IPF_HOB_TYPE_NR_VCPU: return "NR_VCPU";` |
| 614 | `switch` | `case IPF_HOB_TYPE_NR_NVRAM: return "NR_NVRAM";` |
| 615 | `switch` | `case IPF_HOB_TYPE_MAX: return "MAX";` |
| 620 | `ipf_dump_gfw_hob` | `static void ipf_dump_gfw_hob(const char *tag)` |
| 622 | `ipf_dump_gfw_hob` | `const char *dump_env = getenv("QEMU_IPF_DUMP_HOB");` |
| 627 | `if` | `IPFGfwHobInfo info;` |
| 628 | `if` | `if (address_space_read(&address_space_memory, GFW_HOB_START,` |
| 632 | `qemu_log_mask` | `"IPF: HOB dump: read failed at 0x%016" PRIx64 "\n",` |
| 633 | `qemu_log_mask` | `(uint64_t)GFW_HOB_START);` |
| 640 | `qemu_log_mask` | `uint64_t hob_len = le64_to_cpu(info.length);` |
| 641 | `qemu_log_mask` | `uint64_t hob_buf_size = le64_to_cpu(info.buf_size);` |
| 644 | `qemu_log_mask` | `"IPF: HOB dump(%s): sig=%016" PRIx64 " type=%u len=%" PRIu64` |
| 646 | `qemu_log_mask` | `tag ? tag : "boot", sig, type, hob_len, hdr_len, hob_buf_size,` |
| 647 | `qemu_log_mask` | `(uint64_t)GFW_HOB_START);` |
| 649 | `if` | `if (sig != HOB_SIGNATURE \|\| hob_len == 0 \|\| hob_len > GFW_HOB_SIZE) {` |
| 651 | `qemu_log_mask` | `"IPF: HOB dump: invalid header (sig/len)\n");` |
| 655 | `qemu_log_mask` | `g_autofree uint8_t *buf = g_malloc((size_t)hob_len);` |
| 656 | `if` | `if (address_space_read(&address_space_memory, GFW_HOB_START,` |
| 658 | `if` | `(size_t)hob_len) != MEMTX_OK) {` |
| 660 | `qemu_log_mask` | `"IPF: HOB dump: bulk read failed\n");` |
| 667 | `snprintf` | `"scratch/ia64_logs/gfw_hob_%s.bin",` |
| 671 | `if` | `fwrite(buf, 1, (size_t)hob_len, fp);` |
| 674 | `qemu_log_mask` | `"IPF: HOB dump: wrote %s (%" PRIu64 " bytes)\n",` |
| 675 | `qemu_log_mask` | `path, hob_len);` |
| 679 | `while` | `while (off + sizeof(IPFGfwHobHeader) <= hob_len) {` |
| 680 | `while` | `const IPFGfwHobHeader *hdr = (const IPFGfwHobHeader *)(buf + off);` |
| 684 | `if` | `if (hs != HOB_SIGNATURE \|\| hl < sizeof(IPFGfwHobHeader)) {` |
| 686 | `qemu_log_mask` | `"IPF: HOB dump: bad entry off=0x%04" PRIx64` |
| 693 | `qemu_log_mask` | `"IPF: HOB[%02" PRIu64 "] type=%u (%s) len=%u\n",` |
| 694 | `qemu_log_mask` | `off, ht, ipf_gfw_hob_type_name(ht), hl);` |
| 696 | `if` | `if (ht == IPF_HOB_TYPE_MEM && hl >= sizeof(IPFGfwHobHeader) +` |
| 697 | `sizeof` | `sizeof(IPFGfwHobMem)) {` |
| 698 | `sizeof` | `const IPFGfwHobMem *mem = (const IPFGfwHobMem *)(buf + off +` |
| 699 | `sizeof` | `sizeof(IPFGfwHobHeader));` |
| 703 | `qemu_log_mask` | `"IPF: HOB MEM start=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",` |
| 705 | `qemu_log_mask` | `} else if (ht == IPF_HOB_TYPE_NR_VCPU \|\|` |
| 706 | `qemu_log_mask` | `ht == IPF_HOB_TYPE_NR_NVRAM \|\|` |
| 707 | `qemu_log_mask` | `ht == IPF_HOB_TYPE_MAX) {` |
| 708 | `if` | `if (hl >= sizeof(IPFGfwHobHeader) + sizeof(uint64_t)) {` |
| 709 | `if` | `uint64_t val = ldq_le_p(buf + off + sizeof(IPFGfwHobHeader));` |
| 711 | `qemu_log_mask` | `"IPF: HOB %s value=0x%016" PRIx64 "\n",` |
| 712 | `qemu_log_mask` | `ipf_gfw_hob_type_name(ht), val);` |
| 717 | `if` | `if (ht == IPF_HOB_TYPE_TERMINAL) {` |
| 1529 | `qemu_log_mask` | `ipf_boot_findfv_stub = 0;` |
| 1530 | `qemu_log_mask` | `ipf_boot_findfv_iface = 0;` |
| 1567 | `qemu_log_mask` | `const uint64_t findfv_stub_phys = stub_phys + 0x60;` |
| 1568 | `qemu_log_mask` | `const uint64_t findfv_plabel_phys = findfv_stub_phys + 0x20;` |
| 1569 | `qemu_log_mask` | `const uint64_t findfv_iface_phys = findfv_stub_phys + 0x40;` |
| 1570 | `qemu_log_mask` | `const uint64_t secinfo_stub_phys = findfv_stub_phys + 0x60;` |
| 1594 | `qemu_log_mask` | `static const uint8_t findfv_stub[16] = {` |
| 1598 | `qemu_log_mask` | `static const uint8_t findfv_guid[16] = {` |
| 1673 | `if` | `} findfv_plabel = {` |
| 1674 | `if` | `.entry = cpu_to_le64(ipf_fw_region8_addr(findfv_stub_phys)),` |
| 1677 | `if` | `cpu_physical_memory_write(findfv_stub_phys, findfv_stub, sizeof(findfv_stub));` |
| 1678 | `cpu_physical_memory_write` | `cpu_physical_memory_write(findfv_plabel_phys,` |
| 1679 | `cpu_physical_memory_write` | `(const uint8_t *)&findfv_plabel,` |
| 1680 | `cpu_physical_memory_write` | `sizeof(findfv_plabel));` |
| 1681 | `cpu_physical_memory_write` | `cpu_flush_icache_range(findfv_stub_phys, sizeof(findfv_stub));` |
| 1690 | `cpu_physical_memory_write` | `cpu_physical_memory_write(secinfo_stub_phys, findfv_stub,` |
| 1691 | `cpu_physical_memory_write` | `sizeof(findfv_stub));` |
| 1695 | `cpu_physical_memory_write` | `cpu_flush_icache_range(secinfo_stub_phys, sizeof(findfv_stub));` |
| 1704 | `cpu_physical_memory_write` | `cpu_physical_memory_write(memmap_stub_phys, findfv_stub,` |
| 1705 | `cpu_physical_memory_write` | `sizeof(findfv_stub));` |
| 1709 | `cpu_physical_memory_write` | `cpu_flush_icache_range(memmap_stub_phys, sizeof(findfv_stub));` |
| 1718 | `cpu_physical_memory_write` | `cpu_physical_memory_write(security_stub_phys, findfv_stub,` |
| 1719 | `cpu_physical_memory_write` | `sizeof(findfv_stub));` |
| 1723 | `cpu_physical_memory_write` | `cpu_flush_icache_range(security_stub_phys, sizeof(findfv_stub));` |
| 1732 | `cpu_physical_memory_write` | `cpu_physical_memory_write(loadfile_stub_phys, findfv_stub,` |
| 1733 | `cpu_physical_memory_write` | `sizeof(findfv_stub));` |
| 1737 | `cpu_physical_memory_write` | `cpu_flush_icache_range(loadfile_stub_phys, sizeof(findfv_stub));` |
| 1747 | `cpu_physical_memory_write` | `uint64_t findfv_iface = cpu_to_le64(ipf_fw_region8_addr(findfv_plabel_phys));` |
| 1748 | `cpu_physical_memory_write` | `cpu_physical_memory_write(findfv_iface_phys,` |
| 1749 | `cpu_physical_memory_write` | `(const uint8_t *)&findfv_iface,` |
| 1750 | `cpu_physical_memory_write` | `sizeof(findfv_iface));` |
| 1769 | `cpu_physical_memory_write` | `const uint64_t findfv_guid_phys = ppi_phys + 0xa0;` |
| 1781 | `cpu_physical_memory_write` | `stq_le_p(&ppi[0x20], ipf_fw_region8_addr(findfv_guid_phys));` |
| 1782 | `cpu_physical_memory_write` | `stq_le_p(&ppi[0x28], ipf_fw_region8_addr(findfv_iface_phys));` |
| 1797 | `cpu_physical_memory_write` | `memcpy(&ppi[0xa0], findfv_guid, sizeof(findfv_guid));` |
| 1807 | `cpu_physical_memory_write` | `ipf_boot_findfv_stub = ipf_fw_region8_addr(findfv_stub_phys);` |
| 1808 | `cpu_physical_memory_write` | `ipf_boot_findfv_iface = ipf_fw_region8_addr(findfv_iface_phys);` |
| 2029 | `ipf_add_text_watch` | `static void ipf_add_text_watch(IPFMachineState *m, MemoryRegion *sysmem,` |
| 2043 | `memory_region_init_io` | `memory_region_add_subregion_overlap(sysmem, pa, &w->mr, 1000);` |
| 2055 | `ipf_setup_ram_watches` | `static void ipf_setup_ram_watches(IPFMachineState *m, MemoryRegion *sysmem,` |
| 2081 | `ipf_add_text_watch` | `ipf_add_text_watch(m, sysmem, cpu, ram, 0, pa, size,` |
| 2092 | `ipf_add_text_watch` | `ipf_add_text_watch(m, sysmem, cpu, ram, 0, pa, size,` |
| 2103 | `ipf_add_text_watch` | `ipf_add_text_watch(m, sysmem, cpu, ram, 0, pa, size,` |
| 2112 | `ipf_add_text_watch` | `ipf_add_text_watch(m, sysmem, cpu, ram, 0, pa, size,` |
| 3106 | `if` | `s->fw_pei_findfv_stub = ipf_boot_findfv_stub;` |
| 3107 | `if` | `s->fw_pei_findfv_iface = ipf_boot_findfv_iface;` |
| 3117 | `if` | `s->fw_pei_findfv_stub = 0;` |
| 3118 | `if` | `s->fw_pei_findfv_iface = 0;` |
| 3236 | `ipf_debugcon_log_hob` | `static void ipf_debugcon_log_hob(IPFMachineState *m, const char *tag,` |
| 3275 | `qemu_log_mask` | `"FWDBG_HOB %s line=\"%s\" ip=%016" PRIx64` |
| 3281 | `qemu_log_mask` | `" hob28=%016" PRIx64 "/%016" PRIx64 "/%d/%016" PRIx64 "/%d"` |
| 3282 | `qemu_log_mask` | `" hob35=%016" PRIx64 "/%016" PRIx64 "/%d/%016" PRIx64 "/%d"` |
| 3283 | `qemu_log_mask` | `" hob40=%016" PRIx64 "/%016" PRIx64 "/%d/%016" PRIx64 "/%d\n",` |
| 3338 | `ipf_debugcon_trace_line` | `int hob_on_assert_enabled)` |
| 3346 | `if` | `hob_on_assert_enabled &&` |
| 3349 | `if` | `ipf_dump_gfw_hob("assert");` |
| 3351 | `if` | `if (hob_on_assert_enabled && is_assert && !m->debugcon_gcd_dumped && m->cpu) {` |
| 3357 | `qemu_log_mask` | `ia64_fw_dump_hobs_and_gcd(&m->cpu->env);` |
| 3362 | `strstr` | `ia64_fw_dump_hobs_and_gcd(&m->cpu->env);` |
| 3376 | `strstr` | `strstr(line, "hob signature") \|\|` |
| 3377 | `strstr` | `strstr(line, "HOB signature")) {` |
| 3378 | `strstr` | `ipf_debugcon_log_hob(m, "memmap", line);` |
| 3384 | `if` | `ia64_fw_dump_hobs_and_gcd(&m->cpu->env);` |
| 3393 | `if` | `if (hob_on_assert_enabled) {` |
| 3394 | `if` | `ipf_dump_gfw_hob("assert");` |
| 3426 | `ipf_uart_line_hook` | `static int hob_on_assert_enabled = -1;` |
| 3431 | `if` | `if (hob_on_assert_enabled == -1) {` |
| 3432 | `if` | `hob_on_assert_enabled = getenv("QEMU_IPF_DUMP_HOB_ON_ASSERT") ? 1 : 0;` |
| 3437 | `if` | `if (hob_on_assert_enabled) {` |
| 3438 | `if` | `ipf_dump_gfw_hob("assert");` |
| 3441 | `if` | `if (hob_on_assert_enabled && is_assert && !m->debugcon_gcd_dumped && m->cpu) {` |
| 3447 | `qemu_log_mask` | `ia64_fw_dump_hobs_and_gcd(&m->cpu->env);` |
| 3459 | `if` | `ia64_fw_dump_hobs_and_gcd(&m->cpu->env);` |
| 3508 | `ipf_debugcon_write` | `static int hob_on_assert_enabled = -1;` |
| 3525 | `if` | `if (hob_on_assert_enabled == -1) {` |
| 3526 | `if` | `hob_on_assert_enabled = getenv("QEMU_IPF_DUMP_HOB_ON_ASSERT") ? 1 : 0;` |
| 3536 | `ipf_debugcon_trace_line` | `dxe_trace_enabled, hob_on_assert_enabled);` |
| 3573 | `ipf_debugcon_trace_line` | `dxe_trace_enabled, hob_on_assert_enabled);` |
| 3582 | `ipf_debugcon_trace_line` | `dxe_trace_enabled, hob_on_assert_enabled);` |
| 3677 | `ipf_init_uart` | `static void ipf_init_uart(IPFMachineState *m, MemoryRegion *sysmem)` |
| 3714 | `if` | `memory_region_add_subregion_overlap(sysmem, IPF_UART_BASE, mr, 1);` |
| 4040 | `ipf_init_legacy_io` | `static void ipf_init_legacy_io(IPFMachineState *m, MemoryRegion *sysmem)` |
| 4044 | `memory_region_init_io` | `memory_region_add_subregion(sysmem, IPF_LEGACY_IO_BASE, &m->legacy_io_mmio);` |
| 4047 | `memory_region_add_subregion` | `memory_region_add_subregion(sysmem, IPF_LEGACY_IO_BASE_FW,` |
| 4285 | `ipf_init_spad` | `MemoryRegion *sysmem)` |
| 4293 | `memory_region_add_subregion_overlap` | `memory_region_add_subregion_overlap(sysmem,` |
| 4340 | `ipf_init` | `MemoryRegion *sysmem = get_system_memory();` |
| 4372 | `if` | `* Xenipf firmware (and our GFW HOB builder) expect a legacy VGA hole at` |
| 4383 | `memory_region_init_alias` | `memory_region_add_subregion(sysmem, 0, &m->ram_low);` |
| 4388 | `memory_region_add_subregion` | `memory_region_add_subregion(sysmem, IPF_VGA_HOLE_START + IPF_VGA_HOLE_SIZE,` |
| 4392 | `memory_region_add_subregion` | `memory_region_add_subregion(sysmem, 0, machine->ram);` |
| 4401 | `memory_region_add_subregion` | `* up slightly above the top of guest RAM as reported through Xen's HOB` |
| 4415 | `memory_region_init_ram` | `memory_region_add_subregion(sysmem, ram_top, &m->ram_slack);` |
| 4427 | `DPRINTF` | `memory_region_add_subregion(sysmem, GFW_START, &m->rom);` |
| 4432 | `memory_region_init_ram` | `memory_region_add_subregion(sysmem, IPF_FW_WORKRAM_BASE, &m->fw_workram);` |
| 4443 | `memory_region_add_subregion` | `memory_region_add_subregion(sysmem,` |
| 4449 | `memory_region_add_subregion` | `ipf_init_legacy_io(m, sysmem);` |
| 4486 | `ipf_add_text_watch` | `ipf_add_text_watch(m, sysmem, cpu, &m->rom, GFW_START,` |
| 4513 | `ipf_add_text_watch` | `ipf_add_text_watch(m, sysmem, cpu, &m->rom, GFW_START,` |
| 4523 | `if` | `if (ipf_gfw_build_hob(machine->ram_size, machine->smp.cpus,` |
| 4525 | `if` | `error_report("Unable to build GFW HOB list");` |
| 4530 | `if` | `ipf_dump_gfw_hob("boot");` |
| 4548 | `if` | `ipf_init_spad(m, sysmem);` |
| 4557 | `if` | `ipf_init_uart(m, sysmem);` |
| 4575 | `if` | `ipf_boot_r28 = GFW_HOB_START;` |
| 4580 | `if` | `ipf_setup_ram_watches(m, sysmem, cpu, machine->ram, 0, false);` |
| 4649 | `ipf_add_text_watch` | `ipf_add_text_watch(m, sysmem, cpu, machine->ram, 0,` |
| 4660 | `ipf_add_text_watch` | `ipf_add_text_watch(m, sysmem, cpu, machine->ram, 0,` |
| 4670 | `fprintf` | `ipf_setup_ram_watches(m, sysmem, cpu, machine->ram, ipf_kernel_bias, true);` |
| 5243 | `if` | `ipf_boot_r28 = GFW_HOB_START;` |

## `target/ia64/helper.c`

| Line | Enclosing function | Match |
|---:|---|---|
| 34 | `(file scope)` | `static uint64_t ia64_fw_pei_cached_hob_base;` |
| 37 | `ia64_fw_validate_efi_hob_list` | `static bool ia64_fw_validate_efi_hob_list(CPUState *cs, uint64_t base,` |
| 174 | `if` | `/* Match hw/ia64/gfw.h GFW_HOB_START/GFW_HOB_SIZE. */` |
| 175 | `if` | `#define IA64_IPF_GFW_HOB_BASE         0x00000000ff200000ULL` |
| 176 | `if` | `#define IA64_IPF_GFW_HOB_SIZE         (1ULL << 20)` |
| 497 | `ia64_fw_r8_log_pei_hob` | `static void ia64_fw_r8_log_pei_hob(CPUIA64State *env, uint64_t pc,` |
| 1211 | `if` | `const char *s = getenv("QEMU_IA64_CALL_NULL_FIX");` |
| 1400 | `if` | `const char *s = getenv("QEMU_IA64_CALL_NULL_FIX_LOG_LIMIT");` |
| 1411 | `qemu_log_mask` | `"IA64: call_null_fix pc=%016" PRIx64` |
| 4332 | `if` | `static const IA64EfiGuid ia64_efi_guid_flashmap_hob = {` |
| 4575 | `ia64_fw_pei_seed_core_hob_field` | `static void ia64_fw_pei_seed_core_hob_field(CPUState *cs, uint64_t core)` |
| 4582 | `ia64_fw_pei_seed_core_hob_field` | `uint64_t hob260_raw = 0;` |
| 4583 | `ia64_fw_pei_seed_core_hob_field` | `uint64_t hob470_raw = 0;` |
| 4584 | `ia64_fw_pei_seed_core_hob_field` | `uint64_t hob478_raw = 0;` |
| 4585 | `ia64_fw_pei_seed_core_hob_field` | `uint64_t hob260_phys = 0;` |
| 4586 | `ia64_fw_pei_seed_core_hob_field` | `uint64_t hob470_phys = 0;` |
| 4587 | `ia64_fw_pei_seed_core_hob_field` | `uint64_t hob478_phys = 0;` |
| 4588 | `ia64_fw_pei_seed_core_hob_field` | `bool hob260_ok = false;` |
| 4589 | `ia64_fw_pei_seed_core_hob_field` | `bool hob470_ok = false;` |
| 4590 | `ia64_fw_pei_seed_core_hob_field` | `bool hob478_ok = false;` |
| 4591 | `ia64_fw_pei_seed_core_hob_field` | `bool hob260_valid = false;` |
| 4592 | `ia64_fw_pei_seed_core_hob_field` | `bool hob470_valid = false;` |
| 4593 | `ia64_fw_pei_seed_core_hob_field` | `bool hob478_valid = false;` |
| 4602 | `if` | `hob260_ok = ia64_fw_read_u64(cs, core + 0x260, &hob260_raw);` |
| 4603 | `if` | `hob470_ok = ia64_fw_read_u64(cs, core + 0x470, &hob470_raw);` |
| 4604 | `if` | `hob478_ok = ia64_fw_read_u64(cs, core + 0x478, &hob478_raw);` |
| 4605 | `if` | `if (!hob260_ok && !hob470_ok) {` |
| 4609 | `if` | `if (hob260_ok && hob260_raw && hob260_raw != UINT64_MAX) {` |
| 4610 | `if` | `hob260_phys = ia64_phys_mode_addr(hob260_raw);` |
| 4611 | `if` | `hob260_valid = ia64_fw_validate_efi_hob_list(cs, hob260_phys,` |
| 4614 | `if` | `if (hob470_ok && hob470_raw && hob470_raw != UINT64_MAX) {` |
| 4615 | `if` | `hob470_phys = ia64_phys_mode_addr(hob470_raw);` |
| 4616 | `if` | `hob470_valid = ia64_fw_validate_efi_hob_list(cs, hob470_phys,` |
| 4619 | `if` | `if (hob478_ok && hob478_raw && hob478_raw != UINT64_MAX) {` |
| 4620 | `if` | `hob478_phys = ia64_phys_mode_addr(hob478_raw);` |
| 4621 | `if` | `hob478_valid = ia64_fw_validate_efi_hob_list(cs, hob478_phys,` |
| 4625 | `if` | `if (hob260_valid) {` |
| 4626 | `if` | `src_raw = hob260_raw;` |
| 4627 | `if` | `src_phys = hob260_phys;` |
| 4629 | `if` | `} else if (hob470_valid) {` |
| 4630 | `if` | `src_raw = hob470_raw;` |
| 4631 | `if` | `src_phys = hob470_phys;` |
| 4633 | `if` | `} else if (hob478_valid) {` |
| 4634 | `if` | `src_raw = hob478_raw;` |
| 4635 | `if` | `src_phys = hob478_phys;` |
| 4639 | `if` | `if (!src_phys && ia64_fw_pei_cached_hob_base &&` |
| 4640 | `ia64_fw_validate_efi_hob_list` | `ia64_fw_validate_efi_hob_list(cs, ia64_fw_pei_cached_hob_base,` |
| 4642 | `ia64_fw_validate_efi_hob_list` | `src_phys = ia64_fw_pei_cached_hob_base;` |
| 4656 | `if` | `{ 0x260, hob260_ok, hob260_valid, hob260_raw },` |
| 4657 | `if` | `{ 0x470, hob470_ok, hob470_valid, hob470_raw },` |
| 4680 | `qemu_log_mask` | `"IA64: pei_core_hob_seed core=%016" PRIx64` |
| 4718 | `if` | `ia64_fw_pei_seed_core_hob_field(cs, cand);` |
| 4750 | `if` | `ia64_fw_pei_seed_core_hob_field(cs, cand);` |
| 5097 | `return` | `IA64_PEI_SVC_GET_HOB_LIST,` |
| 5098 | `return` | `IA64_PEI_SVC_CREATE_HOB,` |
| 5117 | `return` | `{ IA64_PEI_SVC_GET_HOB_LIST,      0x48, "get_hob_list" },` |
| 5118 | `return` | `{ IA64_PEI_SVC_CREATE_HOB,        0x50, "create_hob" },` |
| 5199 | `if` | `#define IA64_PEI_HOB_FLOW_STACK_MAX 64` |
| 5200 | `if` | `typedef struct IA64PeiHobFlowCall {` |
| 5209 | `if` | `uint64_t hob_ptr_addr;` |
| 5210 | `if` | `uint64_t hob_ptr_pre;` |
| 5211 | `if` | `bool hob_ptr_pre_ok;` |
| 5214 | `if` | `} IA64PeiHobFlowCall;` |
| 5216 | `if` | `static IA64PeiHobFlowCall` |
| 5217 | `if` | `ia64_fw_pei_hob_flow_stack[IA64_PEI_HOB_FLOW_STACK_MAX];` |
| 5218 | `if` | `static uint32_t ia64_fw_pei_hob_flow_sp;` |
| 5416 | `ia64_fw_pei_hob_flow_trace_enabled` | `static bool ia64_fw_pei_hob_flow_trace_enabled(void)` |
| 5420 | `if` | `const char *s = getenv("QEMU_IA64_PEI_HOB_FLOW_TRACE");` |
| 5426 | `ia64_fw_pei_hob_flow_trace_limit` | `static int ia64_fw_pei_hob_flow_trace_limit(void)` |
| 5431 | `if` | `const char *s = getenv("QEMU_IA64_PEI_HOB_FLOW_TRACE_LIMIT");` |
| 5442 | `ia64_fw_pei_hob_ptr_fix_enabled` | `static bool ia64_fw_pei_hob_ptr_fix_enabled(void)` |
| 5446 | `if` | `const char *s = getenv("QEMU_IA64_PEI_HOB_PTR_FIX");` |
| 5452 | `ia64_fw_pei_hob_ptr_fix_log_limit` | `static int ia64_fw_pei_hob_ptr_fix_log_limit(void)` |
| 5457 | `if` | `const char *s = getenv("QEMU_IA64_PEI_HOB_PTR_FIX_LOG_LIMIT");` |
| 5468 | `ia64_fw_pei_create_hob_ptr_guard_enabled` | `static bool ia64_fw_pei_create_hob_ptr_guard_enabled(void)` |
| 5472 | `if` | `const char *s = getenv("QEMU_IA64_PEI_CREATE_HOB_PTR_GUARD");` |
| 5478 | `ia64_fw_pei_hob_ptr_addr` | `static bool ia64_fw_pei_hob_ptr_addr(uint16_t svc_id,` |
| 5483 | `if` | `if (svc_id == IA64_PEI_SVC_GET_HOB_LIST) {` |
| 5485 | `if` | `} else if (svc_id == IA64_PEI_SVC_CREATE_HOB) {` |
| 5495 | `ia64_fw_pei_read_hob_ptr` | `static bool ia64_fw_pei_read_hob_ptr(CPUState *cs, uint64_t ptr_addr,` |
| 5508 | `ia64_fw_pei_hob_ptr_valid` | `static bool ia64_fw_pei_hob_ptr_valid(CPUState *cs, uint64_t raw,` |
| 5516 | `if` | `if (!ia64_fw_validate_efi_hob_list(cs, phys, NULL, NULL)) {` |
| 5526 | `ia64_fw_pei_resolve_hob_source` | `static bool ia64_fw_pei_resolve_hob_source(CPUIA64State *env, uint64_t ps_hint,` |
| 5529 | `ia64_fw_pei_resolve_hob_source` | `uint64_t *hob470_raw_out,` |
| 5530 | `ia64_fw_pei_resolve_hob_source` | `uint64_t *hob260_raw_out,` |
| 5540 | `ia64_fw_pei_resolve_hob_source` | `(void)hob470_raw_out;` |
| 5541 | `ia64_fw_pei_resolve_hob_source` | `(void)hob260_raw_out;` |
| 5550 | `ia64_fw_pei_resolve_hob_source` | `uint64_t hob470_raw = 0;` |
| 5551 | `ia64_fw_pei_resolve_hob_source` | `uint64_t hob260_raw = 0;` |
| 5563 | `if` | `(void)ia64_fw_read_u64(cs, core + 0x470, &hob470_raw);` |
| 5564 | `if` | `(void)ia64_fw_read_u64(cs, core + 0x260, &hob260_raw);` |
| 5565 | `if` | `if (ia64_fw_pei_hob_ptr_valid(cs, hob470_raw, &src_phys)) {` |
| 5566 | `if` | `src_raw = hob470_raw;` |
| 5568 | `if` | `} else if (ia64_fw_pei_hob_ptr_valid(cs, hob260_raw, &src_phys)) {` |
| 5569 | `if` | `src_raw = hob260_raw;` |
| 5574 | `if` | `if (!src_raw && ia64_fw_pei_cached_hob_base &&` |
| 5575 | `ia64_fw_validate_efi_hob_list` | `ia64_fw_validate_efi_hob_list(cs, ia64_fw_pei_cached_hob_base,` |
| 5577 | `ia64_fw_validate_efi_hob_list` | `src_raw = ia64_fw_pei_cached_hob_base;` |
| 5578 | `ia64_fw_validate_efi_hob_list` | `src_phys = ia64_fw_pei_cached_hob_base;` |
| 5588 | `if` | `if (hob470_raw_out) {` |
| 5589 | `if` | `*hob470_raw_out = hob470_raw;` |
| 5591 | `if` | `if (hob260_raw_out) {` |
| 5592 | `if` | `*hob260_raw_out = hob260_raw;` |
| 7286 | `ia64_fw_pei_hob_flow_push` | `static void ia64_fw_pei_hob_flow_push(CPUIA64State *env,` |
| 7310 | `if` | `if (ia64_fw_pei_hob_flow_sp >= IA64_PEI_HOB_FLOW_STACK_MAX) {` |
| 7311 | `memmove` | `memmove(&ia64_fw_pei_hob_flow_stack[0],` |
| 7312 | `memmove` | `&ia64_fw_pei_hob_flow_stack[1],` |
| 7313 | `memmove` | `(IA64_PEI_HOB_FLOW_STACK_MAX - 1) *` |
| 7314 | `memmove` | `sizeof(ia64_fw_pei_hob_flow_stack[0]));` |
| 7315 | `memmove` | `ia64_fw_pei_hob_flow_sp = IA64_PEI_HOB_FLOW_STACK_MAX - 1;` |
| 7318 | `memmove` | `IA64PeiHobFlowCall *ent =` |
| 7319 | `memmove` | `&ia64_fw_pei_hob_flow_stack[ia64_fw_pei_hob_flow_sp++];` |
| 7320 | `memmove` | `*ent = (IA64PeiHobFlowCall) {` |
| 7333 | `memmove` | `ent->hob_ptr_addr = 0;` |
| 7334 | `memmove` | `ent->hob_ptr_pre = 0;` |
| 7335 | `memmove` | `ent->hob_ptr_pre_ok = false;` |
| 7336 | `if` | `if (ia64_fw_pei_hob_ptr_addr(svc_id, a1, a3, &ent->hob_ptr_addr)) {` |
| 7338 | `if` | `if (ia64_fw_pei_read_hob_ptr(env_cpu(env), ent->hob_ptr_addr, &pre)) {` |
| 7339 | `if` | `ent->hob_ptr_pre = pre;` |
| 7340 | `if` | `ent->hob_ptr_pre_ok = true;` |
| 7371 | `ia64_fw_pei_producer_record_call` | `bool want_hob_flow = ia64_fw_pei_hob_flow_trace_enabled() \|\|` |
| 7372 | `ia64_fw_pei_hob_ptr_fix_enabled` | `ia64_fw_pei_hob_ptr_fix_enabled() \|\|` |
| 7373 | `ia64_fw_pei_hob_ptr_fix_enabled` | `ia64_fw_pei_create_hob_ptr_guard_enabled();` |
| 7377 | `if` | `!want_notify_flow && !want_hob_flow) {` |
| 7401 | `if` | `if (want_hob_flow &&` |
| 7402 | `if` | `(svc_id == IA64_PEI_SVC_GET_HOB_LIST \|\|` |
| 7403 | `if` | `svc_id == IA64_PEI_SVC_CREATE_HOB)) {` |
| 7404 | `ia64_fw_pei_hob_flow_push` | `ia64_fw_pei_hob_flow_push(env, svc_id, pc + 16, pc, tgt, seq,` |
| 8053 | `ia64_fw_validate_efi_hob_list` | `static bool ia64_fw_validate_efi_hob_list(CPUState *cs, uint64_t base,` |
| 8057 | `ia64_fw_validate_efi_hob_list` | `EFI_HOB_TYPE_HANDOFF = 0x0001,` |
| 8058 | `ia64_fw_validate_efi_hob_list` | `EFI_HOB_TYPE_END_OF_HOB_LIST = 0xffff,` |
| 8069 | `if` | `if (iter == 0 && type != EFI_HOB_TYPE_HANDOFF) {` |
| 8079 | `if` | `if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {` |
| 8092 | `ia64_fw_pei_log_core_hob_candidates` | `static void ia64_fw_pei_log_core_hob_candidates(CPUState *cs, uint64_t core,` |
| 8097 | `if` | `const char *s = getenv("QEMU_IA64_EFI_HOB_DUMP");` |
| 8116 | `if` | `bool valid = ia64_fw_validate_efi_hob_list(cs, phys, &end, &count);` |
| 8118 | `qemu_log_mask` | `"IA64: pei_core hob_candidate off=0x%zx raw=%016" PRIx64` |
| 8176 | `ia64_fw_find_pei_hob_list` | `static bool ia64_fw_find_pei_hob_list(CPUState *cs, uint64_t stack_phys,` |
| 8177 | `ia64_fw_find_pei_hob_list` | `uint64_t *hob_base_out, uint64_t *hob_end_out)` |
| 8179 | `ia64_fw_find_pei_hob_list` | `static uint64_t cached_hob_base;` |
| 8180 | `ia64_fw_find_pei_hob_list` | `static uint64_t cached_hob_end;` |
| 8183 | `if` | `if (cached_hob_base) {` |
| 8186 | `if` | `if (ia64_fw_validate_efi_hob_list(cs, cached_hob_base, &end, &count)) {` |
| 8187 | `if` | `cached_hob_end = end;` |
| 8188 | `if` | `if (hob_base_out) {` |
| 8189 | `if` | `*hob_base_out = cached_hob_base;` |
| 8191 | `if` | `if (hob_end_out) {` |
| 8192 | `if` | `*hob_end_out = cached_hob_end;` |
| 8196 | `if` | `cached_hob_base = 0;` |
| 8197 | `if` | `cached_hob_end = 0;` |
| 8250 | `if` | `const char *s = getenv("QEMU_IA64_EFI_HOB_DUMP");` |
| 8258 | `qemu_log_mask` | `ia64_fw_pei_log_core_hob_candidates(cs, base, ps_ptr);` |
| 8273 | `if` | `if (ia64_fw_validate_efi_hob_list(cs, phys, &end, &count)) {` |
| 8274 | `if` | `cached_hob_base = phys;` |
| 8275 | `if` | `cached_hob_end = end;` |
| 8276 | `if` | `if (hob_base_out) {` |
| 8277 | `if` | `*hob_base_out = phys;` |
| 8279 | `if` | `if (hob_end_out) {` |
| 8280 | `if` | `*hob_end_out = end;` |
| 8283 | `qemu_log_mask` | `"IA64: pei_core hob_list=%016" PRIx64` |
| 8299 | `if` | `if (ia64_fw_validate_efi_hob_list(cs, phys, &end, &count)) {` |
| 8300 | `if` | `cached_hob_base = phys;` |
| 8301 | `if` | `cached_hob_end = end;` |
| 8302 | `if` | `if (hob_base_out) {` |
| 8303 | `if` | `*hob_base_out = phys;` |
| 8305 | `if` | `if (hob_end_out) {` |
| 8306 | `if` | `*hob_end_out = end;` |
| 8309 | `qemu_log_mask` | `"IA64: hob_patch: PEI core hob_list=%016" PRIx64` |
| 8323 | `ia64_fw_clone_hob_list_ram` | `static bool ia64_fw_clone_hob_list_ram(CPUState *cs,` |
| 8352 | `if` | `uint64_t end_hob_raw = ldq_le_p(&phit[48]);` |
| 8354 | `if` | `uint64_t new_end_hob = dst_base + list_len;` |
| 8355 | `if` | `uint64_t new_free_bottom = (new_end_hob + 0x1fULL) & ~0x1fULL;` |
| 8376 | `if` | `uint64_t end_hob_tmpl = end_hob_raw ? end_hob_raw : mem_bottom_raw;` |
| 8381 | `if` | `stq_le_p(&phit[48], ia64_fw_encode_addr(end_hob_tmpl, new_end_hob));` |
| 8390 | `ia64_fw_find_hob_list_in_range` | `static bool ia64_fw_find_hob_list_in_range(CPUState *cs,` |
| 8392 | `ia64_fw_find_hob_list_in_range` | `uint64_t *hob_base_out,` |
| 8393 | `ia64_fw_find_hob_list_in_range` | `uint64_t *hob_end_out)` |
| 8417 | `if` | `if (!ia64_fw_validate_efi_hob_list(cs, cand, &end, &count)) {` |
| 8432 | `if` | `if (hob_base_out) {` |
| 8433 | `if` | `*hob_base_out = best_base;` |
| 8435 | `if` | `if (hob_end_out) {` |
| 8436 | `if` | `*hob_end_out = best_end;` |
| 8441 | `ia64_fw_dump_efi_hobs_impl` | `static bool ia64_fw_dump_efi_hobs_impl(CPUState *cs, uint64_t stack_hint,` |
| 8445 | `ia64_fw_dump_efi_hobs_impl` | `* Best-effort EFI HOB list dump to diagnose early DXE ASSERTs.` |
| 8446 | `ia64_fw_dump_efi_hobs_impl` | `* The xenipf firmware typically places the HOB list in low RAM.` |
| 8449 | `ia64_fw_dump_efi_hobs_impl` | `EFI_HOB_TYPE_HANDOFF = 0x0001,` |
| 8450 | `ia64_fw_dump_efi_hobs_impl` | `EFI_HOB_TYPE_MEMORY_ALLOCATION = 0x0002,` |
| 8451 | `ia64_fw_dump_efi_hobs_impl` | `EFI_HOB_TYPE_RESOURCE_DESCRIPTOR = 0x0003,` |
| 8452 | `ia64_fw_dump_efi_hobs_impl` | `EFI_HOB_TYPE_GUID_EXTENSION = 0x0004,` |
| 8453 | `ia64_fw_dump_efi_hobs_impl` | `EFI_HOB_TYPE_FV = 0x0005,` |
| 8454 | `ia64_fw_dump_efi_hobs_impl` | `EFI_HOB_TYPE_CPU = 0x0006,` |
| 8455 | `ia64_fw_dump_efi_hobs_impl` | `EFI_HOB_TYPE_END_OF_HOB_LIST = 0xffff,` |
| 8484 | `if` | `uint64_t hob_base = 0;` |
| 8485 | `if` | `uint64_t hob_end = 0;` |
| 8486 | `if` | `uint64_t hob_best_span = 0;` |
| 8487 | `if` | `bool hob_best_end_ok = false;` |
| 8488 | `if` | `int hob_best_count = 0;` |
| 8489 | `if` | `bool hob_from_pei = false;` |
| 8492 | `ia64_fw_find_pei_hob_list` | `ia64_fw_find_pei_hob_list(cs, stack_phys, &hob_base, &hob_end)) {` |
| 8493 | `ia64_fw_find_pei_hob_list` | `hob_best_span = hob_end - hob_base;` |
| 8494 | `ia64_fw_find_pei_hob_list` | `hob_best_end_ok = true;` |
| 8495 | `ia64_fw_find_pei_hob_list` | `hob_from_pei = true;` |
| 8496 | `ia64_fw_find_pei_hob_list` | `(void)ia64_fw_validate_efi_hob_list(cs, hob_base, NULL, &hob_best_count);` |
| 8498 | `if` | `if (!hob_from_pei) {` |
| 8521 | `if` | `if (!ia64_fw_validate_efi_hob_list(cs, addr, &end, &count)) {` |
| 8527 | `if` | `if (!hob_base \|\|` |
| 8528 | `if` | `(end_ok && !hob_best_end_ok) \|\|` |
| 8529 | `if` | `(end_ok == hob_best_end_ok && span > hob_best_span)) {` |
| 8530 | `if` | `hob_best_span = span;` |
| 8531 | `if` | `hob_best_count = count;` |
| 8532 | `if` | `hob_base = addr;` |
| 8533 | `if` | `hob_end = end;` |
| 8534 | `if` | `hob_best_end_ok = end_ok;` |
| 8539 | `if` | `if (!hob_base \|\| !hob_best_end_ok) {` |
| 8583 | `if` | `if (!ia64_fw_validate_efi_hob_list(cs, cand, &end, &count)) {` |
| 8589 | `if` | `if (!hob_base \|\|` |
| 8590 | `if` | `(end_ok && !hob_best_end_ok) \|\|` |
| 8591 | `if` | `(end_ok == hob_best_end_ok && span > hob_best_span)) {` |
| 8592 | `if` | `hob_best_span = span;` |
| 8593 | `if` | `hob_best_count = count;` |
| 8594 | `if` | `hob_base = cand;` |
| 8595 | `if` | `hob_end = end;` |
| 8596 | `if` | `hob_best_end_ok = end_ok;` |
| 8603 | `if` | `if (!hob_base) {` |
| 8605 | `qemu_log_mask` | `"IA64: efi_hob_dump: PHIT HOB not found\n");` |
| 8610 | `if` | `if (cpu_memory_rw_debug(cs, hob_base, phit, sizeof(phit), false) != 0) {` |
| 8612 | `qemu_log_mask` | `"IA64: efi_hob_dump: PHIT read failed addr=%016" PRIx64 "\n",` |
| 8613 | `qemu_log_mask` | `hob_base);` |
| 8619 | `if` | `if (mem_bottom_phys_init && mem_bottom_phys_init != hob_base) {` |
| 8622 | `if` | `if (ia64_fw_validate_efi_hob_list(cs, mem_bottom_phys_init,` |
| 8625 | `if` | `if (alt_span > hob_best_span) {` |
| 8626 | `if` | `hob_base = mem_bottom_phys_init;` |
| 8627 | `if` | `hob_end = alt_end;` |
| 8628 | `if` | `hob_best_span = alt_span;` |
| 8629 | `if` | `hob_best_count = alt_count;` |
| 8630 | `if` | `if (cpu_memory_rw_debug(cs, hob_base, phit, sizeof(phit), false) != 0) {` |
| 8632 | `qemu_log_mask` | `"IA64: efi_hob_dump: PHIT read failed addr=%016" PRIx64 "\n",` |
| 8633 | `qemu_log_mask` | `hob_base);` |
| 8646 | `qemu_log_mask` | `uint64_t end_hob = ldq_le_p(&phit[48]);` |
| 8647 | `qemu_log_mask` | `uint64_t end_hob_phys = ia64_phys_mode_addr(end_hob);` |
| 8653 | `qemu_log_mask` | `"IA64: efi_hob_dump: base=%016" PRIx64` |
| 8658 | `qemu_log_mask` | `" span=0x%" PRIx64 " hobs=%d\n",` |
| 8659 | `qemu_log_mask` | `hob_base, version, boot_mode,` |
| 8661 | `qemu_log_mask` | `end_hob_phys, hob_end, hob_best_span, hob_best_count);` |
| 8663 | `qemu_log_mask` | `"IA64: efi_hob_dump: phys_mem=[%016" PRIx64 "..%016" PRIx64 "]"` |
| 8667 | `qemu_log_mask` | `end_hob_phys);` |
| 8678 | `qemu_log_mask` | `uint64_t cur = hob_base;` |
| 8683 | `qemu_log_mask` | `"IA64: efi_hob_dump: header read failed addr=%016" PRIx64 "\n",` |
| 8691 | `qemu_log_mask` | `"IA64: efi_hob_dump: bad hob len=%u type=%u addr=%016" PRIx64 "\n",` |
| 8695 | `if` | `if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {` |
| 8697 | `qemu_log_mask` | `"IA64: efi_hob_dump: end_hob addr=%016" PRIx64 "\n",` |
| 8702 | `if` | `if (type == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR && len >= 0x30) {` |
| 8716 | `qemu_log_mask` | `"IA64: efi_hob_dump: RES type=%u attr=0x%08x tested=%d start=%016" PRIx64` |
| 8721 | `if` | `if (type == EFI_HOB_TYPE_MEMORY_ALLOCATION && len >= 0x30) {` |
| 8732 | `qemu_log_mask` | `"IA64: efi_hob_dump: ALLOC memtype=%u base=%016" PRIx64` |
| 8740 | `if` | `if (type == EFI_HOB_TYPE_FV && len >= 0x18) {` |
| 8746 | `qemu_log_mask` | `"IA64: efi_hob_dump: FV base=%016" PRIx64 " len=%016" PRIx64 "\n",` |
| 8750 | `if` | `if (type == EFI_HOB_TYPE_CPU && len >= 0x10) {` |
| 8756 | `qemu_log_mask` | `"IA64: efi_hob_dump: CPU mem_bits=%u io_bits=%u\n",` |
| 8760 | `if` | `if (type == EFI_HOB_TYPE_GUID_EXTENSION && len >= 0x18) {` |
| 8768 | `qemu_log_mask` | `"IA64: efi_hob_dump: GUIDEXT len=%u guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",` |
| 8775 | `if` | `if (ia64_fw_guid_equal(&guid, &ia64_efi_guid_flashmap_hob) &&` |
| 8785 | `qemu_log_mask` | `"IA64: efi_hob_dump: FLASHMAP area=0x%02x base=%016" PRIx64` |
| 8795 | `if` | `if (hob_end && cur >= hob_end) {` |
| 8797 | `qemu_log_mask` | `"IA64: efi_hob_dump: reached hob_end cur=%016" PRIx64 "\n",` |
| 8801 | `if` | `if (cur - hob_base > (16ULL << 20)) {` |
| 8803 | `qemu_log_mask` | `"IA64: efi_hob_dump: abort, list too long\n");` |
| 8807 | `if` | `if (hob_best_end_ok) {` |
| 8818 | `ia64_fw_dump_hob_resource_descs` | `static bool ia64_fw_dump_hob_resource_descs(CPUState *cs, uint64_t stack_hint,` |
| 8822 | `ia64_fw_dump_hob_resource_descs` | `EFI_HOB_TYPE_RESOURCE_DESCRIPTOR = 0x0003,` |
| 8823 | `ia64_fw_dump_hob_resource_descs` | `EFI_HOB_TYPE_END_OF_HOB_LIST = 0xffff,` |
| 8831 | `ia64_fw_dump_hob_resource_descs` | `uint64_t hob_base = 0;` |
| 8832 | `ia64_fw_dump_hob_resource_descs` | `uint64_t hob_end = 0;` |
| 8833 | `ia64_fw_dump_hob_resource_descs` | `uint64_t hob_best_span = 0;` |
| 8839 | `ia64_fw_find_pei_hob_list` | `ia64_fw_find_pei_hob_list(cs, stack_phys, &hob_base, &hob_end)) {` |
| 8840 | `ia64_fw_find_pei_hob_list` | `hob_best_span = hob_end - hob_base;` |
| 8842 | `if` | `if (ia64_fw_pei_cached_hob_base) {` |
| 8845 | `if` | `if (ia64_fw_validate_efi_hob_list(cs, ia64_fw_pei_cached_hob_base,` |
| 8847 | `if` | `uint64_t span = end - ia64_fw_pei_cached_hob_base;` |
| 8848 | `if` | `if (!hob_base \|\| span > hob_best_span) {` |
| 8849 | `if` | `hob_base = ia64_fw_pei_cached_hob_base;` |
| 8850 | `if` | `hob_end = end;` |
| 8851 | `if` | `hob_best_span = span;` |
| 8859 | `if` | `if (ia64_fw_find_hob_list_in_range(cs, 0, 64ULL << 20,` |
| 8862 | `if` | `if (!hob_base \|\| span > hob_best_span) {` |
| 8863 | `if` | `hob_base = cand_base;` |
| 8864 | `if` | `hob_end = cand_end;` |
| 8865 | `if` | `hob_best_span = span;` |
| 8869 | `ia64_fw_find_hob_list_in_range` | `ia64_fw_find_hob_list_in_range(cs, flash_base, flash_size,` |
| 8872 | `if` | `if (!hob_base \|\| span > hob_best_span) {` |
| 8873 | `if` | `hob_base = cand_base;` |
| 8874 | `if` | `hob_end = cand_end;` |
| 8875 | `if` | `hob_best_span = span;` |
| 8879 | `ia64_fw_find_hob_list_in_range` | `ia64_fw_find_hob_list_in_range(cs, stack_phys - (32ULL << 20),` |
| 8883 | `if` | `if (!hob_base \|\| span > hob_best_span) {` |
| 8884 | `if` | `hob_base = cand_base;` |
| 8885 | `if` | `hob_end = cand_end;` |
| 8886 | `if` | `hob_best_span = span;` |
| 8891 | `if` | `if (!hob_base) {` |
| 8893 | `qemu_log_mask` | `"IA64: efi_hob_res: %s PHIT HOB not found\n",` |
| 8899 | `if` | `if (cpu_memory_rw_debug(cs, hob_base, phit, sizeof(phit), false) != 0) {` |
| 8901 | `qemu_log_mask` | `"IA64: efi_hob_res: %s PHIT read failed addr=%016" PRIx64 "\n",` |
| 8902 | `qemu_log_mask` | `tag ? tag : "handoff", hob_base);` |
| 8910 | `qemu_log_mask` | `uint64_t end_hob = ldq_le_p(&phit[48]);` |
| 8915 | `qemu_log_mask` | `uint64_t end_hob_raw = end_hob;` |
| 8920 | `qemu_log_mask` | `uint64_t end_hob_phys = ia64_phys_mode_addr(end_hob);` |
| 8922 | `qemu_log_mask` | `"IA64: efi_hob_res: %s base=%016" PRIx64` |
| 8926 | `qemu_log_mask` | `tag ? tag : "handoff", hob_base,` |
| 8929 | `qemu_log_mask` | `end_hob_phys, hob_end);` |
| 8931 | `qemu_log_mask` | `"IA64: efi_hob_res: %s raw mem=[%016" PRIx64 "..%016" PRIx64 "]"` |
| 8937 | `qemu_log_mask` | `end_hob_raw);` |
| 8939 | `qemu_log_mask` | `uint64_t cur = hob_base;` |
| 8941 | `qemu_log_mask` | `uint64_t sysmem_min = UINT64_MAX;` |
| 8942 | `qemu_log_mask` | `uint64_t sysmem_max = 0;` |
| 8947 | `qemu_log_mask` | `"IA64: efi_hob_res: header read failed addr=%016" PRIx64 "\n",` |
| 8955 | `qemu_log_mask` | `"IA64: efi_hob_res: bad hob len=%u type=%u addr=%016" PRIx64 "\n",` |
| 8959 | `if` | `if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {` |
| 8961 | `qemu_log_mask` | `"IA64: efi_hob_res: end_hob addr=%016" PRIx64 "\n",` |
| 8965 | `if` | `if (type == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR && len >= 0x30) {` |
| 8979 | `qemu_log_mask` | `"IA64: efi_hob_res: RES type=%u attr=0x%08x tested=%d start=%016" PRIx64` |
| 8985 | `if` | `if (start < sysmem_min) {` |
| 8986 | `if` | `sysmem_min = start;` |
| 8988 | `if` | `if (end > sysmem_max) {` |
| 8989 | `if` | `sysmem_max = end;` |
| 8996 | `if` | `if (hob_end && cur >= hob_end) {` |
| 8998 | `qemu_log_mask` | `"IA64: efi_hob_res: reached hob_end cur=%016" PRIx64 "\n",` |
| 9002 | `if` | `if (cur - hob_base > (16ULL << 20)) {` |
| 9004 | `qemu_log_mask` | `"IA64: efi_hob_res: abort, list too long\n");` |
| 9011 | `qemu_log_mask` | `"IA64: efi_hob_res: %s no resource descriptors found\n",` |
| 9015 | `if` | `if (sysmem_min != UINT64_MAX) {` |
| 9017 | `qemu_log_mask` | `"IA64: efi_hob_res: sysmem tested range=[%016" PRIx64 "..%016" PRIx64 "]\n",` |
| 9018 | `qemu_log_mask` | `sysmem_min, sysmem_max);` |
| 9024 | `ia64_fw_dump_hob_reg_hint` | `static void ia64_fw_dump_hob_reg_hint(CPUIA64State *env, uint64_t raw,` |
| 9038 | `if` | `if (!ia64_fw_validate_efi_hob_list(cs, phys, &end, &count)) {` |
| 9040 | `qemu_log_mask` | `"IA64: hob_hint %s raw=%016" PRIx64` |
| 9049 | `qemu_log_mask` | `"IA64: hob_hint %s raw=%016" PRIx64` |
| 9059 | `qemu_log_mask` | `uint64_t end_hob_raw = ldq_le_p(&phit[48]);` |
| 9064 | `qemu_log_mask` | `uint64_t end_hob_phys = ia64_phys_mode_addr(end_hob_raw);` |
| 9066 | `qemu_log_mask` | `"IA64: hob_hint %s raw=%016" PRIx64 " phys=%016" PRIx64` |
| 9070 | `qemu_log_mask` | `"IA64: hob_hint %s mem=[%016" PRIx64 "..%016" PRIx64 "]"` |
| 9073 | `qemu_log_mask` | `free_bottom_phys, free_top_phys, end_hob_phys);` |
| 9075 | `qemu_log_mask` | `"IA64: hob_hint %s raw mem=[%016" PRIx64 "..%016" PRIx64 "]"` |
| 9078 | `qemu_log_mask` | `free_bottom_raw, free_top_raw, end_hob_raw);` |
| 9081 | `ia64_fw_dump_hob_candidates` | `static void ia64_fw_dump_hob_candidates(CPUState *cs, uint64_t stack_hint,` |
| 9085 | `ia64_fw_dump_hob_candidates` | `EFI_HOB_TYPE_RESOURCE_DESCRIPTOR = 0x0003,` |
| 9086 | `ia64_fw_dump_hob_candidates` | `EFI_HOB_TYPE_END_OF_HOB_LIST = 0xffff,` |
| 9150 | `if` | `if (!ia64_fw_validate_efi_hob_list(cs, cand, &end, &count)) {` |
| 9152 | `qemu_log_mask` | `"IA64: hob_cand %s base=%016" PRIx64` |
| 9166 | `if` | `uint64_t end_hob_raw = ldq_le_p(&phit[48]);` |
| 9171 | `if` | `uint64_t end_hob_phys = ia64_phys_mode_addr(end_hob_raw);` |
| 9174 | `if` | `uint64_t sysmem_min = UINT64_MAX;` |
| 9175 | `if` | `uint64_t sysmem_max = 0;` |
| 9187 | `if` | `if (type == EFI_HOB_TYPE_END_OF_HOB_LIST) {` |
| 9190 | `if` | `if (type == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR && len >= 0x30) {` |
| 9207 | `if` | `if (start_phys < sysmem_min) {` |
| 9208 | `if` | `sysmem_min = start_phys;` |
| 9210 | `if` | `if (end_phys > sysmem_max) {` |
| 9211 | `if` | `sysmem_max = end_phys;` |
| 9226 | `qemu_log_mask` | `"IA64: hob_cand %s base=%016" PRIx64` |
| 9230 | `qemu_log_mask` | `"IA64: hob_cand %s mem=[%016" PRIx64 "..%016" PRIx64 "]"` |
| 9234 | `qemu_log_mask` | `free_bottom_phys, free_top_phys, end_hob_phys);` |
| 9236 | `qemu_log_mask` | `"IA64: hob_cand %s raw mem=[%016" PRIx64 "..%016" PRIx64 "]"` |
| 9240 | `qemu_log_mask` | `free_bottom_raw, free_top_raw, end_hob_raw);` |
| 9241 | `if` | `if (sysmem_min != UINT64_MAX) {` |
| 9243 | `qemu_log_mask` | `"IA64: hob_cand %s sysmem=[%016" PRIx64 "..%016" PRIx64 "]\n",` |
| 9244 | `qemu_log_mask` | `tag ? tag : "scan", sysmem_min, sysmem_max);` |
| 9283 | `ia64_fw_dump_efi_hobs` | `static bool ia64_fw_dump_efi_hobs(CPUState *cs, uint64_t stack_hint)` |
| 9285 | `ia64_fw_dump_efi_hobs` | `return ia64_fw_dump_efi_hobs_impl(cs, stack_hint, false);` |
| 9288 | `ia64_fw_dump_efi_hobs_force` | `static bool ia64_fw_dump_efi_hobs_force(CPUState *cs, uint64_t stack_hint)` |
| 9290 | `ia64_fw_dump_efi_hobs_force` | `return ia64_fw_dump_efi_hobs_impl(cs, stack_hint, true);` |
| 9296 | `ia64_fw_dump_hobs_and_gcd` | `void ia64_fw_dump_hobs_and_gcd(CPUIA64State *env)` |
| 9304 | `if` | `(void)ia64_fw_dump_efi_hobs_force(cs, stack_phys);` |
| 10005 | `qemu_log_mask` | `static void ia64_fw_try_patch_efi_hobs(CPUIA64State *env);` |
| 10441 | `ia64_fw_handle_findfv` | `static void ia64_fw_handle_findfv(CPUIA64State *env, uint64_t pc)` |
| 10489 | `qemu_log_mask` | `"IA64: fw_findfv pc=%016" PRIx64 " fv=%u addr=%016" PRIx64 "\n",` |
| 10664 | `if` | `* the mode switch, HOB growth is written through an unrelated virtual` |
| 10728 | `if` | `ia64_fw_try_patch_efi_hobs(env);` |
| 10928 | `HELPER` | `static int dxe_hob_res_dump_enabled = -1;` |
| 10929 | `HELPER` | `static bool dxe_hob_res_dumped;` |
| 10930 | `HELPER` | `static int dxe_hob_res_attempts;` |
| 10931 | `HELPER` | `static bool dxe_hob_cand_dumped;` |
| 10949 | `HELPER` | `ia64_fw_try_patch_efi_hobs(env);` |
| 10957 | `if` | `if (env->fw_pei_findfv_stub &&` |
| 10958 | `if` | `((pc ^ env->fw_pei_findfv_stub) & ~0xFULL) == 0) {` |
| 10959 | `if` | `ia64_fw_handle_findfv(env, pc);` |
| 11085 | `if` | `if (dxe_hob_res_dump_enabled == -1) {` |
| 11086 | `if` | `const char *s = getenv("QEMU_IA64_EFI_HOB_RES_DUMP");` |
| 11087 | `if` | `dxe_hob_res_dump_enabled = (s && *s) ? 1 : 0;` |
| 11267 | `if` | `if (dxe_hob_res_dump_enabled && !dxe_hob_res_dumped &&` |
| 11268 | `if` | `dxe_hob_res_attempts < 16 &&` |
| 11270 | `if` | `bool dumped = ia64_fw_dump_hob_resource_descs(env_cpu(env), env->r[12],` |
| 11272 | `if` | `if (!dxe_hob_cand_dumped) {` |
| 11273 | `if` | `ia64_fw_dump_hob_candidates(env_cpu(env), env->r[12], "dxe_handoff");` |
| 11275 | `ia64_fw_dump_phys_mem` | `"hob_phys_4110000");` |
| 11277 | `ia64_fw_dump_phys_mem` | `"hob_phys_ffff7000");` |
| 11278 | `ia64_fw_dump_phys_mem` | `dxe_hob_cand_dumped = true;` |
| 11280 | `ia64_fw_dump_phys_mem` | `ia64_fw_dump_hob_reg_hint(env, env->r[32], "r32");` |
| 11281 | `ia64_fw_dump_phys_mem` | `ia64_fw_dump_hob_reg_hint(env, env->r[33], "r33");` |
| 11282 | `ia64_fw_dump_phys_mem` | `ia64_fw_dump_hob_reg_hint(env, env->r[34], "r34");` |
| 11283 | `ia64_fw_dump_phys_mem` | `dxe_hob_res_attempts++;` |
| 11285 | `if` | `dxe_hob_res_dumped = true;` |
| 11928 | `qemu_log_mask` | `static int hob_dump_enabled = -1;` |
| 11929 | `if` | `if (hob_dump_enabled == -1) {` |
| 11930 | `if` | `const char *s = getenv("QEMU_IA64_EFI_HOB_DUMP");` |
| 11931 | `if` | `hob_dump_enabled = (s && *s) ? 1 : 0;` |
| 11933 | `if` | `if (hob_dump_enabled) {` |
| 11934 | `if` | `(void)ia64_fw_dump_efi_hobs(env_cpu(env), env->r[12]);` |
| 12670 | `if` | `static int hob_failfast = -1;` |
| 12671 | `if` | `if (hob_failfast == -1) {` |
| 12672 | `if` | `const char *s = getenv("QEMU_IA64_DBG_PROBE_HOB_FAILFAST");` |
| 12673 | `if` | `hob_failfast = (s && *s) ? 1 : 0;` |
| 12675 | `if` | `if (hob_failfast) {` |
| 12677 | `if` | `uint64_t hob_ptr = env->r[33];` |
| 12678 | `if` | `if (!hob_ptr && env->r[12]) {` |
| 12683 | `if` | `hob_ptr = ldq_le_p(buf);` |
| 12686 | `if` | `if (hob_ptr) {` |
| 12687 | `if` | `uint64_t cur = hob_ptr;` |
| 12699 | `cpu_abort` | `"IA64: HOB failfast pc=%016" PRIx64` |
| 12700 | `cpu_abort` | `" hob=%016" PRIx64 " type=%04x len=%04x",` |
| 12707 | `if` | `if (cur - hob_ptr > (1U << 20)) {` |
| 14193 | `if` | `uint64_t findfv = ldq_le_p(&disp[48]);` |
| 14203 | `qemu_log_mask` | `" findfv=%016" PRIx64 "\n",` |
| 14205 | `qemu_log_mask` | `boot_fv_addr, findfv);` |
| 14235 | `if` | `uint64_t findfv = ldq_le_p(&disp_dyn[48]);` |
| 14247 | `qemu_log_mask` | `" findfv=%016" PRIx64 "\n",` |
| 14249 | `qemu_log_mask` | `boot_fv_addr, findfv);` |
| 14317 | `if` | `uint64_t findfv = ldq_le_p(&disp2[48]);` |
| 14328 | `qemu_log_mask` | `" findfv=%016" PRIx64 "\n",` |
| 14330 | `qemu_log_mask` | `boot_fv_addr, findfv);` |
| 15252 | `if` | `bool get_hob_seen;` |
| 15253 | `if` | `IA64PeiProducerCall get_hob;` |
| 15254 | `if` | `bool create_hob_seen;` |
| 15255 | `if` | `IA64PeiProducerCall create_hob;` |
| 15647 | `if` | `if (!out->get_hob_seen &&` |
| 15648 | `if` | `ent->svc_id == IA64_PEI_SVC_GET_HOB_LIST) {` |
| 15649 | `if` | `out->get_hob_seen = true;` |
| 15650 | `if` | `out->get_hob = *ent;` |
| 15652 | `if` | `if (!out->create_hob_seen &&` |
| 15653 | `if` | `ent->svc_id == IA64_PEI_SVC_CREATE_HOB) {` |
| 15654 | `if` | `out->create_hob_seen = true;` |
| 15655 | `if` | `out->create_hob = *ent;` |
| 16256 | `qemu_log_mask` | `" get_hob=%d create_hob=%d locate_guid_ok=%d"` |
| 16262 | `qemu_log_mask` | `summary.get_hob_seen ? 1 : 0,` |
| 16263 | `qemu_log_mask` | `summary.create_hob_seen ? 1 : 0,` |
| 17074 | `ia64_fw_pei_maybe_handle_hob_flow_ret` | `static void ia64_fw_pei_maybe_handle_hob_flow_ret(CPUIA64State *env)` |
| 17080 | `if` | `if (ia64_fw_pei_hob_flow_sp == 0) {` |
| 17084 | `if` | `IA64PeiHobFlowCall *ent =` |
| 17085 | `if` | `&ia64_fw_pei_hob_flow_stack[ia64_fw_pei_hob_flow_sp - 1];` |
| 17089 | `if` | `ia64_fw_pei_hob_flow_sp--;` |
| 17093 | `if` | `uint64_t hob_ptr_post = 0;` |
| 17094 | `if` | `bool hob_ptr_post_ok = false;` |
| 17095 | `if` | `bool hob_ptr_post_valid = false;` |
| 17096 | `if` | `if (ent->hob_ptr_addr) {` |
| 17097 | `if` | `hob_ptr_post_ok = ia64_fw_pei_read_hob_ptr(cs, ent->hob_ptr_addr,` |
| 17098 | `if` | `&hob_ptr_post);` |
| 17099 | `if` | `if (hob_ptr_post_ok) {` |
| 17100 | `if` | `hob_ptr_post_valid = ia64_fw_pei_hob_ptr_valid(cs, hob_ptr_post,` |
| 17107 | `if` | `uint64_t hob470_raw = 0;` |
| 17108 | `if` | `uint64_t hob260_raw = 0;` |
| 17112 | `if` | `bool have_src = ia64_fw_pei_resolve_hob_source(env, ent->ps_ptr,` |
| 17114 | `if` | `&hob470_raw, &hob260_raw,` |
| 17120 | `if` | `bool should_fix_get_hob = ia64_fw_pei_hob_ptr_fix_enabled() &&` |
| 17121 | `if` | `ent->svc_id == IA64_PEI_SVC_GET_HOB_LIST &&` |
| 17123 | `if` | `ent->hob_ptr_addr &&` |
| 17125 | `if` | `(!hob_ptr_post_ok \|\| !hob_ptr_post \|\|` |
| 17126 | `if` | `!hob_ptr_post_valid);` |
| 17127 | `if` | `bool should_guard_create = ia64_fw_pei_create_hob_ptr_guard_enabled() &&` |
| 17128 | `if` | `ent->svc_id == IA64_PEI_SVC_CREATE_HOB &&` |
| 17130 | `if` | `ent->hob_ptr_addr &&` |
| 17132 | `if` | `(!hob_ptr_post_ok \|\| !hob_ptr_post \|\|` |
| 17133 | `if` | `!hob_ptr_post_valid);` |
| 17136 | `if` | `uint64_t hob_ptr_post_old = hob_ptr_post;` |
| 17139 | `if` | `if (should_fix_get_hob \|\| should_guard_create) {` |
| 17140 | `if` | `uint64_t template = hob_ptr_post;` |
| 17141 | `if` | `if (!template && ent->hob_ptr_pre_ok) {` |
| 17142 | `if` | `template = ent->hob_ptr_pre;` |
| 17150 | `if` | `repaired = ia64_fw_write_bytes_any(cs, ent->hob_ptr_addr,` |
| 17153 | `if` | `hob_ptr_post = repaired_val;` |
| 17154 | `if` | `hob_ptr_post_ok = true;` |
| 17155 | `if` | `hob_ptr_post_valid = ia64_fw_pei_hob_ptr_valid(cs, hob_ptr_post,` |
| 17158 | `if` | `fix_reason = should_fix_get_hob ? "get_hob_list_null_or_invalid_out"` |
| 17159 | `if` | `: "create_hob_oor_null_or_invalid_out";` |
| 17164 | `if` | `int fix_log_limit = ia64_fw_pei_hob_ptr_fix_log_limit();` |
| 17167 | `qemu_log_mask` | `"IA64: pei_hob_ptr_fix reason=%s svc=%s"` |
| 17170 | `qemu_log_mask` | `" status_post=%016" PRIx64 " hob_ptr_addr=%016" PRIx64` |
| 17171 | `qemu_log_mask` | `" hob_pre=%016" PRIx64 " hob_post_old=%016" PRIx64` |
| 17172 | `qemu_log_mask` | `" hob_post_new=%016" PRIx64 " src=%s"` |
| 17175 | `qemu_log_mask` | `" hob470=%016" PRIx64 " hob260=%016" PRIx64` |
| 17180 | `ia64_fw_pei_service_name` | `ent->status_pre, status_post, ent->hob_ptr_addr,` |
| 17181 | `ia64_fw_pei_service_name` | `ent->hob_ptr_pre,` |
| 17182 | `ia64_fw_pei_service_name` | `hob_ptr_post_old,` |
| 17185 | `ia64_fw_pei_service_name` | `hob470_raw, hob260_raw, ent->a2);` |
| 17190 | `if` | `if (ia64_fw_pei_hob_flow_trace_enabled() &&` |
| 17193 | `qemu_loglevel_mask` | `int trace_limit = ia64_fw_pei_hob_flow_trace_limit();` |
| 17196 | `qemu_log_mask` | `"IA64: pei_hob_flow svc=%s seq=%" PRIu64` |
| 17201 | `qemu_log_mask` | `" hob470=%016" PRIx64 " hob260=%016" PRIx64` |
| 17202 | `qemu_log_mask` | `" hob_ptr_addr=%016" PRIx64` |
| 17203 | `qemu_log_mask` | `" hob_pre=%016" PRIx64 "%s"` |
| 17204 | `qemu_log_mask` | `" hob_post=%016" PRIx64 "%s"` |
| 17205 | `qemu_log_mask` | `" hob_post_valid=%d src=%s src_raw=%016" PRIx64` |

## `target/ia64/translate.c`

| Line | Enclosing function | Match |
|---:|---|---|
| 2042 | `gen_helper_fw_pei_hob_init_fix` | `gen_helper_fw_pei_hob_init_fix(tcg_env,` |
| 2478 | `gen_helper_fw_pei_ppi_dump` | `* to see whether MemoryDiscovered/DxeIpl PPIs were installed.` |
| 2567 | `tcg_constant_i64` | `* PeiCreateHob, and the resulting OUT_OF_RESOURCES check.` |
| 2603 | `tcg_constant_i64` | `* xenipf/EDK firmware: targeted trace for GetHobList store site and the` |

## `scripts/run-ia64-firmware.sh`

| Line | Enclosing function | Match |
|---:|---|---|
| 33 | `IA64_FW_XENIPF_MPBUFFER_FIX_LOG_LIMIT` | `IA64_EFI_HOB_PATCH (default: inherited/off; enable EFI HOB repair path)` |
| 34 | `IA64_FW_XENIPF_MPBUFFER_FIX_LOG_LIMIT` | `IA64_EFI_HOB_PATCH_TRACE (default: inherited/off; verbose HOB patch logs)` |
| 35 | `IA64_FW_XENIPF_MPBUFFER_FIX_LOG_LIMIT` | `IA64_PEI_SYSMEM_HOB_FIX (default: inherited/1; narrowly add a missing tested system-memory HOB for DXE)` |
| 36 | `IA64_FW_XENIPF_MPBUFFER_FIX_LOG_LIMIT` | `IA64_CALL_NULL_FIX (default: inherited/off; bisect-only guard for known null br.call target path)` |
| 37 | `IA64_CALL_NULL_FIX_LOG_LIMIT` | `IA64_CALL_NULL_FIX_LOG_LIMIT (default: helper default)` |
| 57 | `IA64_PEI_LIFECYCLE_TRACE_LIMIT` | `IA64_PEI_BOOT_MODE_RECOVERY_FIX (default: 0; rewrite recovery boot-mode to full configuration when recovery PPI is absent in DxeIpl callsite)` |
| 71 | `IA64_DXE_LOAD_TRACE_LIMIT` | `IA64_DXE_ASSERT_TRACE (default: 0; trace calls into DxeIpl assert helper target 0xffe7e620 with status/file/line payload)` |
| 99 | `IA64_PEI_279D0_SAFE_MODE_LOG_LIMIT` | `IA64_PEI_HOB_FLOW_TRACE (default: 0; trace GetHobList/CreateHob call-return contract)` |
| 100 | `IA64_PEI_HOB_FLOW_TRACE_LIMIT` | `IA64_PEI_HOB_FLOW_TRACE_LIMIT (default: 128)` |
| 101 | `IA64_PEI_HOB_FLOW_TRACE_LIMIT` | `IA64_PEI_HOB_PTR_FIX (default: 0; repair success-returned GetHobList with null/invalid out pointer)` |
| 102 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT (default: 64)` |
| 103 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `IA64_PEI_CREATE_HOB_PTR_GUARD (default: 0; guard CreateHob OOR path when out pointer is null/invalid)` |
| 204 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_EFI_HOB_PATCH:-}" ]]; then` |
| 205 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_EFI_HOB_PATCH="${IA64_EFI_HOB_PATCH}"` |
| 208 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_EFI_HOB_PATCH_TRACE:-}" ]]; then` |
| 209 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_EFI_HOB_PATCH_TRACE="${IA64_EFI_HOB_PATCH_TRACE}"` |
| 212 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_PEI_SYSMEM_HOB_FIX:-}" ]]; then` |
| 213 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_PEI_SYSMEM_HOB_FIX="${IA64_PEI_SYSMEM_HOB_FIX}"` |
| 216 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_CALL_NULL_FIX:-}" ]]; then` |
| 217 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_CALL_NULL_FIX="${IA64_CALL_NULL_FIX}"` |
| 220 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_CALL_NULL_FIX_LOG_LIMIT:-}" ]]; then` |
| 221 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_CALL_NULL_FIX_LOG_LIMIT="${IA64_CALL_NULL_FIX_LOG_LIMIT}"` |
| 471 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_PEI_HOB_FLOW_TRACE:-}" ]]; then` |
| 472 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_PEI_HOB_FLOW_TRACE="${IA64_PEI_HOB_FLOW_TRACE}"` |
| 475 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_PEI_HOB_FLOW_TRACE_LIMIT:-}" ]]; then` |
| 476 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_PEI_HOB_FLOW_TRACE_LIMIT="${IA64_PEI_HOB_FLOW_TRACE_LIMIT}"` |
| 479 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_PEI_HOB_PTR_FIX:-}" ]]; then` |
| 480 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_PEI_HOB_PTR_FIX="${IA64_PEI_HOB_PTR_FIX}"` |
| 483 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_PEI_HOB_PTR_FIX_LOG_LIMIT:-}" ]]; then` |
| 484 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_PEI_HOB_PTR_FIX_LOG_LIMIT="${IA64_PEI_HOB_PTR_FIX_LOG_LIMIT}"` |
| 487 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `if [[ -n "${IA64_PEI_CREATE_HOB_PTR_GUARD:-}" ]]; then` |
| 488 | `IA64_PEI_HOB_PTR_FIX_LOG_LIMIT` | `export QEMU_IA64_PEI_CREATE_HOB_PTR_GUARD="${IA64_PEI_CREATE_HOB_PTR_GUARD}"` |

## Introduction history

### `QEMU_IA64_PEI_SYSMEM_HOB_FIX`

```text
374fe418c9 2026-08-23 ia64: add narrow Xen DXE system-memory HOB repair [skip ci]
```

### `QEMU_IA64_PEI_HOB_FLOW_TRACE`

```text
c41cec22fe 2026-02-17 ia64: advance firmware bringup tracing and corrective paths
```

### `QEMU_IA64_EFI_HOB_PATCH`

```text
c41cec22fe 2026-02-17 ia64: advance firmware bringup tracing and corrective paths
4bd81c7596 2025-12-20 ia64: prefer PEI HOB list and fix debug dump
063caf4703 2025-12-18 ia64: add UEFI PE extractor and branch predicate debug
c31d2e223f 2025-12-18 ia64: firmware bringup: SAL SST_ + EFI systab probe hooks
```

### `QEMU_IA64_CALL_NULL_FIX`

```text
c41cec22fe 2026-02-17 ia64: advance firmware bringup tracing and corrective paths
```

### `PeiFindFile`

```text
(no matching commit)
```

