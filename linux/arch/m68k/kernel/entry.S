/* -*- mode: asm -*-
 *
 *  linux/arch/m68k/kernel/entry.S
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file README.legal in the main directory of this archive
 * for more details.
 *
 * Linux/m68k support by Hamish Macdonald
 *
 * 68060 fixes by Jesper Skov
 *
 * SVR3 personality support by Dmitry K. Butskoy.
 */

/*
 * entry.S  contains the system-call and fault low-level handling routines.
 * This also contains the timer-interrupt handler, as well as all interrupts
 * and faults that can result in a task-switch.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call.
 *
 * Stack layout in 'ret_from_exception':
 *
 *	This allows access to the syscall arguments in registers d1-d5
 *
 *	 0(sp) - d1
 *	 4(sp) - d2
 *	 8(sp) - d3
 *	 C(sp) - d4
 *	10(sp) - d5
 *	14(sp) - a0
 *	18(sp) - a1
 *	1C(sp) - d0
 *	20(sp) - orig_d0
 *	24(sp) - stack adjustment
 *	28(sp) - sr
 *	2A(sp) - pc
 *	2E(sp) - format & vector
 */

/*
 * 12/03/96 Jes: Currently we only support m68k single-cpu systems, so
 *               all pointers that used to be 'current' are now entry
 *               number 0 in the 'current_set' list.
 */

#include <linux/sys.h>
#include <linux/config.h>
#include <linux/linkage.h>
#include <asm/setup.h>
#include <asm/segment.h>

LENOSYS = 38

/*
 * these are offsets into the task-struct
 */
LTASK_STATE	=  0
LTASK_COUNTER	=  4
LTASK_PRIORITY	=  8
LTASK_SIGNAL	= 12
LTASK_BLOCKED	= 16
LTASK_FLAGS	= 20

LTASK_PERS      = 100

PER_SVR3 = 0x2

LCF_MASK        = 0x0001

NR_svr3_syscalls=  140


/* the following macro is used when enabling interrupts */
#if defined(CONFIG_ATARI_ONLY)
	/* block out HSYNC on the atari */
#define ALLOWINT 0xfbff
#define	MAX_NOINT_IPL	3
#else
	/* portable version */
#define ALLOWINT 0xf8ff
#define	MAX_NOINT_IPL	0
#endif /* machine compilation types */ 

LD0		= 0x1C
LORIG_D0	= 0x20
LSR		= 0x28
LFORMATVEC	= 0x2E

/*
 * This defines the normal kernel pt-regs layout.
 *
 * regs are a2-a6 and d6-d7 preserved by C code
 * the kernel doesn't mess with usp unless it needs to
 */
#define SAVE_ALL			\
	clrl	%sp@-;    /* stk_adj */	\
	movel	%d0,%sp@-; /* orig d0 */	\
	movel	%d0,%sp@-; /* d0 */	\
	moveml	%d1-%d5/%a0-%a1,%sp@-

#define RESTORE_ALL			\
	moveml	%sp@+,%a0-%a1/%d1-%d5;	\
	movel	%sp@+,%d0;		\
	addql	#4,%sp;	 /* orig d0 */	\
	addl	%sp@+,%sp; /* stk adj */	\
	rte

#define SWITCH_STACK_SIZE (7*4+4)	/* includes return address */

#define SAVE_SWITCH_STACK \
	moveml	%a2-%a6/%d6-%d7,%sp@-

#define RESTORE_SWITCH_STACK \
	moveml	%sp@+,%a2-%a6/%d6-%d7

.globl SYMBOL_NAME(system_call), SYMBOL_NAME(buserr), SYMBOL_NAME(trap)
.globl SYMBOL_NAME(resume), SYMBOL_NAME(ret_from_exception)
.globl SYMBOL_NAME(ret_from_signal)
.globl SYMBOL_NAME(inthandler), SYMBOL_NAME(sys_call_table)
.globl SYMBOL_NAME(sys_fork), SYMBOL_NAME(sys_clone)
.globl SYMBOL_NAME(ret_from_interrupt), SYMBOL_NAME(bad_interrupt)

.text
ENTRY(buserr)
	SAVE_ALL
	moveq	#-1,%d0
	movel	%d0,%sp@(LORIG_D0)	| a -1 in the ORIG_D0 field
					| signifies that the stack frame
					| is NOT for syscall

	movel	%sp,%sp@- 		| stack frame pointer argument
        jsr     SYMBOL_NAME(buserr_c)
	addql	#4,%sp
	jra	SYMBOL_NAME(ret_from_exception)

ENTRY(trap)
	SAVE_ALL
	moveq	#-1,%d0
	movel	%d0,%sp@(LORIG_D0)	| a -1 in the ORIG_D0 field
					| signifies that the stack frame
					| is NOT for syscall
	movel	%sp,%sp@- 		| stack frame pointer argument
        jsr     SYMBOL_NAME(trap_c)
	addql	#4,%sp
	jra	SYMBOL_NAME(ret_from_exception)

