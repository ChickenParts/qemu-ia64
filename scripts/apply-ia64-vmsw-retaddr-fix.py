#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One-shot fail-closed source transformer for the vmsw GETPC fix."""
from pathlib import Path

helper_path = Path("target/ia64/helper.c")
helper = helper_path.read_text()
old = """static void ia64_vmsw_raise_illegal(CPUIA64State *env)
{
    /*
     * Illegal Operation is not an instruction-access fault.  Preserve the
     * same no-X/R/W ISR construction used by epc's fixed-field path.
     */
    ia64_exception(env_cpu(env), env, false, false,
                   IA64_VEC_ILLEGAL_OP, 0, 0, 0,
                   false, false, GETPC());
    g_assert_not_reached();
}

/* B8 vmsw.0/vmsw.1 on the current CPU model, which advertises NoVM. */
void HELPER(vmsw)(CPUIA64State *env, uint64_t pc, uint64_t insn)
{
    (void)pc;
"""
new = """static G_NORETURN void ia64_vmsw_raise_illegal(CPUIA64State *env,
                                                   uintptr_t retaddr)
{
    /*
     * Illegal Operation is not an instruction-access fault.  Preserve the
     * same no-X/R/W ISR construction used by epc's fixed-field path.
     *
     * GETPC() must be captured in HELPER(vmsw), not in this nested C helper:
     * only the translated-helper return address lets cpu_restore_state()
     * recover the faulting B-slot and construct ISR.ei/IPSR.ri correctly.
     */
    ia64_exception(env_cpu(env), env, false, false,
                   IA64_VEC_ILLEGAL_OP, 0, 0, 0,
                   false, false, retaddr);
    g_assert_not_reached();
}

/* B8 vmsw.0/vmsw.1 on the current CPU model, which advertises NoVM. */
void HELPER(vmsw)(CPUIA64State *env, uint64_t pc, uint64_t insn)
{
    uintptr_t retaddr = GETPC();

    (void)pc;
"""
if helper.count(old) != 1:
    raise SystemExit("unexpected vmsw helper source; refusing edit")
helper = helper.replace(old, new)
if helper.count("ia64_vmsw_raise_illegal(env);") != 2:
    raise SystemExit("unexpected vmsw fault call count; refusing edit")
helper_path.write_text(
    helper.replace("ia64_vmsw_raise_illegal(env);",
                   "ia64_vmsw_raise_illegal(env, retaddr);")
)

test_path = Path("tests/ia64/nonf/test_vmsw_source.py")
test = test_path.read_text()
old = """        absent = body.rindex(\"ia64_vmsw_raise_illegal(env)\")
        self.assertLess(validate, absent)
        self.assertNotIn(\"IA64_PSR_VM\", body)
        self.assertNotIn(\"env->psr =\", body)
        fault_begin = self.helper.index(
            \"static void ia64_vmsw_raise_illegal(\")
        fault_end = self.helper.index(\"void HELPER(vmsw)(\", fault_begin)
        fault = self.helper[fault_begin:fault_end]
        self.assertIn(\"IA64_VEC_ILLEGAL_OP\", fault)
        self.assertNotIn(\"ia64_fault(\", fault)
"""
new = """        absent = body.rindex(\"ia64_vmsw_raise_illegal(env, retaddr)\")
        self.assertLess(validate, absent)
        self.assertIn(\"uintptr_t retaddr = GETPC();\", body)
        self.assertEqual(2, body.count(
            \"ia64_vmsw_raise_illegal(env, retaddr)\"))
        self.assertNotIn(\"IA64_PSR_VM\", body)
        self.assertNotIn(\"env->psr =\", body)
        fault_begin = self.helper.index(
            \"static G_NORETURN void ia64_vmsw_raise_illegal(\")
        fault_end = self.helper.index(\"void HELPER(vmsw)(\", fault_begin)
        fault = self.helper[fault_begin:fault_end]
        self.assertIn(\"IA64_VEC_ILLEGAL_OP\", fault)
        self.assertIn(\"false, false, retaddr);\", fault)
        self.assertNotIn(\"GETPC()\", fault)
        self.assertNotIn(\"ia64_fault(\", fault)
"""
if test.count(old) != 1:
    raise SystemExit("unexpected vmsw source test; refusing edit")
test_path.write_text(test.replace(old, new))
