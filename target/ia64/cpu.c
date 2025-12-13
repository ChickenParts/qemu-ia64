/*
 * IA-64 CPU
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "qemu/bswap.h"
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
    uint64_t kernel_stext = env->kernel_stext;
    uint64_t kernel_etext = env->kernel_etext;
    uint64_t percpu_va_base = env->percpu_va_base;
    uint64_t percpu_pa_base = env->percpu_pa_base;
    uint64_t percpu_size = env->percpu_size;

    if (icc->parent_phases.hold) {
        icc->parent_phases.hold(obj, type);
    }

    /*
     * Reset guest-visible architectural state to a deterministic baseline.
     *
     * Note: preserve machine-provided diagnostic ranges across reset.
     */
    g_free(env->rse_frames);
    g_free(env->intr_frames);
    memset(env, 0, sizeof(*env));
    env->kernel_stext = kernel_stext;
    env->kernel_etext = kernel_etext;
    env->percpu_va_base = percpu_va_base;
    env->percpu_pa_base = percpu_pa_base;
    env->percpu_size = percpu_size;

    /* Basic bootstrap defaults */
    env->ip = 0xFFFF0000ULL;
    /*
     * Linux IA-64 expects to run with PSR.BN=1 in normal (non-interrupt)
     * context; the alternate bank (BN=0) is used on interruption entry.
     * Keeping BN=1 from reset also preserves the bootloader-provided r28
     * across the early head.S rfi used to switch into virtual mode.
     */
    env->psr = IA64_PSR_BN;
    env->cfm = 0;
    env->pr = 1; /* p0 is always true */

    /* CPUID registers (Linux reads indices 0..4 in cpu_init/identify_cpu). */
    {
        static const uint8_t vendor[16] = "GenuineIntel";
        env->cpuid[0] = ldq_le_p(&vendor[0]);
        env->cpuid[1] = ldq_le_p(&vendor[8]);
        env->cpuid[2] = 0; /* processor serial number */
        env->cpuid[3] = (0ULL) |              /* number */
                        (0ULL << 8) |         /* revision */
                        (0ULL << 16) |        /* model */
                        (0x7ULL << 24) |      /* family: Merced */
                        (0x8ULL << 32);       /* archrev */
        env->cpuid[4] = 0; /* features */
    }

    env->last_b0_write_pc = 0;
    env->last_b0_write_insn = 0;
    env->last_b0_write_val = 0;
    env->last_b0_write_kind = 0;
    env->prev_b0_write_pc = 0;
    env->prev_b0_write_insn = 0;
    env->prev_b0_write_val = 0;
    env->prev_b0_write_kind = 0;
    memset(env->b0_trace_pc, 0, sizeof(env->b0_trace_pc));
    memset(env->b0_trace_insn, 0, sizeof(env->b0_trace_insn));
    memset(env->b0_trace_val, 0, sizeof(env->b0_trace_val));
    memset(env->b0_trace_kind, 0, sizeof(env->b0_trace_kind));
    env->b0_trace_idx = 0;

    env->last_branch_from = 0;
    env->last_branch_to = 0;
    env->last_branch_insn = 0;
    env->last_branch_kind = 0;

    /* Disable VHPT until firmware/guest enables it. */
    env->cr[8] = 0; /* PTA.ve = 0 */
    /* Default region registers: VE=1, PS defaults to 28 (256MB), RID=0. */
    for (int i = 0; i < 8; i++) {
        env->rr[i] = (uint64_t)(28ULL << 2) | 1ULL;
    }
    /* TLBs start empty; mappings are seeded in machine reset. */
    memset(env->itlb, 0, sizeof(env->itlb));
    memset(env->dtlb, 0, sizeof(env->dtlb));
    env->itlb_next = 0;
    env->dtlb_next = 0;
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

    /*
     * TB translation depends on more than the bundle slot (RI):
     * - IT/DT select instruction/data translation vs physical mode
     * - CPL selects the MMU index for data accesses
     *
     * Include these bits so TBs are invalidated when the guest toggles them.
     */
    uint64_t flags = (env->psr & PSR_RI_MASK) >> PSR_RI_SHIFT;
    flags |= (env->psr & IA64_PSR_DT) ? (1ULL << 2) : 0;
    flags |= (env->psr & IA64_PSR_IT) ? (1ULL << 3) : 0;
    flags |= ((uint64_t)IA64_PSR_CPL(env->psr) & 3ULL) << 4;

    return (TCGTBCPUState){
        .pc = env->ip,
        .flags = flags,
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
    qemu_fprintf(f, "LAST_BR from=%016" PRIx64 " to=%016" PRIx64
                 " kind=%" PRIu64 " insn=%011" PRIx64 "\n",
                 env->last_branch_from, env->last_branch_to,
                 env->last_branch_kind, env->last_branch_insn);
    qemu_fprintf(f, "LAST_B0_WRITE pc=%016" PRIx64 " val=%016" PRIx64
                 " kind=%" PRIu64 " insn=%011" PRIx64 "\n",
                 env->last_b0_write_pc, env->last_b0_write_val,
                 env->last_b0_write_kind, env->last_b0_write_insn);
    qemu_fprintf(f, "PREV_B0_WRITE pc=%016" PRIx64 " val=%016" PRIx64
                 " kind=%" PRIu64 " insn=%011" PRIx64 "\n",
                 env->prev_b0_write_pc, env->prev_b0_write_val,
                 env->prev_b0_write_kind, env->prev_b0_write_insn);
    qemu_fprintf(f, "B0_TRACE idx=%u\n", env->b0_trace_idx);
    for (int i = 0; i < 16; i++) {
        qemu_fprintf(f, "b0_trace[%02d] pc=%016" PRIx64 " val=%016" PRIx64
                     " kind=%" PRIu64 " insn=%011" PRIx64 "\n",
                     i, env->b0_trace_pc[i], env->b0_trace_val[i],
                     env->b0_trace_kind[i], env->b0_trace_insn[i]);
    }
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
    qemu_fprintf(f, "r32=%016" PRIx64 " r36=%016" PRIx64 " b6=%016" PRIx64 "\n",
                 env->r[32], env->r[36], env->b[6]);
}

