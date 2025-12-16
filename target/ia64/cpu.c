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
#include "qemu/timer.h"
#include "qemu/module.h"
#ifndef CONFIG_USER_ONLY
#include "hw/core/sysemu-cpu-ops.h"
#endif
#include "accel/tcg/cpu-ops.h"

#ifndef CONFIG_USER_ONLY
static void ia64_itm_timer_cb(void *opaque);

typedef struct IA64RSEContext {
    uint64_t bspstore;
    struct IA64RSEFrame *frames;
    uint32_t depth;
    uint32_t cap;
} IA64RSEContext;

static void ia64_rse_ctxs_free(IA64CPU *cpu)
{
    if (!cpu->rse_ctxs) {
        return;
    }

    for (uint32_t i = 0; i < cpu->rse_ctxs_len; i++) {
        g_free(cpu->rse_ctxs[i].frames);
        cpu->rse_ctxs[i].frames = NULL;
        cpu->rse_ctxs[i].depth = 0;
        cpu->rse_ctxs[i].cap = 0;
    }
    g_free(cpu->rse_ctxs);
    cpu->rse_ctxs = NULL;
    cpu->rse_ctxs_len = 0;
    cpu->rse_ctxs_cap = 0;
}
#endif
static void ia64_cpu_do_interrupt(CPUState *cs);

static void ia64_cpu_set_pc(CPUState *cs, vaddr value)
{
    IA64CPU *cpu = IA64_CPU(cs);
    cpu->env.ip = value;
}

static vaddr ia64_cpu_get_pc(CPUState *cs)
{
    IA64CPU *cpu = IA64_CPU(cs);
    return cpu->env.ip;
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

#ifndef CONFIG_USER_ONLY
    if (!cpu->itm_timer) {
        cpu->itm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      ia64_itm_timer_cb, cpu);
    }
#endif
}

static void ia64_cpu_reset_hold(Object *obj, ResetType type)
{
    IA64CPU *cpu = IA64_CPU(obj);
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(cpu);
    CPUIA64State *env = &cpu->env;
    uint64_t kernel_stext = env->kernel_stext;
    uint64_t kernel_etext = env->kernel_etext;
    uint64_t kernel_bias = env->kernel_bias;
    uint64_t percpu_va_base = env->percpu_va_base;
    uint64_t percpu_pa_base = env->percpu_pa_base;
    uint64_t percpu_size = env->percpu_size;
    uint64_t dbg_console_owner_va = env->dbg_console_owner_va;
    uint64_t dbg_console_waiter_va = env->dbg_console_waiter_va;
    uint64_t dbg_switch_mode_phys_va = env->dbg_switch_mode_phys_va;
    uint64_t dbg_switch_mode_virt_va = env->dbg_switch_mode_virt_va;
    uint64_t dbg_switch_mode_phys_size = env->dbg_switch_mode_phys_size;
    uint64_t dbg_switch_mode_virt_size = env->dbg_switch_mode_virt_size;
    uint64_t dbg_ia64_switch_to_va = env->dbg_ia64_switch_to_va;
    uint64_t dbg_ia64_switch_to_size = env->dbg_ia64_switch_to_size;
    uint64_t dbg_load_switch_stack_va = env->dbg_load_switch_stack_va;
    uint64_t dbg_load_switch_stack_size = env->dbg_load_switch_stack_size;

    if (icc->parent_phases.hold) {
        icc->parent_phases.hold(obj, type);
    }

    /*
     * Reset guest-visible architectural state to a deterministic baseline.
     *
     * Note: preserve machine-provided diagnostic ranges across reset.
     */
#ifndef CONFIG_USER_ONLY
    ia64_rse_ctxs_free(cpu);
#endif
    g_free(env->rse_frames);
    g_free(env->intr_frames);
    memset(env, 0, sizeof(*env));
    env->kernel_stext = kernel_stext;
    env->kernel_etext = kernel_etext;
    env->kernel_bias = kernel_bias;
    env->percpu_va_base = percpu_va_base;
    env->percpu_pa_base = percpu_pa_base;
    env->percpu_size = percpu_size;
    env->dbg_console_owner_va = dbg_console_owner_va;
    env->dbg_console_waiter_va = dbg_console_waiter_va;
    env->dbg_switch_mode_phys_va = dbg_switch_mode_phys_va;
    env->dbg_switch_mode_virt_va = dbg_switch_mode_virt_va;
    env->dbg_switch_mode_phys_size = dbg_switch_mode_phys_size;
    env->dbg_switch_mode_virt_size = dbg_switch_mode_virt_size;
    env->dbg_ia64_switch_to_va = dbg_ia64_switch_to_va;
    env->dbg_ia64_switch_to_size = dbg_ia64_switch_to_size;
    env->dbg_load_switch_stack_va = dbg_load_switch_stack_va;
    env->dbg_load_switch_stack_size = dbg_load_switch_stack_size;

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
    /* Architectural FP constants: f0=0.0, f1=1.0. */
    env->f[0][0] = 0;
    env->f[0][1] = 0;
    env->f[1][0] = 0x8000000000000000ULL;
    env->f[1][1] = IA64_FP_SEXP(0, IA64_FP_EXP_BIAS);

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

#ifndef CONFIG_USER_ONLY
    /* External interrupts start idle/spurious; timer vector masked by default. */
    env->cr[65] = IA64_SPURIOUS_INT_VECTOR; /* cr.ivr */
    env->cr[72] = IA64_ITV_MASK;            /* cr.itv */
    if (cpu->itm_timer) {
        timer_del(cpu->itm_timer);
    }
#endif
}

