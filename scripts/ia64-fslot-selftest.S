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
	br.cond.spnt pass

pass:
	movl r8 = 0x66736c70		/* "fslp" marker in break log */
	break 0
	br.sptk.many pass

fail:
	movl r8 = 0x66736c66		/* "fslf" marker in break log */
	break 0
	br.sptk.many fail
