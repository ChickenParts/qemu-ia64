	.text
	.align 16
	.global _start
_start:
	/* Shared test location in RAM. */
	movl r19 = 0x3000
	add r20 = r12, r19
	movl r22 = -8
	and r20 = r20, r22
	movl r21 = 0x1122334455667788
	st8 [r20] = r21

	/* Integer ALAT hit path: chk.a.clr should not branch. */
	ld8.a r14 = [r20]
	chk.a.clr r14, fail

	/* Alias store invalidates integer ALAT entry: chk.a.clr must branch. */
	st8 [r20] = r0
	chk.a.clr r14, int_miss
	br.sptk.many fail

int_miss:
	/* FP ALAT hit path on advanced FP load + chk.a.clr. */
	movl r21 = 0x0102030405060708
	st8 [r20] = r21
	ldf8.a f6 = [r20]
	chk.a.clr f6, fail

	/* Alias store invalidates FP ALAT entry: chk.a.clr must branch. */
	st8 [r20] = r0
	chk.a.clr f6, fp_miss
	br.sptk.many fail

fp_miss:
	/* invala clears any remaining ALAT entries. */
	invala
	chk.a.nc r14, pass
	br.sptk.many fail

pass:
	movl r8 = 0x616c7470		/* "altp" marker in break log */
	break 0
	br.sptk.many pass

fail:
	movl r8 = 0x616c7466		/* "altf" marker in break log */
	break 0
	br.sptk.many fail