ENTRY(reschedule)
	| save top of frame
	pea	%sp@
        jsr     SYMBOL_NAME(set_esp0)
	addql	#4,%sp

	pea	SYMBOL_NAME(ret_from_exception)
	jmp	SYMBOL_NAME(schedule)

ENTRY(system_call)
	SAVE_ALL
	movel	#-LENOSYS,LD0(%sp)	| default return value in d0
					| original D0 is in orig_d0
	movel	%d0,%d2

	| save top of frame
	pea	%sp@
        jsr     SYMBOL_NAME(set_esp0)
	addql	#4,%sp

        mov.l   SYMBOL_NAME(current_set),%a0
        cmp.b   &PER_SVR3,%a0@(LTASK_PERS+3)
        beq     svr3_sys_call

	cmpl	#NR_syscalls,%d2
	jcc	SYMBOL_NAME(ret_from_exception)
	lea	SYMBOL_NAME(sys_call_table),%a0
	movel	%a0@(%d2:l:4),%d3
	jeq	SYMBOL_NAME(ret_from_exception)
	movel	SYMBOL_NAME(current_set),%a0
	btst	#5,%a0@(LTASK_FLAGS+3)	| PF_TRACESYS
	bnes	1f
	movel	%d3,%a0
        jsr     %a0@
	movel	%d0,%sp@(LD0)		| save the return value
	jra	SYMBOL_NAME(ret_from_exception)
1:
	subql	#4,%sp
	SAVE_SWITCH_STACK
        jsr     SYMBOL_NAME(syscall_trace)
	RESTORE_SWITCH_STACK
	addql	#4,%sp
	movel	%d3,%a0
        jsr     %a0@
	movel	%d0,%sp@(LD0)		| save the return value
	subql	#4,%sp			| dummy return address
	SAVE_SWITCH_STACK
        jsr     SYMBOL_NAME(syscall_trace)

SYMBOL_NAME_LABEL(ret_from_signal)
	RESTORE_SWITCH_STACK
	addql	#4,%sp

SYMBOL_NAME_LABEL(ret_from_exception)
	btst	#5,%sp@(LSR)		| check if returning to kernel
	bnes	2f			| if so, skip resched, signals

	tstl	SYMBOL_NAME(need_resched)
	jne	SYMBOL_NAME(reschedule)
	movel	SYMBOL_NAME(current_set),%a0
	cmpl	#SYMBOL_NAME(task),%a0	| task[0] cannot have signals
	jeq	2f
	bclr	#5,%a0@(LTASK_FLAGS+1)	| check for delayed trace
	jeq	1f
	bclr	#7,%sp@(LSR)		| clear trace bit in SR
	pea	1			| send SIGTRAP
	movel	%a0,%sp@-
	pea	5
        jsr     SYMBOL_NAME(send_sig)
	addql	#8,%sp
	addql	#4,%sp
	movel	SYMBOL_NAME(current_set),%a0

1:
	tstl	%a0@(LTASK_STATE)	| state
	jne	SYMBOL_NAME(reschedule)
	tstl	%a0@(LTASK_COUNTER)	| counter
	jeq	SYMBOL_NAME(reschedule)

	movel	%a0@(LTASK_BLOCKED),%d0
	movel	%d0,%d1			| save blocked in d1 for sig handling
	notl	%d0
	btst	#4,%a0@(LTASK_FLAGS+3)	| PF_PTRACED
	jeq	1f
	moveq	#-1,%d0			| let the debugger see all signals
1:	andl	%a0@(LTASK_SIGNAL),%d0
	jne	Lsignal_return
2:	RESTORE_ALL

Lsignal_return:
	subql	#4,%sp			| dummy return address
	SAVE_SWITCH_STACK
	pea	%sp@(SWITCH_STACK_SIZE)
	movel	%d1,%sp@-
        jsr     SYMBOL_NAME(do_signal)
	addql	#8,%sp
	RESTORE_SWITCH_STACK
	addql	#4,%sp
	RESTORE_ALL


/*     This is the stuff for binary compatibility with
   AT&T UNIX System V  Release 3.1 ,  which was ported
   onto m68k family by Stollmann GmbH, Hamburg, etc.  */

svr3_sys_call:
        cmp.l   &NR_svr3_syscalls,%d2
        bcc     svr3_nosys
        lea     SYMBOL_NAME(svr3_syscalls_table),%a0
        mov.l   (%a0,%d2.l*8),%d3
        beq     svr3_nosys

        mov.l   0x4(%a0,%d2.l*8),%d2
        beq.b   1f
        mov.l   %d2,%d1
        mov.l   %usp,%a0