static void ia64_cpu_initfn(Object *obj)
{
#ifndef CONFIG_USER_ONLY
    IA64CPU *cpu = IA64_CPU(obj);
    cpu->itm_timer = NULL;
    cpu->rse_ctxs = NULL;
    cpu->rse_ctxs_len = 0;
    cpu->rse_ctxs_cap = 0;
#endif
}

static void ia64_cpu_finalizefn(Object *obj)
{
    IA64CPU *cpu = IA64_CPU(obj);
#ifndef CONFIG_USER_ONLY
    ia64_rse_ctxs_free(cpu);
    if (cpu->itm_timer) {
        timer_free(cpu->itm_timer);
        cpu->itm_timer = NULL;
    }
#endif
    g_free(cpu->env.rse_frames);
    cpu->env.rse_frames = NULL;
    cpu->env.rse_depth = 0;
    cpu->env.rse_capacity = 0;
    g_free(cpu->env.intr_frames);
    cpu->env.intr_frames = NULL;
    cpu->env.intr_depth = 0;
    cpu->env.intr_capacity = 0;
}

#ifndef CONFIG_USER_ONLY
static void ia64_itm_timer_cb(void *opaque)
{
    IA64CPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    CPUIA64State *env = &cpu->env;

    uint64_t itv = env->cr[72];
    if (itv & IA64_ITV_MASK) {
        return; /* masked */
    }

    uint8_t vec = itv & 0xff;
    if ((env->cr[65] & 0xff) == IA64_SPURIOUS_INT_VECTOR) {
        env->cr[65] = vec;
    }
    cpu_interrupt(cs, CPU_INTERRUPT_HARD);
}

