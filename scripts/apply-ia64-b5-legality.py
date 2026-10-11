#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One-shot exact source transformation for B5 pre-predicate legality."""
from pathlib import Path

helper_path = Path("target/ia64/helper.c")
helper = helper_path.read_text()
include_anchor = '#include "b4.h"\n'
if helper.count(include_anchor) != 1 or '#include "b5.h"' in helper:
    raise SystemExit("unexpected helper include anchor")
helper = helper.replace(include_anchor, include_anchor + '#include "b5.h"\n')
anchor = "static bool ia64_try_translate(CPUIA64State *env, uint64_t va, hwaddr *pa)\n{"
insertion = """/*
 * B5 Table 4-53 assigns only odd indirect-call whether hints.  Even wh
 * values are Reserved and must fault before ordinary predicate skipping.
 */
void HELPER(b5_validate)(CPUIA64State *env, uint64_t pc, uint64_t insn)
{
    uintptr_t retaddr = GETPC();
    unsigned int major = extract64(insn, 37, 4);
    unsigned int wh = extract64(insn, 32, 3);

    (void)pc;
    if (ia64_b5_is_reserved(major, wh)) {
        ia64_exception(env_cpu(env), env, false, false,
                       IA64_VEC_ILLEGAL_OP, 0, 0, 0,
                       false, false, retaddr);
        g_assert_not_reached();
    }
}

static bool ia64_try_translate(CPUIA64State *env, uint64_t va, hwaddr *pa)
{"""
if helper.count(anchor) != 1 or "HELPER(b5_validate)" in helper:
    raise SystemExit("unexpected helper insertion anchor")
helper_path.write_text(helper.replace(anchor, insertion))

helper_h_path = Path("target/ia64/helper.h")
helper_h = helper_h_path.read_text()
decl = "DEF_HELPER_3(b4_validate, void, env, i64, i64)\n"
if helper_h.count(decl) != 1 or "DEF_HELPER_3(b5_validate" in helper_h:
    raise SystemExit("unexpected helper declaration anchor")
helper_h_path.write_text(
    helper_h.replace(decl, decl + "DEF_HELPER_3(b5_validate, void, env, i64, i64)\n")
)

translate_path = Path("target/ia64/translate.c")
translate = translate_path.read_text()
start = translate.index("static void decode_b_unit(")
end = translate.index("static void decode_insn(", start)
bunit = translate[start:end]
qp_anchor = "    TCGLabel *skip_label = gen_qp_skip(qp);\n"
route = """    /* B5 even wh cells are Reserved and fault regardless of qp. */
    if (major == 1) {
        gen_helper_b5_validate(tcg_env,
                               tcg_constant_i64(ctx->base.pc_next),
                               tcg_constant_i64(insn));
    }

"""
if bunit.count(qp_anchor) != 1 or "gen_helper_b5_validate" in bunit:
    raise SystemExit("unexpected B-unit predicate anchor")
bunit = bunit.replace(qp_anchor, route + qp_anchor)
old = "    } else if (major == 0x1) {\n        /* br.call b1 = b2 */"
new = """    } else if (major == 0x1) {
        /* B5 assigned odd-wh br.call b1 = b2; legality checked above. */"""
if bunit.count(old) != 1:
    raise SystemExit("unexpected B5 execution anchor")
bunit = bunit.replace(old, new)
translate_path.write_text(translate[:start] + bunit + translate[end:])
