	.text
	.align 16
	.global _start
_start:
	/* Use current backing-store base (aligned) as scratch region. */
	mov r20 = ar.bspstore
	movl r21 = -8
	and r20 = r20, r21

	/* loadrs consumes loadrs bytes and advances BSP/BSPSTORE together. */
	movl r22 = 0x00100000		/* ar.rsc.loadrs = 16 bytes */
	mov ar.rsc = r22
	loadrs

check_loadrs_zero:
	mov r23 = ar.rsc
	shr.u r23 = r23, 16
	movl r24 = 0x3fff
	and r23 = r23, r24
	movl r9 = 1
	cmp.eq p6,p0 = r23, r0
(p6)	br.cond.spnt check_loadrs_sync
	br.sptk.many fail

check_loadrs_sync:
	mov r25 = ar.bsp
	mov r26 = ar.bspstore
	movl r9 = 2
	cmp.eq p6,p0 = r25, r26
(p6)	br.cond.spnt check_loadrs_advanced
	br.sptk.many fail

check_loadrs_advanced:
	adds r27 = 16, r20
	movl r9 = 3
	cmp.eq p6,p0 = r26, r27
(p6)	br.cond.spnt test_flushrs
	br.sptk.many fail

test_flushrs:
	movl r22 = 0x00200000		/* ar.rsc.loadrs = 32 bytes */
	mov ar.rsc = r22
	mov r28 = ar.rsc
	shr.u r28 = r28, 16
	movl r29 = 0x3fff
	and r28 = r28, r29
	movl r30 = 32
	movl r9 = 4
	cmp.eq p6,p0 = r28, r30
(p6)	br.cond.spnt do_flushrs
	br.sptk.many fail

do_flushrs:
	flushrs
	mov r28 = ar.rsc
	shr.u r28 = r28, 16
	and r28 = r28, r29
	movl r9 = 5
	cmp.eq p6,p0 = r28, r0
(p6)	br.cond.spnt check_flushrs_sync
	br.sptk.many fail

check_flushrs_sync:
	mov r25 = ar.bsp
	mov r26 = ar.bspstore
	movl r9 = 6
	cmp.eq p6,p0 = r25, r26
(p6)	br.cond.spnt pass
	br.sptk.many fail

pass:
	movl r8 = 0x72736570		/* "rsep" marker in break log */
	break 0
	br.sptk.many pass

fail:
	movl r8 = 0x72736566		/* "rsef" marker in break log */
	break 0
	br.sptk.many fail