2:
        movs.l  (%a0,%d1.l*4),%d0
        mov.l   %d0,-(%sp)
        sub.l   &1,%d1
        bgt.b   2b
1:
        mov.l   SYMBOL_NAME(current_set),%a0
        btst    #5,%a0@(LTASK_FLAGS+3)  | PF_TRACESYS
        bne     1f

        mov.l   %d3,%a0
        jsr     (%a0)
        lea     (%sp,%d2.l*4),%sp       | pop the stack
        tst.l   %d0
        bpl.b   2f
        cmp.l   &0xffffff00,%d0         | is it `-errno' value`s range ?
        bcs     2f

svr3_error_ret:
        neg.l   %d0
        and.w   &0x007f,%d0
        lea.l   linux_svr3_errno,%a0
        mov.b   (%a0,%d0.w),%d0
        extb.l  %d0
        or.w    &LCF_MASK,%sp@(LSR)     | set carry to indicate error
2:
        mov.l   %d0,%sp@(LD0)           | save the return value
        bra     SYMBOL_NAME(ret_from_exception)

svr3_nosys:
        mov.l   &-LENOSYS,%d0           | to emulate `-ENOSYS' return
        bra     svr3_error_ret

1:
        sub.l   &4,%sp
        SAVE_SWITCH_STACK
        jsr     SYMBOL_NAME(syscall_trace)
        RESTORE_SWITCH_STACK
        add.l   &4,%sp
        mov.l   %d3,%a0
        jsr     (%a0)
        lea     (%sp,%d2.l*4),%sp       | pop the stack
        tst.l   %d0
        bpl.b   2f
        cmp.l   &0xffffff00,%d0         | is it `-errno' value`s range ?
        bcs     2f

        neg.l   %d0
        and.w   &0x007f,%d0
        lea.l   linux_svr3_errno,%a0
        mov.b   (%a0,%d0.w),%d0
        extb.l  %d0
        or.w    &LCF_MASK,%sp@(LSR)     | set carry to indicate error
2:
        mov.l   %d0,%sp@(LD0)           | save the return value
        sub.l   &4,%sp                  | dummy return address
        SAVE_SWITCH_STACK
        jsr     SYMBOL_NAME(syscall_trace)
        RESTORE_SWITCH_STACK
        add.l   #4,%sp
        bra     SYMBOL_NAME(ret_from_exception)

data
        global orig_ret_pc
orig_ret_pc:
        long 0

text
        global ret_after_svr3_exec
ret_after_svr3_exec:
        mov.l   orig_ret_pc, -(%sp)     |  restore orig return value...

        clr.l   %d6
        clr.l   %d7
        sub.l   %a2,%a2
        sub.l   %a3,%a3
        sub.l   %a4,%a4
        sub.l   %a5,%a5
        sub.l   %a6,%a6

        rts

