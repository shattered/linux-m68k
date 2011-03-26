/*
 * besta/svr3sys.c -- Transparent support for native Bestas sysv executables
 *		      under Linux -- "System V binary compatibility".
 *		      Builded into kernel as PER_SVR3 personality.
 *
 * Copyright 1996, 1997	    Dmitry K. Butskoy
 *			    <buc@citadel.stu.neva.ru>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/a.out.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/user.h>
#include <linux/malloc.h>
#include <linux/binfmts.h>
#include <linux/personality.h>
#include <linux/timex.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/sem.h>
#include <linux/shm.h>
#include <linux/utsname.h>
#include <linux/dirent.h>
#include <linux/termios.h>
#include <linux/resource.h>

#include <asm/setup.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/pgtable.h>

#include <linux/config.h>

/*  Prototypes for ordinary Linux`s syscalls   */
extern int sys_pipe (int *),
	   sys_stime (int *),
	   sys_setsid (void),
	   sys_fcntl (unsigned int, unsigned int, unsigned long),
	   sys_statfs (const char *, struct statfs *),
	   sys_fstatfs (int, struct statfs *),
	   sys_sysfs (int, ...),
	   sys_mount (const char *, const char *, const char *,
					unsigned long, const void *),
	   sys_olduname (struct oldold_utsname *),
	   sys_ustat (int, struct ustat *),
	   sys_reboot (int, int, int),
	   sys_sigaction (int, struct sigaction *, struct sigaction *),
	   sys_pause (void),
	   sys_signal (int, void (*) (int)),
	   sys_kill (int, int),
	   sys_wait4 (int, unsigned long *, int, struct rusage *),
	   sys_ioctl (int, int, ...),
	   sys_brk (unsigned long),
	   old_readdir (int, struct dirent *, int),
	   sys_lseek (int, unsigned long, int),
	   sys_mlock (unsigned long, unsigned int),
	   sys_munlock (unsigned long, unsigned int),
	   sys_getrlimit (unsigned long, struct rlimit *),
	   sys_setrlimit (unsigned long, struct rlimit *),
	   sys_select (int, void *, void *, void *, struct timeval *),
	   sys_swapon (const char *, int),
	   sys_swapoff (const char *),
	   sys_socketcall (int, unsigned long *);

/*  svr3 syscalls without arguments   */

/*   Parent: d0 - child pid, d1 - 0
 *   Child: d0 - parent pid, d1 - 1   */

int svr3_m68k_fork (struct pt_regs *regs) {
	int res;
	unsigned short flags;

	res = do_fork(SIGCHLD, rdusp(), regs);
	if (res != 0)  regs->d1 = 0;

	/*   This is not reached because `copy_thread()'
	   cause child to jump to `ret_from_exception' immediately.
	else {
	    res = current->p_pptr->pid;
	    regs->d1 = 1;
	}
	*/

	/*  Cause new child to return svr3-style values,
	  and cause the child to run first (as svr3 do it).
	    I hope, after Linux`s native `do_fork()' the child
	  has `/=2' priority, so, at this point we can catch
	  the child not yet running...
	*/
	save_flags (flags);
	cli();

	/*  the same proc, yet not running...  */
	if (current->p_cptr->pid == res &&
	    current->p_cptr->utime == 0 &&
	    current->p_cptr->stime == 0
	) {
	    regs = (struct pt_regs *) (current->p_cptr->tss.ksp +
					    sizeof (struct switch_stack));

	    regs->d0 = current->p_cptr->p_pptr->pid;    /*  just in case  */
	    regs->d1 = 1;

	    /*  After svr3`s fork()  child running first.  */
	    current->p_cptr->counter =
		(current->p_cptr->counter >> 1) + current->p_cptr->priority;
	    need_resched = 1;

	}
	restore_flags (flags);

	return res;
}

int svr3_time (void) {
	int i;

	i = CURRENT_TIME;

	return i;
}

int svr3_getpid (volatile struct pt_regs regs) {
	int res;

	res = current->pid;                 /* pid  */
	regs.d1 = current->p_opptr->pid;    /* ppid */

	return res;
}

int svr3_getuid (volatile struct pt_regs regs) {
	int res;

	res = current->uid;                 /* uid  */
	regs.d1 = current->euid;            /* euid */

	return res;
}

int svr3_getgid (volatile struct pt_regs regs) {
	int res;

	res = current->gid;                 /* gid  */
	regs.d1 = current->egid;            /* egid */

	return res;
}

int svr3_pipe (volatile struct pt_regs regs) {
	int res;
	int fd[2];

	set_fs (KERNEL_DS);
	res = sys_pipe(fd);
	set_fs (USER_DS);

	res = fd[0];
	regs.d1 = fd[1];

	return res;
}

int svr3_stime (long newtime) {
	int res;
	int t = newtime;

	set_fs (KERNEL_DS);
	res = sys_stime(&t);
	set_fs (USER_DS);

	return res;
}


int svr3_setpgrp (int cmd) {

	switch (cmd) {

	    case 0:
		/*  svr3`s  getpgrp call   */
		return  current->pgrp;
		break;

	    case 1:
		/*  svr3`s  setpgrp call,  the best way to emulate this
		    is using of setsid(2).
		*/
		return sys_setsid();
		break;

	    default:
		return -EINVAL;
		break;
	}

	return -EINVAL;
}


/*  Uncompatibility `flags' bitfields with svr3.  */

static int flags_svr3_to_linux (int flags) {
	int right_flags;

	right_flags = flags & 0x3;      /*  O_ACCMODE   */
	if (flags & 04)  right_flags |= O_NDELAY;
	if (flags & 010)  right_flags |= O_APPEND;
	if (flags & 020)  right_flags |= O_SYNC;
	if (flags & 040)  right_flags |= 040;
	if (flags & 0100)  right_flags |= O_NONBLOCK;
	if (flags & 0200)  right_flags |= 0200;
	if (flags & 0400)  right_flags |= O_CREAT;
	if (flags & 01000)  right_flags |= O_TRUNC;
	if (flags & 02000)  right_flags |= O_EXCL;
	if (flags & 04000)  right_flags |= O_NOCTTY;

	return right_flags;
}

static int flags_linux_to_svr3 (int right_flags) {
	int flags;

	flags = right_flags & 0x3;
	if (right_flags & O_CREAT)  flags |= 0400;
	if (right_flags & O_TRUNC)  flags |= 01000;
	if (right_flags & O_EXCL)  flags |= 02000;
	if (right_flags & O_NOCTTY)  flags |= 04000;
	if (right_flags & O_APPEND)  flags |= 010;
	if (right_flags & O_APPEND)  flags |= 010;
	if (right_flags & O_NDELAY)  flags |= 04;
	/*  O_NONBLOCK is not equal to O_NDELAY under svr3.
	    Currently this feature is unimplemented, because
	    under svr3 it undocumented (uses only with streams ???)
	    and hear we form a info return value (not control argument).
	*/
	if (right_flags & O_SYNC)  flags |= 020;

	return flags;
}

int svr3_open (const char * filename, int flags, int mode) {
	int right_flags, retval;

	right_flags = flags_svr3_to_linux (flags);

	retval = sys_open (filename, right_flags, mode);

	return retval;
}


int svr3_fcntl (unsigned int fd, unsigned int cmd, unsigned long arg) {
	int retval;
	struct svr3_flock {
	    short l_type;
	    short l_whence;
	    long  l_start;
	    long  l_len;
	    long  l_pid;
	    short l_sysid;
	} flock;

	switch (cmd) {

	    case 8:  case 10:  case 11:
		/*  unimplemented   */
		return -ENOSYS;
		break;

	    case F_SETFL:
		arg = flags_svr3_to_linux (arg);

		retval = sys_fcntl (fd, cmd, arg);
		break;

	    case F_GETFL:
		retval = sys_fcntl (fd, cmd, arg);
		if (retval >= 0)
			retval = flags_linux_to_svr3 (retval);
		break;

	    case F_GETLK:
		retval = verify_area (VERIFY_WRITE, (void *) arg,
					    sizeof (struct svr3_flock));
		if (retval)  return retval;
		memcpy_fromfs (&flock, (void *) arg,
					    sizeof (struct svr3_flock));

		if (flock.l_type == 1)  flock.l_type = F_RDLCK;
		else if (flock.l_type == 2)  flock.l_type = F_WRLCK;
		else if (flock.l_type == 3)  flock.l_type = F_UNLCK;
		else if (flock.l_type == 0)  flock.l_type = 3;

		flock.l_sysid = 0;      /*  Non-actual hear.  */

		set_fs (KERNEL_DS);
		retval = sys_fcntl (fd, cmd, (unsigned int) &flock);
		set_fs (USER_DS);

		if (retval < 0)  return retval;

		if (flock.l_type == F_RDLCK)  flock.l_type = 1;
		else if (flock.l_type == F_WRLCK)  flock.l_type = 2;
		else if (flock.l_type == F_UNLCK)  flock.l_type = 3;
		else if (flock.l_type == 3)  flock.l_type = 0;

		flock.l_sysid = 0;      /*  No RFS supported   */

		memcpy_tofs ((void *) arg, &flock, sizeof (struct svr3_flock));
		break;

	    case F_SETLK:  case F_SETLKW:
		retval = verify_area (VERIFY_READ, (void *) arg,
					    sizeof (struct svr3_flock));
		if (retval)  return retval;
		memcpy_fromfs (&flock, (void *) arg,
					    sizeof (struct svr3_flock));

		if (flock.l_type == 1)  flock.l_type = F_RDLCK;
		else if (flock.l_type == 2)  flock.l_type = F_WRLCK;
		else if (flock.l_type == 3)  flock.l_type = F_UNLCK;
		else if (flock.l_type == 0)  flock.l_type = 3;

		flock.l_sysid = 0;      /*  Non-actual hear.  */

		set_fs (KERNEL_DS);
		retval = sys_fcntl (fd, cmd, (unsigned int) &flock);
		set_fs (USER_DS);

		break;

	    default:
		retval = sys_fcntl (fd, cmd, arg);
		break;
	}

	return retval;
}


