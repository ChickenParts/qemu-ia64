	.text
	.align 16
	.global _start
_start:
	/* Small integer-valued FP inputs via setf.sig. */
	movl r20 = 2
	setf.sig f6 = r20
	movl r21 = 3
	setf.sig f7 = r21
	movl r22 = 5
	setf.sig f8 = r22
	movl r23 = 1
	setf.sig f9 = r23
	movl r24 = 6
	setf.sig f10 = r24

test_fadd:
	fadd.s f11 = f6, f7
	fcmp.eq p6,p7 = f11, f8
(p6)	br.cond.spnt test_fsub
	br.sptk.many fail

test_fsub:
	fsub.s f12 = f7, f6
	fcmp.eq p6,p7 = f12, f9
(p6)	br.cond.spnt test_fmpy
	br.sptk.many fail

test_fmpy:
	fmpy.s f13 = f6, f7
	fcmp.eq p6,p7 = f13, f10
(p6)	br.cond.spnt test_fneg_fabs
	br.sptk.many fail

test_fneg_fabs:
	fneg f14 = f6
	fabs f15 = f14
	fcmp.eq p6,p7 = f15, f6
(p6)	br.cond.spnt test_fcmp_lt
	br.sptk.many fail

test_fcmp_lt:
	fcmp.lt p8,p9 = f6, f7
(p8)	br.cond.spnt test_fcmp_lt_p9
	br.sptk.many fail

test_fcmp_lt_p9:
(p9)	br.sptk.many fail
	br.cond.spnt test_fcmp_le

test_fcmp_le:
	fcmp.le p10,p11 = f6, f6
(p10)	br.cond.spnt test_fcmp_le_p11
	br.sptk.many fail

test_fcmp_le_p11:
(p11)	br.sptk.many fail
	br.cond.spnt test_fcmp_unord_ordered

test_fcmp_unord_ordered:
	fcmp.unord p12,p13 = f6, f7
(p13)	br.cond.spnt test_fcmp_unord_ordered_p12
	br.sptk.many fail

test_fcmp_unord_ordered_p12:
(p12)	br.sptk.many fail
	br.cond.spnt test_fp_inf_overflow

test_fp_inf_overflow:
	/* Large but representable finite value + self add => +Inf. */
	movl r25 = 0x13ffe
	setf.exp f16 = r25
	fadd.s f17 = f16, f16
	getf.exp r26 = f17
	movl r27 = 0x1ffff
	cmp.eq p6,p7 = r26, r27
(p6)	br.cond.spnt test_fp_inf_sig
	br.sptk.many fail

test_fp_inf_sig:
	getf.sig r28 = f17
	movl r29 = 0x8000000000000000
	cmp.eq p6,p7 = r28, r29
(p6)	br.cond.spnt test_fp_inf_order
	br.sptk.many fail

test_fp_inf_order:
	fcmp.lt p14,p15 = f16, f17
(p14)	br.cond.spnt test_fp_inf_order_p15
	br.sptk.many fail

test_fp_inf_order_p15:
(p15)	br.sptk.many fail
	br.cond.spnt test_fp_nan_unord

test_fp_nan_unord:
	/* +Inf - +Inf => NaN; unordered compare must be true. */
	fsub.s f18 = f17, f17
	fcmp.unord p16,p17 = f18, f18
(p16)	br.cond.spnt test_fp_nan_unord_p17
	br.sptk.many fail

test_fp_nan_unord_p17:
(p17)	br.sptk.many fail
	br.cond.spnt test_fcvt_xf_positive

test_fcvt_xf_positive:
	/*
	 * Exact firmware regression: this assembles as fcvt.xf f7=f7,
	 * the opcode that stopped the Xen DXE path (000e000e1c0).
	 * f7 still contains integer-form +3 from startup.
	 */
	fcvt.xf f7 = f7
	getf.sig r30 = f7
	movl r31 = 0xc000000000000000
	cmp.eq p6,p7 = r30, r31
(p6)	br.cond.spnt test_fcvt_xf_positive_exp
	br.sptk.many fail

test_fcvt_xf_positive_exp:
	getf.exp r30 = f7
	movl r31 = 0x10000
	cmp.eq p6,p7 = r30, r31
(p6)	br.cond.spnt test_fcvt_xf_negative
	br.sptk.many fail

test_fcvt_xf_negative:
	movl r30 = 0xfffffffffffffffd
	setf.sig f19 = r30
	fcvt.xf f19 = f19
	getf.sig r30 = f19
	movl r31 = 0xc000000000000000
	cmp.eq p6,p7 = r30, r31
(p6)	br.cond.spnt test_fcvt_xf_negative_exp
	br.sptk.many fail

test_fcvt_xf_negative_exp:
	getf.exp r30 = f19
	movl r31 = 0x30000
	cmp.eq p6,p7 = r30, r31
(p6)	br.cond.spnt test_fcvt_xf_zero
	br.sptk.many fail

test_fcvt_xf_zero:
	movl r30 = 0
	setf.sig f20 = r30
	fcvt.xf f20 = f20
	getf.sig r30 = f20
	cmp.eq p6,p7 = r30, r0
(p6)	br.cond.spnt test_fcvt_xf_zero_exp
	br.sptk.many fail

test_fcvt_xf_zero_exp:
	getf.exp r30 = f20
	cmp.eq p6,p7 = r30, r0
(p6)	br.cond.spnt test_fcvt_xf_min
	br.sptk.many fail

test_fcvt_xf_min:
	movl r30 = 0x8000000000000000
	setf.sig f21 = r30
	fcvt.xf f21 = f21
	getf.sig r30 = f21
	movl r31 = 0x8000000000000000
	cmp.eq p6,p7 = r30, r31
(p6)	br.cond.spnt test_fcvt_xf_min_exp
	br.sptk.many fail

test_fcvt_xf_min_exp:
	getf.exp r30 = f21
	movl r31 = 0x3003e
	cmp.eq p6,p7 = r30, r31
(p6)	br.cond.spnt pass
	br.sptk.many fail

pass:
	movl r8 = 0x66736c70		/* "fslp" marker in break log */
	break 0
pass_spin:
	br.sptk.many pass_spin

fail:
	movl r8 = 0x66736c66		/* "fslf" marker in break log */
	break 0
fail_spin:
	br.sptk.many fail_spin