/*
** This is the main interrupt handler, responsible for calling process_int()
*/
SYMBOL_NAME_LABEL(inthandler)
	SAVE_ALL
	moveq	#-1,%d0
	movel	%d0,%sp@(LORIG_D0)	| a -1 in the ORIG_D0 field
					| signifies that the stack frame
					| is NOT for syscall
	addql	#1,SYMBOL_NAME(intr_count)
					|  put exception # in d0
	bfextu %sp@(LFORMATVEC){#4,#10},%d0

	movel	%sp,%sp@-
	movel	%d0,%sp@- 		|  put vector # on stack
        jsr     SYMBOL_NAME(process_int)|  process the IRQ
	addql	#8,%sp			|  pop parameters off stack

SYMBOL_NAME_LABEL(ret_from_interrupt)
	/* check if we need to do software interrupts */
1:
	movel	SYMBOL_NAME(intr_count),%d1
	subql	#1,%d1
	jne	4f
	bfextu  %sp@(LSR){#5,#3},%d0    | Check for nested interrupt.
#if MAX_NOINT_IPL > 0
	cmpiw	#MAX_NOINT_IPL,%d0
#endif
	jhi	4f
2:
	movel	SYMBOL_NAME(bh_active),%d0
	andl	SYMBOL_NAME(bh_mask),%d0
	jne	3f

	clrl	SYMBOL_NAME(intr_count)	| deliver signals, reschedule etc..
	jra	SYMBOL_NAME(ret_from_exception)
3:	
        jsr     SYMBOL_NAME(do_bottom_half)
        jra     2b
4:
	movel	%d1,SYMBOL_NAME(intr_count)
	RESTORE_ALL


/* Handler for uninitialized and spurious interrupts */

SYMBOL_NAME_LABEL(bad_interrupt)
	addql	#1,SYMBOL_NAME(num_spurious)
	rte

ENTRY(sys_fork)
	SAVE_SWITCH_STACK	
	pea	%sp@(SWITCH_STACK_SIZE)
        jsr     SYMBOL_NAME(m68k_fork)
	addql	#4,%sp
	RESTORE_SWITCH_STACK
	rts

ENTRY(sys_clone)
	SAVE_SWITCH_STACK
	pea	%sp@(SWITCH_STACK_SIZE)
        jsr     SYMBOL_NAME(m68k_clone)
	addql	#4,%sp
	RESTORE_SWITCH_STACK
	rts

ENTRY(sys_sigsuspend)
	SAVE_SWITCH_STACK
	pea	%sp@(SWITCH_STACK_SIZE)
        jsr     SYMBOL_NAME(do_sigsuspend)
	addql	#4,%sp
	RESTORE_SWITCH_STACK
	rts

ENTRY(sys_sigreturn)
	SAVE_SWITCH_STACK
        jsr     SYMBOL_NAME(do_sigreturn)
        RESTORE_SWITCH_STACK
        rts

ENTRY(svr3_fork)
        SAVE_SWITCH_STACK
        pea     %sp@(SWITCH_STACK_SIZE)
        jsr     SYMBOL_NAME(svr3_m68k_fork)
        addql   #4,%sp
	RESTORE_SWITCH_STACK
	rts

LFLUSH_I_AND_D = 0x00000808
LTSS_KSP	= 0
LTSS_USP	= 4
LTSS_SR		= 8
LTSS_FS		= 10
LTSS_CRP	= 12
LTSS_FPCTXT	= 24

SYMBOL_NAME_LABEL(resume)
	/*
	 * Beware - when entering resume, offset of tss is in d1,
	 * prev (the current task) is in a0, next (the new task)
	 * is in a1 and d2.b is non-zero if the mm structure is
	 * shared between the tasks, so don't change these
	 * registers until their contents are no longer needed.
	 */

	/* offset of tss struct (processor state) from beginning
	   of task struct */
	addl	%d1,%a0

	/* save sr */
	movew	%sr,%a0@(LTSS_SR)

	/* disable interrupts */
	oriw	#0x0700,%sr

	/* save fs (sfc,%dfc) (may be pointing to kernel memory) */
	movec	%sfc,%d0
	movew	%d0,%a0@(LTSS_FS)

	/* save usp */
	/* it is better to use a movel here instead of a movew 8*) */
	movec	%usp,%d0
	movel	%d0,%a0@(LTSS_USP)

	/* save non-scratch registers on stack */
	SAVE_SWITCH_STACK

	/* save current kernel stack pointer */
	movel	%sp,%a0@(LTSS_KSP)

	/* save floating point context */
	fsave	%a0@(LTSS_FPCTXT+27*4)

#if defined(CONFIG_M68060)
#if !defined(CONFIG_M68060_ONLY)
	btst	#3,SYMBOL_NAME(boot_info)+BI_cputype+3
	beqs	1f
#endif
	/* The 060 FPU keeps status in bits 15-8 of the first longword */
	tstb	%a0@(LTSS_FPCTXT+27*4+2)
	jeq	3f
#if !defined(CONFIG_M68060_ONLY)
	jra	2f
#endif
#endif /* CONFIG_M68060 */
#if !defined(CONFIG_M68060_ONLY)
1:	tstb	%a0@(LTSS_FPCTXT+27*4)
	jeq	3f
#endif
2:	fmovemx	%fp0-%fp7,%a0@(LTSS_FPCTXT)
	fmoveml	%fpcr/%fpsr/%fpiar,%a0@(LTSS_FPCTXT+24*4)
3:

	/* get pointer to tss struct (a1 contains new task) */
	movel	%a1,SYMBOL_NAME(current_set)
	addl	%d1,%a1

	/* Skip address space switching if they are the same. */
	tstb	%d2
	jne	4f

#if defined(CONFIG_M68020_OR_M68030) && defined(CONFIG_M68040_OR_M68060)
	/* 68040 or 68060 ? */
	tstl	SYMBOL_NAME(m68k_is040or060)
	bnes	1f
#endif
#if defined(CONFIG_M68020_OR_M68030)
	/*
	 * switch address space
	 */

	/* flush MC68030/MC68020 caches (they are virtually addressed) */
	movec	%cacr,%d0
	oriw	#LFLUSH_I_AND_D,%d0
	movec	%d0,%cacr
	
	/* switch the root pointer */
	pmove	%a1@(LTSS_CRP),%crp
#endif

#if defined(CONFIG_M68020_OR_M68030) && defined(CONFIG_M68040_OR_M68060)
	jra	2f	/* skip m68040 stuff */
1:
#endif
#if defined(CONFIG_M68040_OR_M68060)
	/*
	 * switch address space
	 */

	/* flush address translation cache (user entries) */
	.word	0xf510			/* pflushan */

	/* switch the root pointer */
	movel	%a1@(LTSS_CRP+4),%d0
	.long	0x4e7b0806		/* movec d0,urp */

#if defined (CONFIG_M68060)
	/* is it a '060 ? */
	btst	#3,SYMBOL_NAME(boot_info)+BI_cputype+3
	beqs	2f
	/* clear user entries in the branch cache */
	movec	%cacr,%d0
	orl	#0x00200000,%d0
	movec	%d0,%cacr
#endif /* CONFIG_M68060 */
#endif /* CONFIG_M68040_OR_M68060 */
  2:
  4:
	/* restore floating point context */

#if defined(CONFIG_M68060)
#if !defined(CONFIG_M68060_ONLY)
	btst	#3,SYMBOL_NAME(boot_info)+BI_cputype+3
	beqs	1f
#endif
	/* The 060 FPU keeps status in bits 15-8 of the first longword */
	tstb	%a1@(LTSS_FPCTXT+27*4+2)
	jeq	3f
#if !defined(CONFIG_M68060_ONLY)
	jra	2f
#endif
#endif /* CONFIG_M68060 */
#if !defined(CONFIG_M68060_ONLY)
1:	tstb	%a1@(LTSS_FPCTXT+27*4)
	jeq	3f
#endif	
2:	fmovemx	%a1@(LTSS_FPCTXT),%fp0-%fp7
	fmoveml	%a1@(LTSS_FPCTXT+24*4),%fpcr/%fpsr/%fpiar
3:	frestore %a1@(LTSS_FPCTXT+27*4)

	/* restore the kernel stack pointer */
	movel	%a1@(LTSS_KSP),%sp

	/* restore non-scratch registers */
	RESTORE_SWITCH_STACK

	/* restore user stack pointer */
	movel	%a1@(LTSS_USP),%a0
	movel	%a0,%usp

	/* restore fs (sfc,%dfc) */
	movew	%a1@(LTSS_FS),%a0
	movec	%a0,%sfc
	movec	%a0,%dfc

	/* restore status register */
	movew	%a1@(LTSS_SR),%sr

	rts

.data
ALIGN
SYMBOL_NAME_LABEL(sys_call_table)
	.long SYMBOL_NAME(sys_setup)		/* 0 */
	.long SYMBOL_NAME(sys_exit)
	.long SYMBOL_NAME(sys_fork)
	.long SYMBOL_NAME(sys_read)
	.long SYMBOL_NAME(sys_write)
	.long SYMBOL_NAME(sys_open)		/* 5 */
	.long SYMBOL_NAME(sys_close)
	.long SYMBOL_NAME(sys_waitpid)
	.long SYMBOL_NAME(sys_creat)
	.long SYMBOL_NAME(sys_link)
	.long SYMBOL_NAME(sys_unlink)		/* 10 */
	.long SYMBOL_NAME(sys_execve)
	.long SYMBOL_NAME(sys_chdir)
	.long SYMBOL_NAME(sys_time)
	.long SYMBOL_NAME(sys_mknod)
	.long SYMBOL_NAME(sys_chmod)		/* 15 */
	.long SYMBOL_NAME(sys_chown)
	.long SYMBOL_NAME(sys_break)
	.long SYMBOL_NAME(sys_stat)
	.long SYMBOL_NAME(sys_lseek)
	.long SYMBOL_NAME(sys_getpid)		/* 20 */
	.long SYMBOL_NAME(sys_mount)
	.long SYMBOL_NAME(sys_umount)
	.long SYMBOL_NAME(sys_setuid)
	.long SYMBOL_NAME(sys_getuid)
	.long SYMBOL_NAME(sys_stime)		/* 25 */
	.long SYMBOL_NAME(sys_ptrace)
	.long SYMBOL_NAME(sys_alarm)
	.long SYMBOL_NAME(sys_fstat)
	.long SYMBOL_NAME(sys_pause)
	.long SYMBOL_NAME(sys_utime)		/* 30 */
	.long SYMBOL_NAME(sys_stty)
	.long SYMBOL_NAME(sys_gtty)
	.long SYMBOL_NAME(sys_access)
	.long SYMBOL_NAME(sys_nice)
	.long SYMBOL_NAME(sys_ftime)		/* 35 */
	.long SYMBOL_NAME(sys_sync)
	.long SYMBOL_NAME(sys_kill)
	.long SYMBOL_NAME(sys_rename)
	.long SYMBOL_NAME(sys_mkdir)
	.long SYMBOL_NAME(sys_rmdir)		/* 40 */
	.long SYMBOL_NAME(sys_dup)
	.long SYMBOL_NAME(sys_pipe)
	.long SYMBOL_NAME(sys_times)
	.long SYMBOL_NAME(sys_prof)
	.long SYMBOL_NAME(sys_brk)		/* 45 */
	.long SYMBOL_NAME(sys_setgid)
	.long SYMBOL_NAME(sys_getgid)
	.long SYMBOL_NAME(sys_signal)
	.long SYMBOL_NAME(sys_geteuid)
	.long SYMBOL_NAME(sys_getegid)		/* 50 */
	.long SYMBOL_NAME(sys_acct)
	.long SYMBOL_NAME(sys_phys)
	.long SYMBOL_NAME(sys_lock)
	.long SYMBOL_NAME(sys_ioctl)
	.long SYMBOL_NAME(sys_fcntl)		/* 55 */
	.long SYMBOL_NAME(sys_mpx)
	.long SYMBOL_NAME(sys_setpgid)
	.long SYMBOL_NAME(sys_ulimit)
	.long SYMBOL_NAME(sys_olduname)
	.long SYMBOL_NAME(sys_umask)		/* 60 */
	.long SYMBOL_NAME(sys_chroot)
	.long SYMBOL_NAME(sys_ustat)
	.long SYMBOL_NAME(sys_dup2)
	.long SYMBOL_NAME(sys_getppid)
	.long SYMBOL_NAME(sys_getpgrp)		/* 65 */
	.long SYMBOL_NAME(sys_setsid)
	.long SYMBOL_NAME(sys_sigaction)
	.long SYMBOL_NAME(sys_sgetmask)
	.long SYMBOL_NAME(sys_ssetmask)
	.long SYMBOL_NAME(sys_setreuid)		/* 70 */
	.long SYMBOL_NAME(sys_setregid)
	.long SYMBOL_NAME(sys_sigsuspend)
	.long SYMBOL_NAME(sys_sigpending)
	.long SYMBOL_NAME(sys_sethostname)
	.long SYMBOL_NAME(sys_setrlimit)	/* 75 */
	.long SYMBOL_NAME(sys_getrlimit)
	.long SYMBOL_NAME(sys_getrusage)
	.long SYMBOL_NAME(sys_gettimeofday)
	.long SYMBOL_NAME(sys_settimeofday)
	.long SYMBOL_NAME(sys_getgroups)	/* 80 */
	.long SYMBOL_NAME(sys_setgroups)
	.long SYMBOL_NAME(old_select)
	.long SYMBOL_NAME(sys_symlink)
	.long SYMBOL_NAME(sys_lstat)
	.long SYMBOL_NAME(sys_readlink)		/* 85 */
	.long SYMBOL_NAME(sys_uselib)
	.long SYMBOL_NAME(sys_swapon)
	.long SYMBOL_NAME(sys_reboot)
	.long SYMBOL_NAME(old_readdir)
	.long SYMBOL_NAME(old_mmap)		/* 90 */
	.long SYMBOL_NAME(sys_munmap)
	.long SYMBOL_NAME(sys_truncate)
	.long SYMBOL_NAME(sys_ftruncate)
	.long SYMBOL_NAME(sys_fchmod)
	.long SYMBOL_NAME(sys_fchown)		/* 95 */
	.long SYMBOL_NAME(sys_getpriority)
	.long SYMBOL_NAME(sys_setpriority)
	.long SYMBOL_NAME(sys_profil)
	.long SYMBOL_NAME(sys_statfs)
	.long SYMBOL_NAME(sys_fstatfs)		/* 100 */
	.long SYMBOL_NAME(sys_ioperm)
	.long SYMBOL_NAME(sys_socketcall)
	.long SYMBOL_NAME(sys_syslog)
	.long SYMBOL_NAME(sys_setitimer)
	.long SYMBOL_NAME(sys_getitimer)	/* 105 */
	.long SYMBOL_NAME(sys_newstat)
	.long SYMBOL_NAME(sys_newlstat)
	.long SYMBOL_NAME(sys_newfstat)
	.long SYMBOL_NAME(sys_uname)
	.long SYMBOL_NAME(sys_ni_syscall)	/* iopl for i386 */ /* 110 */
	.long SYMBOL_NAME(sys_vhangup)
	.long SYMBOL_NAME(sys_idle)
	.long SYMBOL_NAME(sys_ni_syscall)	/* vm86 for i386 */
	.long SYMBOL_NAME(sys_wait4)
	.long SYMBOL_NAME(sys_swapoff)		/* 115 */
	.long SYMBOL_NAME(sys_sysinfo)
	.long SYMBOL_NAME(sys_ipc)
	.long SYMBOL_NAME(sys_fsync)
	.long SYMBOL_NAME(sys_sigreturn)
	.long SYMBOL_NAME(sys_clone)		/* 120 */
	.long SYMBOL_NAME(sys_setdomainname)
	.long SYMBOL_NAME(sys_newuname)
	.long SYMBOL_NAME(sys_cacheflush)	/* modify_ldt for i386 */
	.long SYMBOL_NAME(sys_adjtimex)
	.long SYMBOL_NAME(sys_mprotect)		/* 125 */
	.long SYMBOL_NAME(sys_sigprocmask)
	.long SYMBOL_NAME(sys_create_module)
	.long SYMBOL_NAME(sys_init_module)
	.long SYMBOL_NAME(sys_delete_module)
	.long SYMBOL_NAME(sys_get_kernel_syms)	/* 130 */
	.long SYMBOL_NAME(sys_quotactl)
	.long SYMBOL_NAME(sys_getpgid)
	.long SYMBOL_NAME(sys_fchdir)
	.long SYMBOL_NAME(sys_bdflush)
	.long SYMBOL_NAME(sys_sysfs)		/* 135 */
	.long SYMBOL_NAME(sys_personality)
	.long SYMBOL_NAME(sys_ni_syscall)	/* for afs_syscall */
	.long SYMBOL_NAME(sys_setfsuid)
	.long SYMBOL_NAME(sys_setfsgid)
	.long SYMBOL_NAME(sys_llseek)		/* 140 */
	.long SYMBOL_NAME(sys_getdents)
	.long SYMBOL_NAME(sys_select)
	.long SYMBOL_NAME(sys_flock)
	.long SYMBOL_NAME(sys_msync)
	.long SYMBOL_NAME(sys_readv)		/* 145 */
	.long SYMBOL_NAME(sys_writev)
	.long SYMBOL_NAME(sys_getsid)
	.long SYMBOL_NAME(sys_fdatasync)
	.long SYMBOL_NAME(sys_sysctl)
	.long SYMBOL_NAME(sys_mlock)		/* 150 */
	.long SYMBOL_NAME(sys_munlock)
	.long SYMBOL_NAME(sys_mlockall)
	.long SYMBOL_NAME(sys_munlockall)
	.long SYMBOL_NAME(sys_sched_setparam)
	.long SYMBOL_NAME(sys_sched_getparam)   /* 155 */
	.long SYMBOL_NAME(sys_sched_setscheduler)
	.long SYMBOL_NAME(sys_sched_getscheduler)
	.long SYMBOL_NAME(sys_sched_yield)
	.long SYMBOL_NAME(sys_sched_get_priority_max)
	.long SYMBOL_NAME(sys_sched_get_priority_min)  /* 160 */
	.long SYMBOL_NAME(sys_sched_rr_get_interval)
	.long SYMBOL_NAME(sys_nanosleep)
	.long SYMBOL_NAME(sys_mremap)
	.space (NR_syscalls-163)*4

/*          svr3 syscalls              */
ALIGN
SYMBOL_NAME_LABEL(svr3_syscalls_table)
        /*      Linux                           svr3        */

        long    0,             0    /*  0       nosys   */
        long    sys_exit,      1    /*  1       rexit   */
        long    svr3_fork,     0    /*  2       fork    */
        long    svr3_read,     3    /*  3       read    */
        long    sys_write,     3    /*  4       write   */
        long    svr3_open,     3    /*  5       open    */
        long    sys_close,     1    /*  6       close   */
        long    svr3_wait,     0    /*  7       wait    */
        long    sys_creat,     2    /*  8       creat   */
        long    sys_link,      2    /*  9       link    */
        long    sys_unlink,    1    /* 10       unlink  */
        long    svr3_exec,     2    /* 11       exec    */
        long    sys_chdir,     1    /* 12       chdir   */
        long    svr3_time,     0    /* 13       gtime   */
        long    sys_mknod,     3    /* 14       mknod   */
        long    sys_chmod,     2    /* 15       chmod   */
        long    sys_chown,     3    /* 16       chown   */
        long    svr3_sbreak,   1    /* 17       sbreak  */
        long    sys_stat,      2    /* 18       stat    */
        long    sys_lseek,     3    /* 19       seek    */
        long    svr3_getpid,   0    /* 20       getpid  */
        long    svr3_mount,    6    /* 21       smount  */
        long    sys_umount,    1    /* 22       sumount */
        long    sys_setuid,    1    /* 23       setuid  */
        long    svr3_getuid,   0    /* 24       getuid  */
        long    svr3_stime,    1    /* 25       stime   */
        long    0,             4    /* 26       ptrace  */
        long    sys_alarm,     1    /* 27       alarm   */
        long    sys_fstat,     2    /* 28       fstat   */
        long    sys_pause,     0    /* 29       pause   */
        long    sys_utime,     2    /* 30       utime   */
        long    sys_stty,      2    /* 31       stty    */
        long    sys_gtty,      2    /* 32       gtty    */
        long    sys_access,    2    /* 33       saccess */
        long    sys_nice,      1    /* 34       nice    */
        long    svr3_statfs,   4    /* 35       statfs  */
        long    sys_sync,      0    /* 36       sync    */
        long    svr3_kill,     2    /* 37       kill    */
        long    svr3_fstatfs,  4    /* 38       fstatfs */
        long    svr3_setpgrp,  1    /* 39       setpgrp */
        long    0,             0    /* 40       nosys   */
        long    sys_dup,       1    /* 41       dup     */
        long    svr3_pipe,     0    /* 42       pipe    */
        long    sys_times,     1    /* 43       times   */
        long    sys_profil,    4    /* 44       profil  */
        long    svr3_lock,     1    /* 45       lock    */
        long    sys_setgid,    1    /* 46       setgid  */
        long    svr3_getgid,   0    /* 47       getgid  */
        long    svr3_ssig,     2    /* 48       ssig    */
        long    svr3_msgsys,   6    /* 49       msgsys  */
        long    svr3_sysm68k,  4    /* 50       sysm68k */
        long    0,             1    /* 51       sysacct */
        long    svr3_shmsys,   4    /* 52       shmsys  */
        long    svr3_semsys,   5    /* 53       semsys  */
        long    svr3_ioctl,    3    /* 54       ioctl   */
        long    svr3_uadmin,   3    /* 55       uadmin  */
        long    0,             0    /* 56       nosys   */
        long    svr3_utssys,   3    /* 57       utssys  */
        long    0,             0    /* 58       nosys   */
        long    svr3_exece,    3    /* 59       exece   */
        long    sys_umask,     1    /* 60       umask   */
        long    sys_chroot,    1    /* 61       chroot  */
        long    svr3_fcntl,    3    /* 62       fcntl   */
        long    svr3_ulimit,   2    /* 63       ulimit  */
        long    0,             0    /* 64       nosys   */
        long    0,             0    /* 65       nosys   */
        long    0,             0    /* 66       nosys   */
        long    0,             0    /* 67       nosys   */
        long    0,             0    /* 68       nosys   */
        long    0,             0    /* 69       nosys   */
        long    0,             4    /* 70       advfs   */
        long    0,             1    /* 71       unadvfs */
        long    0,             4    /* 72       rmount  */
        long    0,             1    /* 73       rumount */
        long    0,             5    /* 74       rfstart */
        long    0,             0    /* 75       nosys   */
        long    0,             1    /* 76       rdebug  */
        long    0,             0    /* 77       rfstop  */
        long    0,             6    /* 78       rfsys   */
        long    sys_rmdir,     1    /* 79       rmdir   */
        long    sys_mkdir,     2    /* 80       mkdir   */
        long    sys_getdents,  4    /* 81       getdents        */
        long    0,             3    /* 82       libattach       */
        long    0,             1    /* 83       libdetach       */
        long    sys_sysfs,     3    /* 84       sysfs   */
        long    svr3_getmsg,   4    /* 85       getmsg  */
        long    svr3_putmsg,   4    /* 86       putmsg  */
        long    svr3_poll,     3    /* 87       poll    */
        long    0,             0    /* 88               */
        long    0,             0    /* 89               */
        long    old_mmap,      1    /* 90    extension for svr3     */
        long    sys_munmap,    2    /* 91    extension for svr3     */
        long    0,             0    /* 92               */
        long    0,             0    /* 93               */
        long    0,             0    /* 94               */
        long    0,             0    /* 95               */
        long    0,             0    /* 96               */
        long    0,             0    /* 97               */
        long    0,             0    /* 98               */
        long    0,             0    /* 99               */
        long    0,             0    /* 100              */
        long    old_select,    1    /* 101   extension for svr3     */
        long  svr3_socketcall, 2    /* 102   extension for svr3     */
        long    0,             0    /* 103              */
        long    0,             0    /* 104              */
        long    0,             0    /* 105              */
        long    0,             0    /* 106              */
        long    0,             0    /* 107              */
        long    0,             0    /* 108              */
        long    0,             0    /* 109              */
        long    0,             0    /* 110              */
        long    0,             0    /* 111              */
        long    0,             0    /* 112              */
        long    0,             0    /* 113              */
        long    0,             0    /* 114              */
        long    0,             0    /* 115              */
        long    0,             0    /* 116              */
        long    0,             0    /* 117              */
        long    0,             0    /* 118              */
        long    sys_sigreturn, 0    /* 119  the same value as under Linux,
                                     because sigreturn is in order context. */
        long    0,             0    /* 120              */
        long    0,             0    /* 121              */
        long    0,             0    /* 122              */
        long    0,             0    /* 123              */
        long    0,             0    /* 124              */
        long    0,             0    /* 125              */
        long    0,             0    /* 126              */
        long    0,             0    /* 127              */
        long    0,             0    /* 128              */
        long    0,             0    /* 129              */
        long    0,             0    /* 130              */
        long    0,             0    /* 131              */
        long    0,             0    /* 132              */
        long    0,             0    /* 133              */
        long    0,             0    /* 134              */
        long    0,             0    /* 135              */
        long  sys_personality, 1    /* 136   extension for svr3     */
        long    0,             0    /* 137              */
        long    0,             0    /* 138              */
        long    0,             0    /* 139              */
        long    0,             0    /* 140              */


text
        align 4
data
        align 4