/*    IPC  syscalls       */

#ifdef CONFIG_SYSVIPC

struct svr3_ipc_perm {
	ushort  uid;
	ushort  gid;
	ushort  cuid;
	ushort  cgid;
	ushort  mode;
	ushort  seq;
	key_t   key;
};

struct svr3_msqid_ds {
	struct svr3_ipc_perm msg_perm;
	struct msg      *msg_first;
	struct msg      *msg_last;
	ushort          msg_cbytes;
	ushort          msg_qnum;
	ushort          msg_qbytes;
	ushort          msg_lspid;
	ushort          msg_lrpid;
	time_t          msg_stime;
	time_t          msg_rtime;
	time_t          msg_ctime;
};

struct svr3_semid_ds {
	struct svr3_ipc_perm sem_perm;
	struct sem      *sem_base;
	ushort          sem_nsems;
	time_t          sem_otime;
	time_t          sem_ctime;
};

struct svr3_shmid_ds {
	struct svr3_ipc_perm shm_perm;
	int             shm_segsz;
	void            *shm_reg;
	char            pad[4];
	ushort          shm_lpid;
	ushort          shm_cpid;
	ushort          shm_nattch;
	ushort          shm_cnattch;
	time_t          shm_atime;
	time_t          shm_dtime;
	time_t          shm_ctime;
};


int svr3_msgsys (int call, int one, int two, int three, int four, int five) {
	int err;

	switch (call) {

	    case 0:     /*  MSGGET   */
		return sys_msgget ((key_t) one, two);
		break;

	    case 1:     /*  MSGCTL   */
		switch (two) {
		    struct msqid_ds tmp;
		    struct svr3_msqid_ds buf;

		    case 0:     /*  IPC_RMID   */
			return  sys_msgctl (one, IPC_RMID, NULL);
			break;

		    case 1:     /*  IPC_SET   */
			err = verify_area (VERIFY_READ, (void *) three,
						sizeof (struct svr3_msqid_ds));
			if (err)  return err;

			memcpy_fromfs (&buf, (void *) three, sizeof (buf));

			tmp.msg_perm.uid = buf.msg_perm.uid;
			tmp.msg_perm.gid = buf.msg_perm.gid;
			tmp.msg_perm.cuid = buf.msg_perm.cuid;
			tmp.msg_perm.cgid = buf.msg_perm.cgid;
			tmp.msg_perm.mode = buf.msg_perm.mode;
			tmp.msg_perm.seq = buf.msg_perm.seq;
			tmp.msg_perm.key = buf.msg_perm.key;

			/*  Currently Linux use only msg_qbytes and
			   msg_perm in this context, ignore pointers.  */
			tmp.msg_cbytes = buf.msg_cbytes;
			tmp.msg_qnum = buf.msg_qnum;
			tmp.msg_qbytes = buf.msg_qbytes;
			tmp.msg_lspid = buf.msg_lspid;
			tmp.msg_lrpid = buf.msg_lrpid;
			tmp.msg_stime = buf.msg_stime;
			tmp.msg_rtime = buf.msg_rtime;
			tmp.msg_ctime = buf.msg_ctime;

			set_fs (KERNEL_DS);
			err = sys_msgctl (one, IPC_SET, &tmp);
			set_fs (USER_DS);

			return err;
			break;

		    case 2:     /*  IPC_STAT   */
			err = verify_area (VERIFY_WRITE, (void *) three,
						sizeof (struct svr3_msqid_ds));
			if (err)  return err;

			set_fs (KERNEL_DS);
			err = sys_msgctl (one, IPC_STAT, &tmp);
			set_fs (USER_DS);
			if (err < 0) return err;

			buf.msg_perm.uid = tmp.msg_perm.uid;
			buf.msg_perm.gid = tmp.msg_perm.gid;
			buf.msg_perm.cuid = tmp.msg_perm.cuid;
			buf.msg_perm.cgid = tmp.msg_perm.cgid;
			buf.msg_perm.mode = tmp.msg_perm.mode;
			buf.msg_perm.seq = tmp.msg_perm.seq;
			buf.msg_perm.key = tmp.msg_perm.key;

			/*  Another struct msg under svr3 !!!   */
			/*  Only addresses are useful (may be). */
			buf.msg_first = tmp.msg_first;
			buf.msg_last = tmp.msg_last;

			buf.msg_cbytes = tmp.msg_cbytes;
			buf.msg_qnum = tmp.msg_qnum;
			buf.msg_qbytes = tmp.msg_qbytes;
			buf.msg_lspid = tmp.msg_lspid;
			buf.msg_lrpid = tmp.msg_lrpid;
			buf.msg_stime = tmp.msg_stime;
			buf.msg_rtime = tmp.msg_rtime;
			buf.msg_ctime = tmp.msg_ctime;

			memcpy_tofs ((void *) three, &buf, sizeof (buf));

			return err;
			break;

		    default:
			return -EINVAL;
			break;
		}
		break;

	    case 2:     /*  MSGRCV   */
		return sys_msgrcv (one, (struct msgbuf *) two, three,
								four, five);
		break;

	    case 3:     /*  MSGSND   */
		return sys_msgsnd (one, (struct msgbuf *) two, three, four);
		break;

	    default:
		return -EINVAL;
		break;
	}

	return 0;
}


int svr3_semsys (int call, int one, int two, int three, int four) {
	int err;

	switch (call) {

	    case 0:     /*  SEMCTL   */
		switch (three) {
		    struct semid_ds tmp;
		    struct svr3_semid_ds buf;

		    case 0:     /*  IPC_RMID   */
			three = IPC_RMID;
			break;

		    case 1:     /*  IPC_SET   */
			err = verify_area (VERIFY_READ, (void *) four,
						sizeof (struct svr3_semid_ds));
			if (err)  return err;

			memcpy_fromfs (&buf, (void *) four, sizeof (buf));

			tmp.sem_perm.uid = buf.sem_perm.uid;
			tmp.sem_perm.gid = buf.sem_perm.gid;
			tmp.sem_perm.cuid = buf.sem_perm.cuid;
			tmp.sem_perm.cgid = buf.sem_perm.cgid;
			tmp.sem_perm.mode = buf.sem_perm.mode;
			tmp.sem_perm.seq = buf.sem_perm.seq;
			tmp.sem_perm.key = buf.sem_perm.key;

			/*  Currently Linux use only sem_perm
			   in this context, ignore others.    */

			set_fs (KERNEL_DS);
			err = sys_semctl (one, two, IPC_SET,
						(union semun) &tmp);
			set_fs (USER_DS);

			return err;
			break;

		    case 2:     /*  IPC_STAT   */
			err = verify_area (VERIFY_WRITE, (void *) four,
						sizeof (struct svr3_semid_ds));
			if (err)  return err;

			set_fs (KERNEL_DS);
			err = sys_semctl (one, two, IPC_STAT,
						(union semun) &tmp);
			set_fs (USER_DS);
			if (err < 0) return err;

			buf.sem_perm.uid = tmp.sem_perm.uid;
			buf.sem_perm.gid = tmp.sem_perm.gid;
			buf.sem_perm.cuid = tmp.sem_perm.cuid;
			buf.sem_perm.cgid = tmp.sem_perm.cgid;
			buf.sem_perm.mode = tmp.sem_perm.mode;
			buf.sem_perm.seq = tmp.sem_perm.seq;
			buf.sem_perm.key = tmp.sem_perm.key;

			/*  Another struct fields under svr3 !!!   */
			/*  Only addresses are useful (may be). */
			buf.sem_base = tmp.sem_base;

			buf.sem_nsems = tmp.sem_nsems;
			buf.sem_otime = tmp.sem_otime;
			buf.sem_ctime = tmp.sem_ctime;

			memcpy_tofs ((void *) four, &buf, sizeof (buf));

			return err;
			break;

		    case 3:
			three = GETNCNT;
			break;
		    case 4:
			three = GETPID;
			break;
		    case 5:
			three = GETVAL;
			break;
		    case 6:
			three = GETALL;
			break;
		    case 7:
			three = GETZCNT;
			break;
		    case 8:
			three = SETVAL;
			break;
		    case 9:
			three = SETALL;
			break;

		    default:
			return -EINVAL;
			break;
		}

		return sys_semctl (one, two, three, (union semun) four);
		break;

	    case 1:     /*  SEMGET   */
		return sys_semget ((key_t) one, two, three);
		break;

	    case 2:     /*  SEMOP   */
		return sys_semop (one, (struct sembuf *) two, three);
		break;

	    default:
		return -EINVAL;
		break;
	}

	return 0;
}


