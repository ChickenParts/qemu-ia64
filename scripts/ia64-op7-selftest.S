	.text
	.align 16
	.global _start
_start:
	/* Shared vectors for op=7 packed multimedia tests. */
	movl r14 = 0xff007fff80001234
	movl r15 = 0x0100fffe00030002

	/* pmpy2.r */
	pmpy2.r r16 = r14, r15
	movl r17 = 0xffff0000fffe8000
	cmp.eq p6,p0 = r16, r17
(p6)	br.cond.spnt test_pmpy2_l
	br.sptk.many fail

test_pmpy2_l:
	/* pmpy2.l */
	pmpy2.l r16 = r14, r15
	movl r17 = 0xffff000200002468
	cmp.eq p6,p0 = r16, r17
(p6)	br.cond.spnt test_pmpyshr2
	br.sptk.many fail

test_pmpyshr2:
	/* pmpyshr2 count=7 */
	pmpyshr2 r16 = r14, r15, 7
	movl r17 = 0xfe00fe00fd000048
	cmp.eq p6,p0 = r16, r17
(p6)	br.cond.spnt test_pmpyshr2_u
	br.sptk.many fail

test_pmpyshr2_u:
	/* pmpyshr2.u count=16 */
	pmpyshr2.u r16 = r14, r15, 16
	movl r17 = 0x00ff7ffe00010000
	cmp.eq p6,p0 = r16, r17
(p6)	br.cond.spnt test_pmin1_u
	br.sptk.many fail

test_pmin1_u:
	pmin1.u r16 = r14, r15
	movl r17 = 0x01007ffe00000002
	cmp.eq p6,p0 = r16, r17
(p6)	br.cond.spnt test_pmax1_u
	br.sptk.many fail

test_pmax1_u:
	pmax1.u r16 = r14, r15
	movl r17 = 0xff00ffff80031234
	cmp.eq p6,p0 = r16, r17
(p6)	br.cond.spnt test_pmin2
	br.sptk.many fail

test_pmin2:
	pmin2 r16 = r14, r15
	movl r17 = 0xff00fffe80000002
	cmp.eq p6,p0 = r16, r17
(p6)	br.cond.spnt test_pmax2
	br.sptk.many fail

test_pmax2:
	pmax2 r16 = r14, r15
	movl r17 = 0x01007fff00031234
	cmp.eq p6,p0 = r16, r17
(p6)	br.cond.spnt test_psad1
	br.sptk.many fail

test_psad1:
	psad1 r16 = r14, r15
	movl r17 = 0x0000000000000246
	cmp.eq p6,p0 = r16, r17
(p6)	br.cond.spnt pass
	br.sptk.many fail

pass:
	movl r8 = 0x70617373		/* "pass" marker in break log */
	break 0
	br.sptk.many pass

fail:
	movl r8 = 0x6661696c		/* "fail" marker in break log */
	break 0
	br.sptk.many fail
