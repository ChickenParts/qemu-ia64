	.text
	.align 16
	.global _start
_start:
	/* Ensure we have a valid stacked frame for in0..in7 syscall args. */
	alloc r2 = ar.pfs, 8, 0, 0, 0
	;;

	/* open("/dev/kmsg", O_WRONLY) */
	mov r15 = 1028			/* __NR_open */
	movl r32 = kmsg_path
	mov r33 = 1			/* O_WRONLY */
	mov r34 = 0
	break 0x100000
	cmp.ne p6,p0 = r10, r0		/* r10==-1 on error */
(p6)	br.cond.spnt 1f

	/* write(fd, msg, len) */
	mov r15 = 1027			/* __NR_write */
	mov r32 = r8
	movl r33 = kmsg_msg
	mov r34 = (kmsg_msg_end - kmsg_msg)
	break 0x100000

1:
	/* nanosleep(&ts, NULL) */
	mov r15 = 1168			/* __NR_nanosleep */
	movl r32 = ts
	mov r33 = r0
	break 0x100000
	br.sptk.many 1b

	/* exit(0) */
	mov r15 = 1025			/* __NR_exit */
	mov r32 = 0
	break 0x100000

	.section .rodata
kmsg_path:
	.asciz "/dev/kmsg"
kmsg_msg:
	.ascii "<6>IA64 init: hello from /init\n"
kmsg_msg_end:

	.data
	.align 8
ts:
	.quad 1				/* tv_sec */
	.quad 0				/* tv_nsec */