int svr3_shmsys (int call, int one, int two, int three) {
	unsigned long raddr;   /*  To return shmat value.  */
	int err;
	struct shmid_ds tmp;

	switch (call) {

	    case 0:     /*  SHMAT   */

		if (two == 0) {
		    /*  addr == 0 : svr3 gives 0x80000000, 0x80040000, etc.  */
		    struct vm_area_struct *vmm;
		    unsigned long addr, len;

		    /*  get segment size   */
		    set_fs (KERNEL_DS);
		    err = sys_shmctl (one, IPC_STAT, &tmp);
		    set_fs (USER_DS);
		    if (err < 0) return err;

		    len = tmp.shm_segsz;

		    /*  Get unmapped area in svr3 stile:
		       starts at 0x80000000 (stack boundary),
		       increments by 0x40000 (`region' size, 1<<18).
		    */
#if 1
		    addr = SHM_RANGE_START;
#else
		    addr = 0x80000000;
#endif

		    for (vmm = find_vma (current->mm, addr); ;
							vmm = vmm->vm_next) {
			addr = (addr + 0x3ffff) & ~0x3ffff;

			/* At this point:  (!vmm || addr < vmm->vm_end). */
			if (TASK_SIZE - len < addr)  return -ENOMEM;

			if (!vmm || addr + len <= vmm->vm_start)  break;

			addr = vmm->vm_end;
		    }

		    two = addr;
		}

		err = sys_shmat (one, (char *) two, three, &raddr);
		if (err < 0)  return err;

		return raddr;

		break;

	    case 1:     /*  SHMCTL   */
		switch (two) {
		    struct svr3_shmid_ds buf;

		    case 0:     /*  IPC_RMID   */
			return  sys_shmctl (one, IPC_RMID, NULL);
			break;

		    case 1:     /*  IPC_SET   */
			err = verify_area (VERIFY_READ, (void *) three,
						sizeof (struct svr3_shmid_ds));
			if (err)  return err;

			memcpy_fromfs (&buf, (void *) three, sizeof (buf));

			tmp.shm_perm.uid = buf.shm_perm.uid;
			tmp.shm_perm.gid = buf.shm_perm.gid;
			tmp.shm_perm.cuid = buf.shm_perm.cuid;
			tmp.shm_perm.cgid = buf.shm_perm.cgid;
			tmp.shm_perm.mode = buf.shm_perm.mode;
			tmp.shm_perm.seq = buf.shm_perm.seq;
			tmp.shm_perm.key = buf.shm_perm.key;

			/*  Currently Linux use only
			   shm_perm in this context, ignore pointers.  */
			tmp.shm_segsz = buf.shm_segsz;
			tmp.shm_cpid = buf.shm_cpid;
			tmp.shm_lpid = buf.shm_lpid;
			tmp.shm_nattch = buf.shm_nattch;
			tmp.shm_atime = buf.shm_atime;
			tmp.shm_dtime = buf.shm_dtime;
			tmp.shm_ctime = buf.shm_ctime;

			set_fs (KERNEL_DS);
			err = sys_shmctl (one, IPC_SET, &tmp);
			set_fs (USER_DS);

			return err;
			break;

		    case 2:     /*  IPC_STAT   */
			err = verify_area (VERIFY_WRITE, (void *) three,
						sizeof (struct svr3_shmid_ds));
			if (err)  return err;

			set_fs (KERNEL_DS);
			err = sys_shmctl (one, IPC_STAT, &tmp);
			set_fs (USER_DS);
			if (err < 0) return err;

			buf.shm_perm.uid = tmp.shm_perm.uid;
			buf.shm_perm.gid = tmp.shm_perm.gid;
			buf.shm_perm.cuid = tmp.shm_perm.cuid;
			buf.shm_perm.cgid = tmp.shm_perm.cgid;
			buf.shm_perm.mode = tmp.shm_perm.mode;
			buf.shm_perm.seq = tmp.shm_perm.seq;
			buf.shm_perm.key = tmp.shm_perm.key;

			/*  Another struct fields under svr3 !!!   */
			/*  Only addresses are useful (may be). */
			buf.shm_reg = tmp.shm_pages;

			buf.shm_segsz = tmp.shm_segsz;
			buf.shm_cpid = tmp.shm_cpid;
			buf.shm_lpid = tmp.shm_lpid;
			buf.shm_nattch = tmp.shm_nattch;
			buf.shm_atime = tmp.shm_atime;
			buf.shm_dtime = tmp.shm_dtime;
			buf.shm_ctime = tmp.shm_ctime;

			/*  Cannot emulate ???   */
			buf.shm_cnattch = 0;

			memcpy_tofs ((void *) three, &buf, sizeof (buf));

			return err;
			break;

		    case 3:
			return sys_shmctl (one, SHM_LOCK, NULL);
			break;

		    case 4:
			return sys_shmctl (one, SHM_UNLOCK, NULL);
			break;

		    default:
			return -EINVAL;
			break;
		}
		break;

	    case 2:     /*  SHMDT   */
		return sys_shmdt ((char *) one);
		break;

	    case 3:     /*  SHMGET   */
		return sys_shmget ((key_t) one, two, three);
		break;

	    default:
		return -EINVAL;
	    }

	return 0;
}

#endif  /*  CONFIG_SYSVIPC   */


/*   statfs()  stuff    */

struct  svr3_statfs {
	short   f_fstyp;        /* File system type */
	long    f_bsize;        /* Block size */
	long    f_frsize;       /* Fragment size (if supported) */
	long    f_blocks;       /* Total number of blocks on file system */
	long    f_bfree;        /* Total number of free blocks */
	long    f_files;        /* Total number of file nodes (inodes) */
	long    f_ffree;        /* Total number of free file nodes */
	char    f_fname[6];     /* Volume name */
	char    f_fpack[6];     /* Pack name */
};

int svr3_statfs (const char *path, void *buf, int len, int fstyp) {
	struct statfs tmp;
	struct svr3_statfs res;
	struct super_block *sb;
	int err;

	len = (len > sizeof (res)) ? sizeof (res) : len;

	err = verify_area (VERIFY_WRITE, buf, len);
	if (err)  return err;

	set_fs (KERNEL_DS);
	err = sys_statfs (path, &tmp);
	set_fs (USER_DS);
	if (err < 0)  return err;

	/*  svr3`s f_fstyp is an index, but Linux`s f_type is a magic. */
	for (sb = super_blocks; sb < super_blocks + NR_SUPER; sb++)
		if (sb->s_magic == tmp.f_type) break;
	if (sb == super_blocks + NR_SUPER)
	    /*  let it be junk   */
	    res.f_fstyp = tmp.f_type;
	else {
	    set_fs (KERNEL_DS);
	    res.f_fstyp = (short) sys_sysfs (1, sb->s_type->name);
	    set_fs (USER_DS);
	}

	res.f_bsize = tmp.f_bsize;
	res.f_frsize = 0;       /*  Not used on sysv   */
	res.f_blocks = tmp.f_blocks;
	res.f_bfree = tmp.f_bfree;
	res.f_files = tmp.f_files;
	res.f_ffree = tmp.f_ffree;
	/*  Very difficult to emulate.   */
	memcpy (res.f_fname, "*****", 6);
	memcpy (res.f_fpack, "*****", 6);

	memcpy_tofs (buf, &res, len);

	return err;
}

int svr3_fstatfs (int fd, void *buf, int len, int fstyp) {
	struct statfs tmp;
	struct svr3_statfs res;
	struct super_block *sb;
	int err;

	len = (len > sizeof (res)) ? sizeof (res) : len;

	err = verify_area (VERIFY_WRITE, buf, len);
	if (err)  return err;

	set_fs (KERNEL_DS);
	err = sys_fstatfs (fd, &tmp);
	set_fs (USER_DS);
	if (err < 0)  return err;

	/*  svr3`s f_fstyp is an index, but Linux`s f_type is a magic. */
	for (sb = super_blocks; sb < super_blocks + NR_SUPER; sb++)
		if (sb->s_magic == tmp.f_type) break;
	if (sb == super_blocks + NR_SUPER)
	    /*  let it be junk   */
	    res.f_fstyp = tmp.f_type;
	else {
	    set_fs (KERNEL_DS);
	    res.f_fstyp = (short) sys_sysfs (1, sb->s_type->name);
	    set_fs (USER_DS);
	}

	res.f_bsize = tmp.f_bsize;
	res.f_frsize = 0;       /*  Not used on sysv   */
	res.f_blocks = tmp.f_blocks;
	res.f_bfree = tmp.f_bfree;
	res.f_files = tmp.f_files;
	res.f_ffree = tmp.f_ffree;
	/*  Very difficult to emulate.   */
	memcpy (res.f_fname, "*****", 6);
	memcpy (res.f_fpack, "*****", 6);

	memcpy_tofs (buf, &res, len);

	return err;
}


