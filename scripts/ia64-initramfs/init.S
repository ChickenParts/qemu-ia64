	.text
	.align 16
	.global _start
_start:
	/* Ensure we have a valid stacked frame for syscall args. */
	alloc r2 = ar.pfs, 0, 0, 8, 0
	;;

	/*
	 * Some kernels may not preserve device nodes from an external initramfs
	 * (or the table may be unpacked before /dev exists). Create the minimal
	 * set we need so we can always log progress.
	 */

	/* mkdir("/dev", 0755) */
	mov r15 = 1055			/* __NR_mkdir */
	movl r32 = dev_dir
	mov r33 = 0755
	break 0x100000

	/* mknod("/dev/console", S_IFCHR|0600, makedev(5,1)) */
	mov r15 = 1037			/* __NR_mknod */
	movl r32 = console_path
	mov r33 = 0x2180		/* 0020000 | 0600 */
	mov r34 = 0x501
	break 0x100000

	/* mknod("/dev/null", S_IFCHR|0666, makedev(1,3)) */
	mov r15 = 1037			/* __NR_mknod */
	movl r32 = null_path
	mov r33 = 0x21b6		/* 0020000 | 0666 */
	mov r34 = 0x103
	break 0x100000

	/* mknod("/dev/kmsg", S_IFCHR|0644, makedev(1,11)) */
	mov r15 = 1037			/* __NR_mknod */
	movl r32 = kmsg_path
	mov r33 = 0x21a4		/* 0020000 | 0644 */
	mov r34 = 0x10b
	break 0x100000

	/* Try /dev/kmsg first (preferred), then fall back to /dev/console. */

	/* open("/dev/kmsg", O_WRONLY) */
	mov r15 = 1028			/* __NR_open */
	movl r32 = kmsg_path
	mov r33 = 1			/* O_WRONLY */
	mov r34 = 0
	break 0x100000
	cmp.eq p6,p0 = r10, r0		/* r10==0 on success */
(p6)	br.cond.spnt kmsg_ok

	/* open("/dev/console", O_WRONLY) */
	mov r15 = 1028			/* __NR_open */
	movl r32 = console_path
	mov r33 = 1			/* O_WRONLY */
	mov r34 = 0
	break 0x100000
	cmp.eq p6,p0 = r10, r0		/* r10==0 on success */
(p6)	br.cond.spnt con_ok
	br.sptk.many loop

kmsg_ok:
	/* write(fd, msg, len) */
	mov r15 = 1027			/* __NR_write */
	mov r32 = r8
	movl r33 = kmsg_msg
	mov r34 = (kmsg_msg_end - kmsg_msg)
	break 0x100000
	br.sptk.many loop

con_ok:
	mov r15 = 1027			/* __NR_write */
	mov r32 = r8
	movl r33 = con_msg
	mov r34 = (con_msg_end - con_msg)
	break 0x100000
	br.sptk.many loop

	/* nanosleep(&ts, NULL) */
loop:
	mov r15 = 1168			/* __NR_nanosleep */
	movl r32 = ts
	mov r33 = r0
	break 0x100000
	br.sptk.many loop

	/* exit(0) */
	mov r15 = 1025			/* __NR_exit */
	mov r32 = 0
	break 0x100000

	.section .rodata
dev_dir:
	.asciz "/dev"
console_path:
	.asciz "/dev/console"
null_path:
	.asciz "/dev/null"
kmsg_path:
	.asciz "/dev/kmsg"
con_msg:
	.ascii "IA64 init: hello from /init\n"
con_msg_end:
kmsg_msg:
	.ascii "<6>IA64 init: hello from /init\n"
kmsg_msg_end:

	.data
	.align 8
ts:
	.quad 1				/* tv_sec */
	.quad 0				/* tv_nsec */
