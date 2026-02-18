	.text
	.align 16
	.global _start
_start:
	/*
	 * Create a NaT source register via ld8.fill + ar.unat.
	 * r12 is 16-byte aligned in this harness, so bit0 of ar.unat maps to
	 * [r12] for ld8.fill's NaT extraction.
	 */
	movl r20 = 1
	mov ar.unat = r20
	ld8.fill r14 = [r12]
	tnat.z p6,p7 = r14
(p7)	br.cond.spnt test_add_nat
	br.sptk.many fail

test_add_nat:
	movl r15 = 0x1122334455667788
	add r16 = r14, r15
	tnat.z p6,p7 = r16
(p7)	br.cond.spnt test_and_nat
	br.sptk.many fail

test_and_nat:
	and r16 = r14, r15
	tnat.z p6,p7 = r16
(p7)	br.cond.spnt test_shladd_nat
	br.sptk.many fail

test_shladd_nat:
	shladd r16 = r14, 1, r15
	tnat.z p6,p7 = r16
(p7)	br.cond.spnt test_adds_nat
	br.sptk.many fail

test_adds_nat:
	adds r16 = 1, r14
	tnat.z p6,p7 = r16
(p7)	br.cond.spnt test_pavg2_nat
	br.sptk.many fail

test_pavg2_nat:
	pavg2 r16 = r14, r15
	tnat.z p6,p7 = r16
(p7)	br.cond.spnt test_clean_non_nat
	br.sptk.many fail

test_clean_non_nat:
	mov r17 = r15
	add r18 = r17, r0
	tnat.z p6,p7 = r18
(p6)	br.cond.spnt test_getf_sig_clears_nat
	br.sptk.many fail

test_getf_sig_clears_nat:
	mov r19 = r14
	tnat.z p6,p7 = r19
(p7)	br.cond.spnt 1f
	br.sptk.many fail
1:
	getf.sig r19 = f0
	tnat.z p6,p7 = r19
(p6)	br.cond.spnt test_getf_exp_clears_nat
	br.sptk.many fail

test_getf_exp_clears_nat:
	mov r19 = r14
	tnat.z p6,p7 = r19
(p7)	br.cond.spnt 1f
	br.sptk.many fail
1:
	getf.exp r19 = f0
	tnat.z p6,p7 = r19
(p6)	br.cond.spnt pass
	br.sptk.many fail

pass:
	movl r8 = 0x6e617470		/* "natp" marker in break log */
	break 0
	br.sptk.many pass

fail:
	movl r8 = 0x6e617466		/* "natf" marker in break log */
	break 0
	br.sptk.many fail