int svr3_mount (const char *spec, const char *dir, int mflag,
				int fstyp, const void *data, int datalen) {
	const char *name;
	char namebuf[256];
	int err;
	unsigned long usp, newusp;

	/*  Cannot support 6-args` syntax.   */
	if (mflag & 0x4)  return -ENOSYS;

	/*  Do not support RFS .   */
	if (mflag & 0x8)  return -ENOSYS;

	/* What name of filesys should be mounted ???  */
	if (mflag & 0x2) {      /*  MS_FSS   */
	    int err;

	    set_fs (KERNEL_DS);
	    err = sys_sysfs (2, fstyp, &namebuf);
	    set_fs (USER_DS);

	    if (err)  return err;

	    name = namebuf;

	} else {    /*  the same type as root filesystems  */
	    struct super_block *sb;

	    for (sb = super_blocks; sb < super_blocks + NR_SUPER; sb++)
		if (sb->s_dev == ROOT_DEV && sb->s_magic != 0)  break;
	    if (sb == super_blocks + NR_SUPER)  return -ENODEV;

	    name = sb->s_type->name;
	}

	/*  because `name' *must* be in the user space (see `sys_mount()')  */
	usp = rdusp();
	newusp = usp - sizeof (namebuf) - 4;
	wrusp ((unsigned long) newusp);
	/*  put filesystem type name into the user stack...  */
	memcpy_tofs ((void *) newusp, name, strlen (name) + 1);

	err = sys_mount (spec, dir, (char *) newusp,
				(mflag & 0x1) ? MS_RDONLY : 0, NULL);

	wrusp ((unsigned long) usp);    /*  restore old stack value   */

	return err;
}


int svr3_utssys (int one, int two, int call) {
	struct svr3_ustat {
	    int            f_tfree;
	    unsigned short f_tinode;
	    char           f_fname[6];
	    char           f_fpack[6];
	} otmp;
	struct ustat tmp;
	int err;

	switch (call) {

	    case 0:     /*  uname   */
		return sys_olduname ((struct oldold_utsname *) one);
		break;

	    case 2:     /*  ustat   */
		err = verify_area (VERIFY_WRITE, (void *) one, sizeof (otmp));
		if (err)  return err;

		set_fs (KERNEL_DS);
		err = sys_ustat (two, &tmp);
		set_fs (USER_DS);
		if (err < 0)  return err;

		otmp.f_tfree = tmp.f_tfree;
		otmp.f_tinode = tmp.f_tinode;
		memcpy (otmp.f_fname, tmp.f_fname, 6);
		memcpy (otmp.f_fpack, tmp.f_fpack, 6);

		memcpy_tofs ((void *) one, &otmp, sizeof (otmp));

		break;

	    default:
		return -EFAULT;     /*  svr3 use `EFAULT' hear.  */
		break;
	}

	return 0;
}

int svr3_uadmin (int cmd, int fcn, int mdep) {

	switch (cmd) {
	    case 1:     /*  A_REBOOT   */
		sys_reboot (0xfee1dead, 672274793, 0x01234567);
		break;
	    case 2:     /*  A_SHUTDOWN   */
		sys_reboot (0xfee1dead, 672274793, 0xCDEF0123);
		break;
	    case 4:     /*  A_REMOUNT   */
		return -ENOSYS;     /*  currently unimplemented   */
		break;
	    default:
		return -EINVAL;
		break;
	}

	return 0;
}



int svr3_sysm68k (int cmd, int one, int two, int three) {
	int err, i;

	cmd -= 68000;
	switch (cmd) {

	    case 1:     /*  S3BSWPI      */
		{   struct svr3_swapint {
			char    si_cmd;
			char    filler;
			char   *si_buf;
			int     si_swplo;
			int     si_nblks;
		    } swapint;
		    struct svr3_swaptab {
			unsigned short st_dev;
			short   st_flags;
			void    *st_ucnt;
			void    *st_next;
			int     st_swplo;
			int     st_npgs;
			int     st_nfpgs;
			int     st_pad[2];
		    } swaptab, *swapptr;
		    int i,j;

		    err = verify_area (VERIFY_READ, (void *) one,
							sizeof (swapint));
		    if (err)  return err;

		    memcpy_fromfs (&swapint, (void *) one, sizeof (swapint));

		    switch (swapint.si_cmd) {

			case 0:     /*  SI_LIST   */
			    swapptr = (struct svr3_swaptab *) swapint.si_buf;

			    for (i = 0; i < nr_swapfiles; i++) {
				if (!(swap_info[i].flags & SWP_USED))
					continue;

				err = verify_area (VERIFY_WRITE, swapptr,
							    sizeof (*swapptr));
				if (err)  return err;

				swaptab.st_dev = swap_info[i].swap_device;
				swaptab.st_flags = 0;
				swaptab.st_ucnt = (void *) 1;   /* mark used */
				swaptab.st_next = (void *) 1;
				swaptab.st_swplo = 0;

				swaptab.st_npgs = 0;
				swaptab.st_nfpgs = 0;
				for (j = 0; j < swap_info[i].max; j++)
				    switch (swap_info[i].swap_map[j]) {
					case 128:  continue;  break;
					case 0:  swaptab.st_nfpgs++;
					default: swaptab.st_npgs++;
						 break;
				    }

				/*  svr3 page size is 2048 bytes...  */
				swaptab.st_nfpgs *= (PAGE_SIZE / 2048);
				swaptab.st_npgs *= (PAGE_SIZE / 2048);

				memcpy_tofs (swapptr, &swaptab,
							sizeof (swaptab));
				swapptr++;
			    }

			    err = verify_area (VERIFY_WRITE, swapptr,
							sizeof (*swapptr));
			    if (err)  return err;

			    memset (&swaptab, 0, sizeof (swaptab));
			    memcpy_tofs (swapptr, &swaptab, sizeof (swaptab));

			    return 0;
			    break;

			case 1:     /*  SI_ADD   */
			    if (swapint.si_swplo)  return -ENOSYS;

			    return sys_swapon (swapint.si_buf, 0);
			    break;

			case 2:     /*  SI_DEL   */
			    if (swapint.si_swplo)  return -ENOSYS;

			    return sys_swapoff (swapint.si_buf);
			    break;

			default:
			    return -EINVAL;
			    break;
		    }
		}
		break;

	    case 2:     /*  Support of hardware generated signals (may be). */
		/*  No more needed under Linux (???). */
		return one;
		break;

	    case 3:     /*  Get the extended double value of exception
			    float-point operand.  */
		{   unsigned char fstate[216];

		    err = verify_area (VERIFY_WRITE, (void *) one, 12);
		    if (err)  return err;

		    if (boot_info.cputype & FPU_68881) {

			__asm volatile ("fsave (%0)"
					:
					: "a" (fstate)
					: "memory"
			);

			if (fstate[1] == 0x18) {
			    memcpy_tofs ((void *) one, fstate + 0x8, 12);
			    return 0;
			} else
			    return -EBUSY;
		    }
		    else if (boot_info.cputype & FPU_68882) {

			__asm volatile ("fsave (%0)"
					:
					: "a" (fstate)
					: "memory"
			);

			if (fstate[1] == 0x38) {
			    memcpy_tofs ((void *) one, fstate + 0x28, 12);
			    return 0;
			} else
			    return -EBUSY;
		    }
		    else
			return -EINVAL;
		}
		break;

	    case 6:     /*  S3BIOP       */
		return -ENOSYS;
		break;

	    case 40:    /*  S3BFPHW      */
		err = verify_area (VERIFY_WRITE, (void *) one, sizeof (int));
		if (err)  return err;

		if (boot_info.cputype &
		    (FPU_68881 | FPU_68882 | FPU_68040 | FPU_68060))
			put_fs_long (1, (int *) one);
		else
			put_fs_long (0, (int *) one);

		break;

	    case 54:    /*  STIME        */
		if (!suser())  return -EPERM;

		cli();
		xtime.tv_sec = one;
		xtime.tv_usec = 0;
		time_status = TIME_ERROR;
		time_maxerror = MAXPHASE;
		time_esterror = MAXPHASE;
		sti();
		return 0;

	    case 56:    /*  SETNAME      */
		if (!suser())  return -EPERM;

		/*  Not very accuracy.   */
		err = verify_area (VERIFY_READ, (void *) one, 9);
		if (err)  return err;

		for (i=0; i < 9; i++)
		    system_utsname.sysname[i] =
		    system_utsname.nodename[i] =
				    get_fs_byte ((char *) (one + i));

		system_utsname.sysname[i] = '\0';
		system_utsname.nodename[i] = '\0';

		return 0;

	    case 64:    /*  S3BKSTR      */
		if (!suser())  return -EPERM;

		err = verify_area (VERIFY_WRITE, (void *) one, three);
		if (err)  return err;

		set_fs (KERNEL_DS);
		err = verify_area (VERIFY_READ, (void *) two, three);
		set_fs (USER_DS);
		if (err)  return err;

		memcpy_tofs ((void *) one, (void *) two, three);

		return 0;
		break;

	    case 65:    /*  S3BMEM       */
		return high_memory;
		break;

	    case 71:    /*  RDUBLK       */
	    case 101:   /*  S3ADD_ALLOC  */
	    case 102:   /*  S3ADD_ACTIV  */
	    case 103:   /*  S3ADD_FREE   */
	    case 104:   /*  S3ADD_LOOK   */
		return -ENOSYS;
		break;

	    default:
		return -EINVAL;
		break;
	}

	return 0;
}