void ia64_itm_update(CPUIA64State *env)
{
    IA64CPU *cpu = IA64_CPU(env_cpu(env));
    if (!cpu->itm_timer) {
        return;
    }

    uint64_t itv = env->cr[72];
    if ((itv & IA64_ITV_MASK) || env->cr[1] == 0) {
        timer_del(cpu->itm_timer);
        return;
    }

    int64_t off = (int64_t)env->ar[IA64_AR_ITC];
    int64_t when = (int64_t)env->cr[1] - off;
    int64_t now = (int64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (when < now) {
        when = now;
    }
    timer_mod_ns(cpu->itm_timer, (int64_t)when);
}

static IA64RSEContext *ia64_rse_ctx_lookup(IA64CPU *cpu, uint64_t bspstore)
{
    for (uint32_t i = 0; i < cpu->rse_ctxs_len; i++) {
        if (cpu->rse_ctxs[i].bspstore == bspstore) {
            return &cpu->rse_ctxs[i];
        }
    }
    return NULL;
}

static IA64RSEContext *ia64_rse_ctx_get(IA64CPU *cpu, uint64_t bspstore)
{
    IA64RSEContext *ctx = ia64_rse_ctx_lookup(cpu, bspstore);
    if (ctx) {
        return ctx;
    }

    if (cpu->rse_ctxs_len == cpu->rse_ctxs_cap) {
        uint32_t new_cap = cpu->rse_ctxs_cap ? cpu->rse_ctxs_cap * 2 : 8;
        cpu->rse_ctxs = g_realloc_n(cpu->rse_ctxs, new_cap,
                                    sizeof(*cpu->rse_ctxs));
        cpu->rse_ctxs_cap = new_cap;
    }
    ctx = &cpu->rse_ctxs[cpu->rse_ctxs_len++];
    memset(ctx, 0, sizeof(*ctx));
    ctx->bspstore = bspstore;
    return ctx;
}

void ia64_rse_switch_bspstore(CPUIA64State *env, uint64_t new_bspstore)
{
    IA64CPU *cpu = IA64_CPU(env_cpu(env));
    uint64_t old_bspstore = env->ar[IA64_AR_BSPSTORE] & ~0x7ULL;

    new_bspstore &= ~0x7ULL;
    if (old_bspstore == new_bspstore) {
        return;
    }

    /*
     * Preserve the modeled stacked window + call frames per task backing
     * store. Linux swaps bspstore when switching tasks; restore the saved
     * window state associated with the new backing store.
     */
    IA64RSEContext *old = ia64_rse_ctx_get(cpu, old_bspstore);
    g_free(old->frames);
    old->frames = env->rse_frames;
    old->depth = env->rse_depth;
    old->cap = env->rse_capacity;
    env->rse_frames = NULL;
    env->rse_depth = 0;
    env->rse_capacity = 0;

    IA64RSEContext *next = ia64_rse_ctx_get(cpu, new_bspstore);
    env->rse_frames = next->frames;
    env->rse_depth = next->depth;
    env->rse_capacity = next->cap;
    next->frames = NULL;
    next->depth = 0;
    next->cap = 0;
}

static bool ia64_cpu_has_work(CPUState *cs)
{
    CPUIA64State *env = cpu_env(cs);
    return (cpu_test_interrupt(cs, CPU_INTERRUPT_HARD) &&
            (env->psr & IA64_PSR_I));
}

static const struct SysemuCPUOps ia64_sysemu_ops = {
    .get_phys_page_debug = NULL,
    .has_work = ia64_cpu_has_work,
};

static bool ia64_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    IA64CPU *cpu = IA64_CPU(cs);
    CPUIA64State *env = &cpu->env;

    if (interrupt_request & CPU_INTERRUPT_HARD) {
        uint8_t vec = env->cr[65] & 0xff;
        if ((env->psr & IA64_PSR_I) && vec != IA64_SPURIOUS_INT_VECTOR) {
            ia64_intr_push_window(env);
            env->cr_ipsr = env->psr;
            env->cr_iip = env->ip & ~0xFULL;
            env->cr_ifs = env->cfm;
            env->cr_isr = 0;
            env->cr_iim = 0;

            cs->exception_index = IA64_EXCP_BASE + IA64_VEC_EXTERNAL_INTERRUPT;
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
            ia64_cpu_do_interrupt(cs);
            return true;
        }
    }
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
    uint64_t old_psr = env->psr;

    /* Basic handler target: always vector via cr.iva (IVT base). */
    uint64_t iva = env->cr[2]; /* cr.iva */
    env->ip = iva + vec;
    env->psr = old_psr & (IA64_PSR_IT | IA64_PSR_DT);

    /*
     * On interruption, Linux enters the IVT with BN=0 (alternate bank) and
     * later uses bsw.1 to switch back to the normal bank (BN=1) when it wants
     * to save/restore the interrupted context's r16..r31.
     */
    if (old_psr & IA64_PSR_BN) {
        ia64_switch_banks_local(env);
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
    cc->get_pc = ia64_cpu_get_pc;
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
        .instance_finalize = ia64_cpu_finalizefn,
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
