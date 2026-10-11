#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One-shot exact source transformation for B4 pre-predicate legality."""
from pathlib import Path

helper_path = Path("target/ia64/helper.c")
helper = helper_path.read_text()
include_anchor = '#include "vmsw.h"\n'
if helper.count(include_anchor) != 1 or '#include "b4.h"' in helper:
    raise SystemExit("unexpected helper include anchor")
helper = helper.replace(include_anchor, include_anchor + '#include "b4.h"\n')

anchor = """    /* Feature absence has priority over CPL and all virtualization checks. */
    ia64_vmsw_raise_illegal(env, retaddr);
}

static bool ia64_try_translate(CPUIA64State *env, uint64_t va, hwaddr *pa)
"""
insertion = """    /* Feature absence has priority over CPL and all virtualization checks. */
    ia64_vmsw_raise_illegal(env, retaddr);
}

/*
 * B4 legality that must be decided before normal predicate qualification.
 * Brown table cells are architecturally Reserved and fault unconditionally;
 * br.ia is unpredicated and requires the encoded qp field to select PR0.
 * Successful br.ia transition semantics remain a separate implementation.
 */
void HELPER(b4_validate)(CPUIA64State *env, uint64_t pc, uint64_t insn)
{
    uintptr_t retaddr = GETPC();
    unsigned int major = extract64(insn, 37, 4);
    unsigned int x6 = extract64(insn, 27, 6);
    unsigned int btype = extract64(insn, 6, 3);
    unsigned int qp = extract64(insn, 0, 6);
    IA64B4Class kind = ia64_b4_classify(major, x6, btype);

    (void)pc;
    if (ia64_b4_faults_before_predication(kind, qp)) {
        ia64_exception(env_cpu(env), env, false, false,
                       IA64_VEC_ILLEGAL_OP, 0, 0, 0,
                       false, false, retaddr);
        g_assert_not_reached();
    }
}

static bool ia64_try_translate(CPUIA64State *env, uint64_t va, hwaddr *pa)
"""
if helper.count(anchor) != 1 or "HELPER(b4_validate)" in helper:
    raise SystemExit("unexpected helper insertion anchor")
helper_path.write_text(helper.replace(anchor, insertion))

helper_h_path = Path("target/ia64/helper.h")
helper_h = helper_h_path.read_text()
decl = "DEF_HELPER_3(vmsw, void, env, i64, i64)\n"
if helper_h.count(decl) != 1 or "DEF_HELPER_3(b4_validate" in helper_h:
    raise SystemExit("unexpected helper declaration anchor")
helper_h_path.write_text(
    helper_h.replace(decl, decl + "DEF_HELPER_3(b4_validate, void, env, i64, i64)\n")
)

translate_path = Path("target/ia64/translate.c")
translate = translate_path.read_text()
qp_anchor = "    TCGLabel *skip_label = gen_qp_skip(qp);\n"
route = """    /*
     * B4 reserved btypes fault unconditionally, and br.ia requires qp=0.
     * Validate before gen_qp_skip(); legal br.cond/br.ret remain predicated.
     */
    if (major == 0 && (x6 == 0x20 || x6 == 0x21)) {
        gen_helper_b4_validate(tcg_env,
                               tcg_constant_i64(ctx->base.pc_next),
                               tcg_constant_i64(insn));
    }

"""
if translate.count(qp_anchor) != 1 or "gen_helper_b4_validate" in translate:
    raise SystemExit("unexpected B-unit predicate anchor")
translate = translate.replace(qp_anchor, route + qp_anchor)
old_comment = "        /* B4: br.cond/br.ia b2 (x6=0x20) and br.ret b2 (x6=0x21). */"
new_comment = """        /*
         * Assigned B4 forms were validated before predicate qualification:
         * br.cond/br.ia for x6=0x20 and br.ret for x6=0x21.
         * Successful br.ia transition semantics remain explicitly deferred.
         */"""
if translate.count(old_comment) != 1:
    raise SystemExit("unexpected B4 decoder comment")
translate_path.write_text(translate.replace(old_comment, new_comment))