/*   Signals  stuff.     */

/*   svr3            Linux      */

unsigned char svr3_linux_sig[] = {
			0,
/* SIGHUP  1  */        1,
/* SIGINT  2  */        2,
/* SIGQUIT 3  */        3,
/* SIGILL  4  */        4,
/* SIGTRAP 5  */        5,
/* SIGABRT 6  */        6,
/* SIGEMT  7  */        0,
/* SIGFPE  8  */        8,
/* SIGKILL 9  */        9,
/* SIGBUS  10 */        7,
/* SIGSEGV 11 */        11,
/* SIGSYS  12 */        0,
/* SIGPIPE 13 */        13,
/* SIGALRM 14 */        14,
/* SIGTERM 15 */        15,
/* SIGUSR1 16 */        10,
/* SIGUSR2 17 */        12,
/* SIGCHLD 18 */        17,
/* SIGPWR  19 */        30,
/* SIGWINCH 20*/        28,
/* SIGPHONE 21*/        0,
/* SIGPOLL 22 */        29,

/* NOT supported in svr3, but defined  */

/* SIGSTOP 23 */        19,
/* SIGTSTP 24 */        20,
/* SIGCONT 25 */        18,
/* SIGTTIN 26 */        21,
/* SIGTTOU 27 */        22
};

unsigned char linux_svr3_sig[] = {

/*   Linux            svr3      */

			0,
/* SIGHUP     1 */      1,
/* SIGINT     2 */      2,
/* SIGQUIT    3 */      3,
/* SIGILL     4 */      4,
/* SIGTRAP    5 */      5,
/* SIGABRT    6 */      6,
/* SIGBUS     7 */      10,
/* SIGFPE     8 */      8,
/* SIGKILL    9 */      9,
/* SIGUSR1   10 */      16,
/* SIGSEGV   11 */      11,
/* SIGUSR2   12 */      17,
/* SIGPIPE   13 */      13,
/* SIGALRM   14 */      14,
/* SIGTERM   15 */      15,
/* SIGSTKFLT 16 */      0,
/* SIGCHLD   17 */      18,
/* SIGCONT   18 */      25,
/* SIGSTOP   19 */      23,
/* SIGTSTP   20 */      24,
/* SIGTTIN   21 */      26,
/* SIGTTOU   22 */      27,
/* SIGURG    23 */      0,
/* SIGXCPU   24 */      0,
/* SIGXFSZ   25 */      0,
/* SIGVTALRM 26 */      0,
/* SIGPROF   27 */      0,
/* SIGWINCH  28 */      20,
/* SIGPOLL   29 */      22,
/* SIGPWR    30 */      19,
/* SIGUNUSED 31 */      0
};


#ifndef _BLOCKABLE
#define _BLOCKABLE (~(1 << (SIGKILL-1)) | (1 << (SIGSTOP-1)))
#endif

int svr3_ssig (int sig, void (*handler) (int)) {
	int signum = sig & 0xff;
	struct sigaction tmp;
	void (*ohandler) (int);
	int err;
	struct task_struct *p;

	if (signum <= 0 || signum >= 23)  return -EINVAL;

	signum = svr3_linux_sig[signum];
	if (!signum)  return -ENOSYS;

	if (signum == SIGKILL || signum == SIGSTOP)  return -EINVAL;

	switch (sig & ~0xff) {

	    case 0x200:   /* sighold */

		current->blocked |= (1 << (signum-1)) & _BLOCKABLE;

		return 0;
		break;

	    case 0x400:  /* sigrelse */

		current->blocked &= ~((1 << (signum-1)) & _BLOCKABLE);

		return 0;
		break;

	    case 0x800: /* signore */

		current->signal &= ~((1 << (signum-1)) & _BLOCKABLE);
		current->blocked &= ~((1 << (signum-1)) & _BLOCKABLE);

		memset (&tmp, 0, sizeof (tmp));
		tmp.sa_handler = SIG_IGN;

		current->sig->action[signum-1] = tmp;

		return 0;
		break;

	    case 0x1000:  /* sigpause */

		current->blocked &= ~((1 << (signum-1)) & _BLOCKABLE);
		sys_pause();

		return 0;
		break;

	    case 0x100:          /* sigset */

		if (handler != SIG_DFL &&
		    handler != SIG_IGN &&
		    (int) handler != 2
		) {
		    err = verify_area (VERIFY_READ, handler, 1);
		    if (err)  return err;
		}

		if (current->blocked & (1 << (signum-1)))
		    ohandler = (void (*) (int)) 2;  /*  SIG_HOLD   */
		else
		    ohandler = current->sig->action[signum-1].sa_handler;

		if ((int) handler == 2)     /*  SIG_HOLD   */
			current->blocked |= (1 << (signum-1)) & _BLOCKABLE;
		else {
		    memset (&tmp, 0, sizeof (tmp));
		    tmp.sa_handler = handler;

		    current->sig->action[signum-1] = tmp;
		    current->blocked &= ~((1 << (signum-1)) & _BLOCKABLE);
		}

		/*  Should we do check_pending() here?
		   Svr3 real behavior don`t do it (???)
		*/
		break;

	    case 0: /* signal */

		/*  svr3 behavior is to not check handler area...  */

		current->signal &= ~((1 << (signum-1)) & _BLOCKABLE);

		memset (&tmp, 0, sizeof (tmp));
		tmp.sa_handler = handler;
		tmp.sa_flags = SA_ONESHOT | SA_NOMASK;

		ohandler = current->sig->action[signum-1].sa_handler;
		current->sig->action[signum-1] = tmp;

		break;

	    default:                /* error */
		return -EINVAL;
		break;
	}

	/*  If operated signal is SIGCHLD, svr3 checks:
	  "has current task a zombied child?", and send itself
	  SIGCHLD if a zomby is present.
	*/
	if (signum == SIGCHLD) {
	    for (p = current->p_cptr ; p ; p = p->p_osptr)
		if (p->state == TASK_ZOMBIE) {
		    send_sig (SIGCHLD, current, 1);
		    break;
		}
	}

	return  (int) ohandler;
}

int svr3_kill (int pid, int signum) {

	if (signum <= 0 || signum >= 23)  return -EINVAL;

	signum = svr3_linux_sig[signum];
	if (!signum)  return -ENOSYS;

	return sys_kill (pid, signum);
}


int svr3_wait (volatile struct pt_regs regs) {
	int res;
	unsigned long status;

	set_fs (KERNEL_DS);
	res = sys_wait4 (-1, &status, 0, NULL);
	set_fs (USER_DS);

	if ((status & 0xff) == 0x7f) {    /*  stopped   */
	    int sig = (status >> 8) & 0xff;

	    /*  How can we indicate a not implemented signo ???   */
	    if ((sig = linux_svr3_sig[sig]) == 0)  sig = 31;

	    status = (sig << 8) | 0x7f;

	} else if (status & 0x7f) {     /*  signalled   */
	    int sig = status & 0x7f;

	    /*  How can we indicate a not implemented signo ???   */
	    if ((sig = linux_svr3_sig[sig]) == 0)  sig = 31;

	    status = (status & 0x80) | (sig & 0x7f);
	}

	regs.d1 = status;

	return res;
}


/*  Ioctl  stuff.  May be conflicts, if different things use
					    the same magic byte in cmd.  */

