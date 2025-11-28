/*
 * IA-64 CPU
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "qemu/module.h"
#ifndef CONFIG_USER_ONLY
#include "hw/core/sysemu-cpu-ops.h"
#endif
#include "accel/tcg/cpu-ops.h"

static void ia64_cpu_set_pc(CPUState *cs, vaddr value)
{
    IA64CPU *cpu = IA64_CPU(cs);
    cpu->env.ip = value;
}

static void ia64_cpu_realizefn(DeviceState *dev, Error **errp)
{
    IA64CPU *cpu = IA64_CPU(dev);
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(CPU(cpu), &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    qemu_init_vcpu(CPU(cpu));

    icc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
}

static void ia64_cpu_reset_hold(Object *obj, ResetType type)
{
    IA64CPU *cpu = IA64_CPU(obj);
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(cpu);
    CPUIA64State *env = &cpu->env;

    if (icc->parent_phases.hold) {
        icc->parent_phases.hold(obj, type);
    }

    env->ip = 0xFFFF0000ULL;
}

static void ia64_cpu_initfn(Object *obj)
{
}

#ifndef CONFIG_USER_ONLY
static bool ia64_cpu_has_work(CPUState *cs)
{
    return false;
}

static const struct SysemuCPUOps ia64_sysemu_ops = {
    .get_phys_page_debug = NULL,
    .has_work = ia64_cpu_has_work,
};

static bool ia64_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    return false;
}
#endif

static TCGTBCPUState ia64_get_tb_cpu_state(CPUState *cs)
{
    IA64CPU *cpu = IA64_CPU(cs);
    CPUIA64State *env = &cpu->env;

    return (TCGTBCPUState){
        .pc = env->ip,
        .flags = (env->psr & PSR_RI_MASK) >> PSR_RI_SHIFT,
        .cs_base = 0,
    };
}

static int ia64_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return MMU_KERNEL_IDX;
}

static void ia64_cpu_do_interrupt(CPUState *cs)
{
    // TODO: Implement interrupt handling
}

static const TCGCPUOps ia64_tcg_ops = {
    .initialize = ia64_tcg_init,
    .translate_code = ia64_translate_code,
    .tlb_fill = ia64_cpu_tlb_fill,
    .get_tb_cpu_state = ia64_get_tb_cpu_state,
    .mmu_index = ia64_cpu_mmu_index,
#ifndef CONFIG_USER_ONLY
    .cpu_exec_halt = ia64_cpu_has_work,
    .cpu_exec_interrupt = ia64_cpu_exec_interrupt,
    .cpu_exec_reset = cpu_reset,
    .pointer_wrap = cpu_pointer_wrap_notreached,
    .do_interrupt = ia64_cpu_do_interrupt,
#endif
};

static void ia64_cpu_class_init(ObjectClass *oc, const void *data)
{
    IA64CPUClass *icc = IA64_CPU_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, ia64_cpu_realizefn,
                                    &icc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, ia64_cpu_reset_hold, NULL,
                                       &icc->parent_phases);

    cc->class_by_name = object_class_by_name;
    cc->dump_state = NULL; // TODO
    cc->set_pc = ia64_cpu_set_pc;
    cc->gdb_read_register = NULL; // TODO
    cc->gdb_write_register = NULL; // TODO
    cc->gdb_num_core_regs = 0;
#ifndef CONFIG_USER_ONLY
    cc->sysemu_ops = &ia64_sysemu_ops;
#endif
    cc->tcg_ops = &ia64_tcg_ops;
}



static const TypeInfo ia64_cpu_type_infos[] = {
    {
        .name = TYPE_IA64_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(IA64CPU),
        .instance_init = ia64_cpu_initfn,
        .abstract = true,
        .class_size = sizeof(IA64CPUClass),
        .class_init = ia64_cpu_class_init,
    },
    {
        .name = IA64_CPU_TYPE_NAME("itanium"),
        .parent = TYPE_IA64_CPU,
    },
};

DEFINE_TYPES(ia64_cpu_type_infos)

void ia64_cpu_list(void)
{
    // TODO
}
