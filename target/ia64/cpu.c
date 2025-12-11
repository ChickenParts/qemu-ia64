/*
 * IA-64 CPU
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "qemu/qemu-print.h"
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

    /* Basic bootstrap defaults */
    env->ip = 0xFFFF0000ULL;
    env->psr = 0;
    env->cfm = 0;

    /* Disable VHPT until firmware/guest enables it. */
    env->cr[8] = 0; /* PTA.ve = 0 */
    /* Default region registers: VE=1, PS defaults to 28 (256MB) for region 0. */
    for (int i = 0; i < 8; i++) {
        env->rr[i] = (uint64_t)(28ULL << 56) | (1ULL << 63);
    }
    /* Prefill a large identity mapping so early faults don't spin. */
    memset(env->itlb, 0, sizeof(env->itlb));
    memset(env->dtlb, 0, sizeof(env->dtlb));
    env->itlb[0].tag = 0;
    env->itlb[0].pa = 0;
    env->itlb[0].rid = 0;
    env->itlb[0].ps = 28; /* 256MB */
    env->itlb[0].ar = 7;
    env->itlb[0].pl = 0;
    env->itlb[0].d = 1;
    env->itlb[0].a = 1;
    env->itlb[0].p = 1;
    env->itlb[0].ed = 0;
    env->itlb[0].valid = 1;
    env->dtlb[0] = env->itlb[0];
    /* Map kernel virtual region with static bias (VA-PA offset). */
    uint8_t k_ps = 28; /* 256MB page, enough for the kernel text window. */
    uint64_t bias = 0xa0000000fc000000ULL;
    uint64_t kva_base = 0xa000000100000000ULL;
    uint64_t mask = ~((1ULL << k_ps) - 1);
    uint64_t tag = kva_base & mask;
    uint64_t pa = tag - bias;
    env->rr[5] = ((uint64_t)k_ps << 56) | (1ULL << 63);
    env->itlb[1].tag = tag;
    env->itlb[1].pa = pa;
    env->itlb[1].rid = 0;
    env->itlb[1].ps = k_ps;
    env->itlb[1].ar = 7;
    env->itlb[1].pl = 0;
    env->itlb[1].d = 1;
    env->itlb[1].a = 1;
    env->itlb[1].p = 1;
    env->itlb[1].ed = 0;
    env->itlb[1].valid = 1;
    env->dtlb[1] = env->itlb[1];
    env->itlb_next = 2;
    env->dtlb_next = 2;
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

static void ia64_restore_state_to_opc(CPUState *cs,
                                      const TranslationBlock *tb,
                                      const uint64_t *data)
{
    CPUIA64State *env = cpu_env(cs);
    /* data[0] = pc, data[1] = ri (from tcg_gen_insn_start) */
    env->ip = data[0];
    env->psr &= ~PSR_RI_MASK;
    env->psr |= (data[1] << PSR_RI_SHIFT) & PSR_RI_MASK;
}

static void ia64_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUIA64State *env = cpu_env(cs);
    qemu_fprintf(f, "IP=%016" PRIx64 " PSR=%016" PRIx64 " CFM=%016" PRIx64 "\n",
                 env->ip, env->psr, env->cfm);
    qemu_fprintf(f, "PR=%016" PRIx64 "\n", env->pr);
    for (int i = 0; i < 8; i++) {
        qemu_fprintf(f, "r%-2d=%016" PRIx64 "%s", i, env->r[i],
                     (i % 4 == 3) ? "\n" : " ");
    }
    for (int i = 12; i < 20; i++) {
        qemu_fprintf(f, "r%-2d=%016" PRIx64 "%s", i, env->r[i],
                     ((i - 12) % 4 == 3) ? "\n" : " ");
    }
    qemu_fprintf(f, "b0=%016" PRIx64 " b1=%016" PRIx64 " b2=%016" PRIx64 " b3=%016" PRIx64 "\n",
                 env->b[0], env->b[1], env->b[2], env->b[3]);
}

static int ia64_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return MMU_KERNEL_IDX;
}

static void ia64_cpu_do_interrupt(CPUState *cs)
{
    IA64CPU *cpu = IA64_CPU(cs);
    CPUIA64State *env = &cpu->env;
    uint32_t vec = cs->exception_index - IA64_EXCP_BASE;

    /* Save interrupted state */
    env->cr_iip = env->ip;
    env->cr_ipsr = env->psr;
    env->cr_ifs = env->cfm;

    /* Basic handler target */
    if (env->cr_iha) {
        env->ip = env->cr_iha;
    } else {
        uint64_t iva = env->cr[2]; /* cr.iva */
        env->ip = iva + vec;
    }
    env->psr &= ~PSR_RI_MASK; /* clear RI */
}

static const TCGCPUOps ia64_tcg_ops = {
    .initialize = ia64_tcg_init,
    .translate_code = ia64_translate_code,
    .tlb_fill = ia64_cpu_tlb_fill,
    .tlb_fill_align = NULL,
    .get_tb_cpu_state = ia64_get_tb_cpu_state,
    .restore_state_to_opc = ia64_restore_state_to_opc,
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
    cc->dump_state = ia64_cpu_dump_state;
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