int svr3_ioctl (int fd, int cmd, int arg) {
	unsigned char cmd_family = (cmd & 0xff00) >> 8;

	if (cmd_family == 0x54) {   /*  'T'  termios   */

	    switch (cmd) {
		int err;
		struct termio tmpa;
		struct termios tmps;

		case 0x5401:   /* TCGETA   */
		    err = verify_area (VERIFY_WRITE, (void *) arg,
						    sizeof (struct termio));
		    if (err)  return err;

		    set_fs (KERNEL_DS);
		    err = sys_ioctl (fd, TCGETS, &tmps);
		    set_fs (USER_DS);
		    if (err < 0)  return err;

		    tmpa.c_iflag = tmps.c_iflag & 0x1fff;
		    tmpa.c_oflag = tmps.c_oflag & 0xffff;
		    tmpa.c_cflag = tmps.c_cflag & 0x0fff;
		    tmpa.c_lflag = tmps.c_lflag & 0x01ff;
		    tmpa.c_line = tmps.c_line;
		    tmpa.c_cc[0] = tmps.c_cc[VINTR];
		    tmpa.c_cc[1] = tmps.c_cc[VQUIT];
		    tmpa.c_cc[2] = tmps.c_cc[VERASE];
		    tmpa.c_cc[3] = tmps.c_cc[VKILL];
		    tmpa.c_cc[4] = tmps.c_cc[VEOF];
		    tmpa.c_cc[5] = tmps.c_cc[VEOL];
		    tmpa.c_cc[6] = tmps.c_cc[VEOL2];
		    tmpa.c_cc[7] = tmps.c_cc[VSWTC];

		    memcpy_tofs ((void *) arg, &tmpa, sizeof (struct termio));

		    return 0;
		    break;

		case 0x5402:   /* TCSETA   */
		case 0x5403:   /* TCSETAW  */
		case 0x5404:   /* TCSETAF  */
		    err = verify_area (VERIFY_READ, (void *) arg,
						     sizeof (struct termio));
		    if (err)  return err;

		    memcpy_fromfs (&tmpa, (void *) arg,
						 sizeof (struct termio));
		    set_fs (KERNEL_DS);
		    err = sys_ioctl (fd, TCGETS, &tmps);
		    set_fs (USER_DS);
		    if (err < 0)  return err;

		    tmps.c_iflag = tmpa.c_iflag & 0x1fff;
		    tmps.c_oflag = tmpa.c_oflag & 0xffff;
		    tmps.c_cflag = tmpa.c_cflag & 0x0fff;
		    /*   In violation for the POSIX standard,
		       sysv`s  `eol2' and `iuclc' is an `iexten' features.
		       So, we cause `iexten'.  Also it translates `werase',
		      `next', `reprint', etc  to sysv personality too.
		    */
		    tmps.c_lflag = (tmpa.c_lflag & 0x01ff) | IEXTEN;
		    tmps.c_line = tmpa.c_line;
		    tmps.c_cc[VINTR] = tmpa.c_cc[0];
		    tmps.c_cc[VQUIT] = tmpa.c_cc[1];
		    tmps.c_cc[VERASE] = tmpa.c_cc[2];
		    tmps.c_cc[VKILL] = tmpa.c_cc[3];
		    tmps.c_cc[VEOF] = tmpa.c_cc[4];
		    tmps.c_cc[VEOL] = tmpa.c_cc[5];
		    tmps.c_cc[VEOL2] = tmpa.c_cc[6];
		    tmps.c_cc[VSWTC] = tmpa.c_cc[7];

		    if (cmd == 0x5402)  cmd = TCSETS;
		    else if (cmd == 0x5403)  cmd = TCSETSW;
		    else if (cmd == 0x5404)  cmd = TCSETSF;
		    else  return 0;   /*  not reached   */

		    set_fs (KERNEL_DS);
		    err = sys_ioctl (fd, cmd, &tmps);
		    set_fs (USER_DS);

		    return err;
		    break;

		case 0x5405:
		    cmd = TCSBRK;
		    break;
		case 0x5406:
		    cmd = TCXONC;
		    break;
		case 0x5407:
		    cmd = TCFLSH;
		    break;
		case 0x5408:   /* TTYTYPE  */
		case 0x5420:   /* TCDSET   */
		    return -ENOSYS;
		    break;
		default:
		    break;
	    }
	}
	/*  For sockets   */
	else if (cmd_family == 0x66)    /*  'f'   */
	    switch (cmd & 0xff) {
		case 0x7d:  cmd = 0x5452; break; /*  FIOASYNC   */
		case 0x7e:  cmd = 0x5421; break; /*  FIONBIO   */
		case 0x7f:  cmd = 0x541b; break; /*  FIONREAD   */
	    }
	else if (cmd_family == 0x69)    /*  'i'   */
	    switch (cmd & 0xff) {
		case 12:  cmd = 0x8916; break; /*   SIOCSIFADDR   */
		case 13:  cmd = 0x8915; break; /*   SIOCGIFADDR   */
		case 14:  cmd = 0x8918; break; /*   SIOCSIFDSTADDR   */
		case 15:  cmd = 0x8917; break; /*   SIOCGIFDSTADDR   */
		case 16:  cmd = 0x8914; break; /*   SIOCSIFFLAGS   */
		case 17:  cmd = 0x8913; break; /*   SIOCGIFFLAGS   */
		case 18:  cmd = 0x8920; break; /*   SIOCSIFMEM   */
		case 19:  cmd = 0x891f; break; /*   SIOCGIFMEM   */
		case 20:  cmd = 0x8912; break; /*   SIOCGIFCONF   */
		case 21:  cmd = 0x8922; break; /*   SIOCSIFMTU   */
		case 22:  cmd = 0x8921; break; /*   SIOCGIFMTU   */
		case 23:  cmd = 0x8919; break; /*   SIOCGIFBRDADDR   */
		case 24:  cmd = 0x891a; break; /*   SIOCSIFBRDADDR   */
		case 25:  cmd = 0x891b; break; /*   SIOCGIFNETMASK   */
		case 26:  cmd = 0x891c; break; /*   SIOCSIFNETMASK   */
		case 27:  cmd = 0x891d; break; /*   SIOCGIFMETRIC   */
		case 28:  cmd = 0x891e; break; /*   SIOCSIFMETRIC   */
		case 30:  cmd = 0x8952; break; /*   SIOCSARP   */
		case 31:  cmd = 0x8951; break; /*   SIOCGARP   */
		case 32:  cmd = 0x8950; break; /*   SIOCDARP   */
		case 49:  cmd = 0x8931; break; /*   SIOCADDMULTI   */
		case 50:  cmd = 0x8932; break; /*   SIOCDELMULTI   */
	    }
	else if (cmd_family == 0x72)    /*  'r'  */
	    switch (cmd & 0xff) {

		case 10:  cmd = 0x890B; break; /*   SIOCADDRT   */
		case 11:  cmd = 0x890C; break; /*   SIOCDELRT   */
	    }
	else if (cmd_family == 0x73)    /*  's'  */
	    switch (cmd & 0xff) {

		case 8:   cmd = 0x8902; break; /*   SIOCSPGRP   */
		case 9:   cmd = 0x8904; break; /*   SIOCGPGRP   */
		case 7:   cmd = 0x8905; break; /*   SIOCATMARK  */
	    }
	else ;

	return sys_ioctl (fd, cmd, arg);
}


/*   errno  renumbering   */

unsigned char linux_svr3_errno[128] = {

/*  nothing  */     0,
/*  EPERM    */     1,
/*  ENOENT   */     2,
/*  ESRCH    */     3,
/*  EINTR    */     4,
/*  EIO      */     5,
/*  ENXIO    */     6,
/*  E2BIG    */     7,
/*  ENOEXEC  */     8,
/*  EBADF    */     9,
/*  ECHILD   */     10,
/*  EAGAIN   */     11,
/*  ENOMEM   */     12,
/*  EACCES   */     13,
/*  EFAULT   */     14,
/*  ENOTBLK  */     15,
/*  EBUSY    */     16,
/*  EEXIST   */     17,
/*  EXDEV    */     18,
/*  ENODEV   */     19,
/*  ENOTDIR  */     20,
/*  EISDIR   */     21,
/*  EINVAL   */     22,
/*  ENFILE   */     23,
/*  EMFILE   */     24,
/*  ENOTTY   */     25,
/*  ETXTBSY  */     26,
/*  EFBIG    */     27,
/*  ENOSPC   */     28,
/*  ESPIPE   */     29,
/*  EROFS    */     30,
/*  EMLINK   */     31,
/*  EPIPE    */     32,
/*  EDOM     */     33,
/*  ERANGE   */     34,
/*  EDEADLK  */     45,
/*  ENAMETOOLONG */ 78,
/*  ENOLCK   */     46,
/*  ENOSYS   */     0,      /*  89 > 87=limit  */
/*  ENOTEMPTY  */   17,     /*  really  EEXIST , because 93 > 87=limit  */
/*  ELOOP    */     0,      /*  90 > 87=limit  */
/*  EAGAIN   */     11,
/*  ENOMSG   */     35,
/*  EIDRM    */     36,
/*  ECHRNG   */     37,
/*  EL2NSYNC */     38,
/*  EL3HLT   */     39,
/*  EL3RST   */     40,
/*  ELNRNG   */     41,
/*  EUNATCH  */     42,
/*  ENOCSI   */     43,
/*  EL2HLT   */     44,
/*  EBADE    */     50,
/*  EBADR    */     51,
/*  EXFULL   */     52,
/*  ENOANO   */     53,
/*  EBADRQC  */     54,
/*  EBADSLT  */     55,
/*  EDEADLOCK  */   56,
/*  EBFONT   */     57,
/*  ENOSTR   */     60,
/*  ENODATA  */     61,
/*  ETIME    */     62,
/*  ENOSR    */     63,
/*  ENONET   */     64,
/*  ENOPKG   */     65,
/*  EREMOTE  */     66,
/*  ENOLINK  */     67,
/*  EADV     */     68,
/*  ESRMNT   */     69,
/*  ECOMM    */     70,
/*  EPROTO   */     71,
/*  EMULTIHOP  */   74,
/*  EDOTDOT  */     76,
/*  EBADMSG  */     77,
/*  EOVERFLOW  */   79,
/*  ENOTUNIQ */     80,
/*  EBADFD   */     81,
/*  EREMCHG  */     82,
/*  ELIBACC  */     83,
/*  ELIBBAD  */     84,
/*  ELIBSCN  */     85,
/*  ELIBMAX  */     86,
/*  ELIBEXEC */     87,
/*  EILSEQ   */     0,   /* 88  */
/*  ERESTART */     0,   /* 91  */
/*  ESTRPIPE */     0,   /* 92  */
/*  EUSERS   */     0,   /* 94  */
/*  ENOTSOCK */     0,   /* 95  */
/*  EDESTADDRREQ */ 0,   /* 96  */
/*  EMSGSIZE */     0,   /* 97  */
/*  EPROTOTYPE  */  0,   /* 98  */
/*  ENOPROTOOPT */  0,   /* 99  */
/*  EPROTONOSUPPORT */  0,   /* 120  */
/*  ESOCKTNOSUPPORT */  0,   /* 121  */
/*  EOPNOTSUPP  */  0,   /* 122  */
/*  EPFNOSUPPORT */ 0,   /* 123  */
/*  EAFNOSUPPORT */ 0,   /* 124  */
/*  EADDRINUSE  */  0,   /* 125  */
/*  EADDRNOTAVAIL  */   0,   /* 126  */
/*  ENETDOWN */     0,   /* 127  */
/*  ENETUNREACH */  0,   /* 128  */
/*  ENETRESET   */  0,   /* 129  */
/*  ECONNABORTED */ 0,   /* 130  */
/*  ECONNRESET  */  0,   /* 131  */
/*  ENOBUFS  */     0,   /* 132  */
/*  EISCONN  */     0,   /* 133  */
/*  ENOTCONN */     0,   /* 134  */
/*  ESHUTDOWN   */  0,   /* 143  */
/*  ETOOMANYREFS */ 0,   /* 144  */
/*  ETIMEDOUT   */  0,   /* 145  */
/*  ECONNREFUSED */ 0,   /* 146  */
/*  EHOSTDOWN   */  0,   /* 147  */
/*  EHOSTUNREACH */ 0,   /* 148  */
/*  EALREADY */     0,   /* 149  */
/*  EINPROGRESS */  0,   /* 150  */
/*  ESTALE   */     0,   /* 151  */
/*  EUCLEAN  */     0,   /* 135  */
/*  ENOTNAM  */     0,   /* 137  */
/*  ENAVAIL  */     0,   /* 138  */
/*  EISNAM   */     0,   /* 139  */
/*  EREMOTEIO   */  0,   /* 140  */
/*  EDQUOT   */     0,   /* 89  */
		    0,

};