static int ia64_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    CPUIA64State *env = cpu_env(cs);
    uint8_t cpl = IA64_PSR_CPL(env->psr);
    if (ifetch) {
        if (!(env->psr & IA64_PSR_IT)) {
            return MMU_PHYS_IDX;
        }
    } else {
        if (!(env->psr & IA64_PSR_DT)) {
            return MMU_PHYS_IDX;
        }
    }
    return (cpl == 3) ? MMU_USER_IDX : MMU_KERNEL_IDX;
}

static void ia64_switch_banks_local(CPUIA64State *env)
{
    for (int i = 0; i < 16; i++) {
        uint64_t tmp = env->banked_r[i];
        env->banked_r[i] = env->r[16 + i];
        env->r[16 + i] = tmp;
    }
}

static void ia64_cpu_do_interrupt(CPUState *cs)
{
    IA64CPU *cpu = IA64_CPU(cs);
    CPUIA64State *env = &cpu->env;
    uint32_t vec = cs->exception_index - IA64_EXCP_BASE;

    /* Basic handler target: always vector via cr.iva (IVT base). */
    uint64_t iva = env->cr[2]; /* cr.iva */
    env->ip = iva + vec;
    env->psr &= ~PSR_RI_MASK; /* clear RI */

    /*
     * On interruption, Linux enters the IVT with BN=0 (alternate bank) and
     * later uses bsw.1 to switch back to the normal bank (BN=1) when it wants
     * to save/restore the interrupted context's r16..r31.
     */
    if (env->psr & IA64_PSR_BN) {
        ia64_switch_banks_local(env);
        env->psr &= ~IA64_PSR_BN;
    }
}

static vaddr ia64_pointer_wrap(CPUState *cs, int idx, vaddr res, vaddr base);

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
    .pointer_wrap = ia64_pointer_wrap,
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

static vaddr ia64_pointer_wrap(CPUState *cs, int idx, vaddr res, vaddr base)
{
    (void)cs;
    (void)idx;
    (void)base;
    /* No address truncation/wrapping for IA-64. */
    return res;
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