int svr3_sbreak (unsigned long addr) {
	unsigned long res;

	if (addr < current->mm->start_data)
		return -ENOMEM;     /*  sysv behavior   */

	res = sys_brk (addr);
	if (res != addr)  return -ENOMEM;    /*  sysv behavior   */

	return res;
}


/*   We have putted params under the true frame `regs'...  */

int svr3_exece (char *name, char **argv, char **envp, struct pt_regs regs) {
	int error;
	char * filename;

	error = getname(name, &filename);
	if (error)  return error;

	error = do_execve (filename, argv, envp, &regs);

	putname(filename);

	return error;
}

/*  The same without envp -- svr3 separate call.  */
int svr3_exec (char *name, char**argv, struct pt_regs regs) {
	int error;
	char * filename;

	error = getname(name, &filename);
	if (error)  return error;

	error = do_execve (filename, argv, NULL, &regs);

	putname(filename);

	return error;
}


#if 0
int svr3_socketcall (int call, unsigned long *args) {
	int err;
	long tmp[6];
	char *call_name;

	err = verify_area (VERIFY_READ, args, 6 * sizeof (long));
	if (err)  return err;

	memcpy_fromfs (&tmp, args, 6 * sizeof (long));

	err = sys_socketcall (call, args);

	switch (call) {
	    case 1:  call_name = "SYS_SOCKET";  break;
	    case 2:  call_name = "SYS_BIND";  break;
	    case 3:  call_name = "SYS_CONNECT";  break;
	    case 4:  call_name = "SYS_LISTEN";  break;
	    case 5:  call_name = "SYS_ACCEPT";  break;
	    case 14: call_name = "SYS_SETSOCKOPT";  break;
	    case 15: call_name = "SYS_GETSOCKOPT";  break;
	    default: call_name = "";  break;
	}

	printk ("(%s): %d %s: 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx : %d\n",
		current->comm, call, call_name, tmp[0], tmp[1], tmp[2],
						tmp[3], tmp[4], tmp[5], err);

	if (call == 2) {    /*  SYS_BIND   */
	    short buf[32];

	    if (verify_area (VERIFY_READ, (void *) tmp[1], tmp[2]))  return err;

	    memcpy_fromfs (&buf, (void *) tmp[1], tmp[2]);
	    if (buf[0] == 1) {  /* AF_UNIX  */
		((unsigned char *) buf)[tmp[2]] = '\0';
		printk ("  BIND: 0x%x : %s\n", buf[0], (unsigned char *) &buf[1]);
	    } else if (buf[0] == 2) {   /* AF_INET  */
		unsigned char *p = (unsigned char *) &buf[2];
		((unsigned char *) buf)[tmp[2]] = '\0';
		printk ("  BIND: 0x%x : port %d, addr %u:%u:%u:%u\n",
				buf[0], buf[1], p[0], p[1], p[2], p[3]);
	    }

	}
	else if (call == 3) { /*  SYS_CONNECT   */
	    short buf[32];

	    if (verify_area (VERIFY_READ, (void *) tmp[1], tmp[2]))  return err;

	    memcpy_fromfs (&buf, (void *) tmp[1], tmp[2]);
	    if (buf[0] == 1) {  /* AF_UNIX  */
		((unsigned char *) buf)[tmp[2]] = '\0';
		printk ("  CONNECT: 0x%x : %s\n", buf[0], (unsigned char *) &buf[1]);
	    } else if (buf[0] == 2) {   /* AF_INET  */
		unsigned char *p = (unsigned char *) &buf[2];
		((unsigned char *) buf)[tmp[2]] = '\0';
		printk ("  CONNECT: 0x%x : port %d, addr %u:%u:%u:%u\n",
				buf[0], buf[1], p[0], p[1], p[2], p[3]);
	    }

	}
	else if (call == 14) {   /*  SYS_SETSOCKOPT   */
	    char *p;

	    switch (tmp[2]) {
		case  1: p = "SO_DEBUG";  break;
		case  2: p = "SO_REUSEADDR";  break;
		case  3: p = "SO_TYPE";  break;
		case  4: p = "SO_ERROR";  break;
		case  5: p = "SO_DONTROUTE";  break;
		case  6: p = "SO_BROADCAST";  break;
		case  7: p = "SO_SNDBUF";  break;
		case  8: p = "SO_RCVBUF";  break;
		case  9: p = "SO_KEEPALIVE";  break;
		case 10: p = "SO_OOBINLINE";  break;
		case 13: p = "SO_LINGER";  break;
		default: p = "";  break;
	    }

	    if (tmp[2] != 13)
		printk ("   SETSOCKOPT: %d %s : *optval = 0x%x, optlen = %d\n",
			    tmp[2], p, get_fs_long ((long *) tmp[3]), tmp[4]);
	    else
		printk ("   SETSOCKOPT: %d %s : optval->l_onoff = 0x%x, "
			"optval->l_linger = 0x%x, optlen = %d\n",
			tmp[2], p, get_fs_long ((long *) tmp[3]),
			   get_fs_long ((long *) (tmp[3] + 4)), tmp[4]);
	}
	else if (call == 15) {   /*  SYS_GETSOCKOPT   */
	    char *p;

	    switch (tmp[2]) {
		case  1: p = "SO_DEBUG";  break;
		case  2: p = "SO_REUSEADDR";  break;
		case  3: p = "SO_TYPE";  break;
		case  4: p = "SO_ERROR";  break;
		case  5: p = "SO_DONTROUTE";  break;
		case  6: p = "SO_BROADCAST";  break;
		case  7: p = "SO_SNDBUF";  break;
		case  8: p = "SO_RCVBUF";  break;
		case  9: p = "SO_KEEPALIVE";  break;
		case 10: p = "SO_OOBINLINE";  break;
		case 13: p = "SO_LINGER";  break;
		default: p = "";  break;
	    }

	    if (tmp[2] != 13)
		printk ("   GETSOCKOPT: %d %s : *optval = 0x%x, *optlen = %d\n",
			    tmp[2], p, get_fs_long ((long *) tmp[3]),
				       get_fs_long ((long *) tmp[4]));
	    else
		printk ("   GETSOCKOPT: %d %s : optval->l_onoff = 0x%x, "
			"optval->l_linger = 0x%x, *optlen = %d\n",
			tmp[2], p, get_fs_long ((long *) tmp[3]),
			   get_fs_long ((long *) (tmp[3] + 4)),
			   get_fs_long ((long *) tmp[4]));
	}

	return err;
}
#else
int svr3_socketcall (int call, unsigned long *args) {

	return  sys_socketcall (call, args);
}
#endif


/*  Some svr3 programs sometimes read directories directly
    (instead of `getdents(2)' call). And in such cases these programs
    very hope  the file system type is `sysv'.
    So, if any program under the svr3 personality try to *read*
    the directory (instead of getdents call), we emulate for them
    sysv`s  directory contents  (2 bytes per inode number, 14 per name).
*/

int svr3_read (unsigned int fd, char *buf, unsigned int count) {
	int error;
	struct file *file;
	struct inode *inode;
	int stored;

	if (fd >= NR_OPEN ||
		!(file = current->files->fd[fd]) ||
		    !(inode = file->f_inode)
	)  return -EBADF;
	if (!(file->f_mode & 1))  return -EBADF;
	if (!file->f_op || !file->f_op->read)  return -EINVAL;
	if (!count)  return 0;

	error = verify_area(VERIFY_WRITE,buf,count);
	if (error)  return error;

	if (!S_ISDIR(inode->i_mode))    /*  usual way   */
		return file->f_op->read (inode, file, buf, count);

	/*  Emulating sysv dir.   */
	if (!file->f_op->readdir)  return -EINVAL;

	stored = 0;
	while (stored < count) {
	    struct dirent dirent;
	    struct svr3_direct {
		unsigned short d_ino;
		char d_name[14];
	    } sysv_entry;
	    int i, err;
	    unsigned int pos = file->f_pos;

	    set_fs (KERNEL_DS);
	    err = old_readdir (fd, &dirent, 1);
	    set_fs (USER_DS);
	    if (err < 0)  return err;

	    if (err == 0)  return stored;

	    /*  What should we done hear ???   */
	    if (dirent.d_reclen > sizeof (sysv_entry.d_name)) {
		dirent.d_reclen = sizeof (sysv_entry.d_name);
		dirent.d_name[13] = '?';
	    }

	    sysv_entry.d_ino = dirent.d_ino & 0xffff;
	    for (i = 0; i < dirent.d_reclen; i++)
		sysv_entry.d_name[i] = dirent.d_name[i];
	    for ( ; i < sizeof (sysv_entry.d_name); i++)
		sysv_entry.d_name[i] = '\0';

	    if (stored + sizeof (sysv_entry) > count) {
		sys_lseek (fd, pos, 0);     /*  go back  */
		break;
	    }

	    memcpy_tofs (buf, &sysv_entry, sizeof (sysv_entry));

	    buf += sizeof (sysv_entry);
	    stored += sizeof (sysv_entry);
	}

	return stored;
}


/*  `plock(2)' svr3  call stuff.  Brrrr....   */

static __inline int svr3_txtlock (void) {
	struct vm_area_struct *vma;

	vma = find_vma (current->mm, current->mm->start_code);
	if (vma->vm_flags & VM_LOCKED)  return -EINVAL;

	return sys_mlock (current->mm->start_code,
			      current->mm->end_code - current->mm->start_code);
}

static __inline void svr3_txtunlock (void) {

	sys_munlock (current->mm->start_code,
			      current->mm->end_code - current->mm->start_code);
}

static __inline int svr3_datlock (void) {
	struct vm_area_struct *vma;

	vma = find_vma (current->mm, current->mm->start_data);
	if (vma->vm_flags & VM_LOCKED)  return -EINVAL;

	return sys_mlock (current->mm->start_data,
			      current->mm->brk - current->mm->start_data);
}

static __inline void svr3_datunlock (void) {

	sys_munlock (current->mm->start_data,
			      current->mm->brk - current->mm->start_data);
}

static __inline int svr3_stlock (void) {
	struct vm_area_struct *vma;

	vma = find_vma (current->mm, current->tss.usp);
	if (vma->vm_flags & VM_LOCKED)  return -EINVAL;

	return sys_mlock (current->tss.usp,
			      current->tss.usp - current->mm->start_stack);
}

static __inline void svr3_stunlock (void) {

	sys_munlock (current->tss.usp,
			      current->tss.usp - current->mm->start_stack);
}

int svr3_lock (int op) {

	if (!suser())  return -EPERM;

	switch (op) {

	    case 0:     /*  UNLOCK   */
		svr3_txtunlock();
		svr3_datunlock();
		svr3_stunlock();
		break;

	    case 1:     /*  PROCLOCK   */
		if (svr3_txtlock())  return -EINVAL;
		if (svr3_datlock()) {
			svr3_txtunlock();
			return -EINVAL;
		}
		if (svr3_stlock()) {
			svr3_txtunlock();
			svr3_datunlock();
			return -EINVAL;
		}
		break;

	    case 2:     /*  TXTLOCK   */
		if (svr3_txtlock())  return -EINVAL;
		break;

	    case 4:     /*  DATLOCK   */
		if (svr3_datlock())  return -EINVAL;
		if (svr3_stlock()) {
			svr3_datunlock();
			return -EINVAL;
		}
		break;

	    default:
		return -EINVAL;
		break;
	}

	return 0;
}


int svr3_ulimit (int cmd, int arg) {
	struct rlimit rlim;
	int retval;

	switch (cmd) {

	    case 1:
		set_fs (KERNEL_DS);
		retval = sys_getrlimit (RLIMIT_FSIZE, &rlim);
		set_fs (USER_DS);
		if (retval < 0)  return retval;

		return rlim.rlim_cur >> 9;      /*  512-byte`s blocks  */
		break;

	    case 2:
		rlim.rlim_cur =
		    rlim.rlim_max = arg << 9;   /*  bytes   */
		set_fs (KERNEL_DS);
		retval = sys_setrlimit (RLIMIT_FSIZE, &rlim);
		set_fs (USER_DS);

		return retval;
		break;

	    case 3:
		set_fs (KERNEL_DS);
		retval = sys_getrlimit (RLIMIT_DATA, &rlim);
		set_fs (USER_DS);
		if (retval < 0)  return retval;

		return  current->mm->end_code + rlim.rlim_cur;
		break;

	    case 4:
		set_fs (KERNEL_DS);
		retval = sys_getrlimit (RLIMIT_NOFILE, &rlim);
		set_fs (USER_DS);
		if (retval < 0)  return retval;

		return  rlim.rlim_cur;
		break;

	    default:
		return -EINVAL;
		break;
	}

	return 0;
}


/*   sysv streams  stuff   */
int (*stream_getmsg_func) (struct file *, void *, void *, int *) = NULL;
int (*stream_putmsg_func) (struct file *, void *, void *, int) = NULL;

int svr3_getmsg (int fd, void *two, void *three, int *flags) {
	struct file *filp;

	if (fd >= NR_OPEN || !(filp = current->files->fd[fd]))
		return -EBADF;
	if (!stream_getmsg_func)  return -ENOPKG;

	return stream_getmsg_func (filp, two, three, flags);
}

int svr3_putmsg (int fd, void *two, void *three, int flags) {
	struct file *filp;

	if (fd >= NR_OPEN || !(filp = current->files->fd[fd]))
		return -EBADF;
	if (!stream_putmsg_func)  return -ENOPKG;

	return stream_putmsg_func (filp, two, three, flags);
}

/*  transparent `poll <--> select' implementation, not only for streams...  */
struct pollfd {
	int fd;                         /* file desc to poll */
	short events;                   /* events of interest on fd */
	short revents;                  /* events that occurred on fd */
};

int svr3_poll (struct pollfd *pfd_user, int nfds, int timeout) {
	int err, i;
	struct timeval timeval, *tmv;
	fd_set rfds, wfds, efds;
	struct pollfd *pfd;
	int pfdlen = nfds * sizeof (struct pollfd);
	int n;

	if (nfds < 0 || nfds > NR_OPEN)  return -EINVAL;

	/*  read & write    */
	err = verify_area (VERIFY_WRITE, pfd_user, pfdlen);
	if (err)  return err;

	pfd = (struct pollfd *)  kmalloc (pfdlen, GFP_KERNEL);
	if (!pfd)  return -ENOMEM;

	memcpy_fromfs (pfd, pfd_user, pfdlen);

	FD_ZERO (&rfds);
	FD_ZERO (&wfds);
	FD_ZERO (&efds);

	n = 0;
	for (i = 0; i < nfds; i++) {
	    if (pfd[i].fd < 0)  continue;

	    if (pfd[i].fd > n)  n = pfd[i].fd;

	    if (pfd[i].events & 0x1)  FD_SET (pfd[i].fd, &rfds);  /* POLLIN  */
	    if (pfd[i].events & 0x2)  FD_SET (pfd[i].fd, &efds);  /* POLLPRI */
	    if (pfd[i].events & 0x4)  FD_SET (pfd[i].fd, &wfds);  /* POLLOUT */
	}
	n += 1;     /*  max fd in sets   */

	if (timeout < 0)  tmv = NULL;
	else {
	    timeval.tv_sec = timeout / 1000;
	    timeval.tv_usec = (timeout % 1000) * 1000;
	    tmv = &timeval;
	}

	set_fs (KERNEL_DS);
	err = sys_select (n, &rfds, &wfds, &efds, tmv);
	set_fs (USER_DS);
	if (err < 0)  { kfree (pfd);  return err; }

	for (i = 0; i < nfds; i++) {
	    pfd[i].revents = 0;

	    if (pfd[i].fd < 0)  continue;

	    if (FD_ISSET (pfd[i].fd, &rfds))  pfd[i].revents |= 0x1;
	    if (FD_ISSET (pfd[i].fd, &efds))  pfd[i].revents |= 0x2;
	    if (FD_ISSET (pfd[i].fd, &wfds))  pfd[i].revents |= 0x4;
	}

	memcpy_tofs (pfd_user, pfd, pfdlen);

	kfree (pfd);

	return err;
}
