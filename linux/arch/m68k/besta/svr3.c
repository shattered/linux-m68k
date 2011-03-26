/*
 * besta/svr3.c -- Support for System V SVR3.1 a.out COFF executable format.
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
/*
 *   Support for System V SVR3.1 a.out executable format.
 * Appropriate processes would be under `PER_SVR3' personality.
 * Such executables need `besta/svr3sys.c' kernel support 
 * for sysv-style system calls.
 *
 *   Note: under Linux *demand loading* is used for these executables,
 *	   (System V have more old less efficient once-loading).
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
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

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/pgtable.h>

#include <linux/config.h>

/* Stuff for COFF format. Don`t include linux/coff.h , because it use
   chars in structure field instead of more useful shorts, ints etc. */

#include "svr3.h"

#ifndef SHLIB_MAX
#define SHLIB_MAX       6
#endif

#ifndef PAGE_ROUND
#define PAGE_ROUND(X)   ((X) & (~(PAGE_SIZE - 1)))
#endif

struct bin_info {
	struct file    *file;
	unsigned long   text_addr;
	unsigned long   text_offs;
	unsigned long   text_len;
	unsigned long   data_addr;
	unsigned long   data_offs;
	unsigned long   data_len;
	unsigned long   bss_len;

};

static int load_svr3_binary(struct linux_binprm *, struct pt_regs * regs);
static int load_svr3_library(int fd);
static int svr3_core_dump(long signr, struct pt_regs * regs);

struct linux_binfmt svr3_format = {
	NULL, NULL, load_svr3_binary, load_svr3_library, svr3_core_dump
};

extern off_t sys_lseek (unsigned int, off_t, unsigned int);
extern int do_open (const char * filename,int flags,int mode);


/* create_svr3_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */
static unsigned long *create_svr3_tables (char * p,
					    struct linux_binprm * bprm) {
	unsigned long *argv,*envp;
	unsigned long * sp;
	int argc = bprm->argc;
	int envc = bprm->envc;

	sp = (unsigned long *) ((-(unsigned long)sizeof(char *)) & (unsigned long) p);
	sp -= envc+1;
	envp = sp;
	sp -= argc+1;
	argv = sp;
	put_user(argc,--sp);
	current->mm->arg_start = (unsigned long) p;
	while (argc-->0) {
		put_user(p,argv++);
		while (get_user(p++)) /* nothing */ ;
	}
	put_user(NULL,argv);
	current->mm->arg_end = current->mm->env_start = (unsigned long) p;
	while (envc-->0) {
		put_user(p,envp++);
		while (get_user(p++)) /* nothing */ ;
	}
	put_user(NULL,envp);
	current->mm->env_end = (unsigned long) p;
	return sp;
}


/*  We don`t use ordinary `setup_arg_pages()' because
   svr3 has another stack start address.
*/
static unsigned long svr3_setup_arg_pages (unsigned long p,
						struct linux_binprm * bprm) {
	unsigned long stack_base;
	struct vm_area_struct *mpnt;
	int i;

	stack_base = SVR3_STACK_TOP - MAX_ARG_PAGES*PAGE_SIZE;

	p += stack_base;

	mpnt = (struct vm_area_struct *) kmalloc (sizeof (*mpnt), GFP_KERNEL);
	if (mpnt) {
		mpnt->vm_mm = current->mm;
		mpnt->vm_start = PAGE_MASK & (unsigned long) p;
		mpnt->vm_end = SVR3_STACK_TOP;
		mpnt->vm_page_prot = PAGE_COPY;
		mpnt->vm_flags = VM_STACK_FLAGS;
		mpnt->vm_ops = NULL;
		mpnt->vm_offset = 0;
		mpnt->vm_inode = NULL;
		mpnt->vm_pte = 0;
		insert_vm_struct (current->mm, mpnt);
		current->mm->total_vm =
			(mpnt->vm_end - mpnt->vm_start) >> PAGE_SHIFT;
	}

	for (i = 0 ; i < MAX_ARG_PAGES ; i++) {
		if (bprm->page[i]) {
			current->mm->rss++;
			put_dirty_page(current,bprm->page[i],stack_base);
		}
		stack_base += PAGE_SIZE;
	}
	return p;
}


/*
 *  Cheking a.out binary file and full bin_info structure for this
 *                                                          executable.
 *  fd      opened file descriptor,
 *  buf     static 1024-byte`s array for start block of file
 *                                              (where headers etc.),
 *  binf    pointer to filled by this function `bin_info' structure,
 *  magic   if not zero, a.out header magic to check.
 *
 *  Returned value is 0 if successful, else errno number (< 0).
 */

static int touch_svr3_binary (int fd, char buf[], struct bin_info *binf,
							    int magic) {
	struct file *file = current->files->fd[fd];
	int retval, i;
	struct filehdr *fh;
	struct aouthdr *ah;
	struct scnhdr *sh, *texth, *datah, *bssh;

	if (!file || !file->f_op)  return -EACCES;

	if (file->f_op->lseek) {
	    if (file->f_op->lseek (file->f_inode, file, 0, 0) != 0)
							return -EACCES;
	} else  file->f_pos = 0;

	set_fs (KERNEL_DS);
	retval = file->f_op->read (file->f_inode, file, buf, 1024);
	set_fs (USER_DS);
	if (retval < 0)  return retval;
	if (retval < FILHSZ + AOUTSZ + 3 * SCNHSZ)  return -ENOEXEC;

	fh = (struct filehdr *) buf;
	if (fh->f_magic != MC68MAGIC ||
	    fh->f_opthdr < AOUTSZ ||
	    fh->f_nscns < 3 ||
	    !(fh->f_flags & F_AR32W) ||
	    !(fh->f_flags & F_EXEC)
	)  return -ENOEXEC;

	ah = (struct aouthdr *) (buf + FILHSZ);
	if ((magic && ah->magic != magic) ||
	    !ah->tsize ||
	    ah->tsize + ah->dsize + FILHSZ + fh->f_opthdr +
		 SCNHSZ * fh->f_nscns > file->f_inode->i_size ||
	    ah->text_start + ah->tsize > ah->data_start
	)  return -ENOEXEC;

	sh = (struct scnhdr *) (buf + FILHSZ + fh->f_opthdr);
	texth = datah = bssh = NULL;
	for (i = 0; i < fh->f_nscns; i++) {

	    if (sh[i].s_flags == STYP_TEXT &&
		sh[i].s_size == ah->tsize &&
		sh[i].s_vaddr == ah->text_start
	    )  texth = sh + i;

	    if (sh[i].s_flags == STYP_DATA &&
		sh[i].s_size == ah->dsize &&
		sh[i].s_vaddr == ah->data_start
	    )  datah = sh + i;

	    if (sh[i].s_flags & STYP_BSS  &&
		sh[i].s_size == ah->bsize
	    )  bssh = sh + i;

	}

	if (!texth || !datah || !bssh)  return -ENOEXEC;

	if ((texth->s_scnptr <= datah->s_scnptr &&
	     texth->s_scnptr + texth->s_size > datah->s_scnptr) ||
	    (datah->s_scnptr <= texth->s_scnptr &&
	     datah->s_scnptr + datah->s_size > texth->s_scnptr)
	)  return -ENOEXEC;

	/*  a.out binary is OK .  */

	binf->file = file;

	binf->text_addr = ah->text_start;
	binf->text_offs = texth->s_scnptr;
	binf->text_len = ah->tsize;

	binf->data_addr = ah->data_start;
	binf->data_offs = datah->s_scnptr;
	binf->data_len = ah->dsize;

	binf->bss_len = ah->bsize;

	return 0;
}



static int load_svr3_binary (struct linux_binprm *bprm,
						struct pt_regs *regs) {
	struct file *file;
	int error, retval, i, j, shlibs;
	int fd[1+SHLIB_MAX];
	long entry;
	unsigned long rlim;
	unsigned long p = bprm->p;

	struct filehdr *fh;
	struct aouthdr *ah;
	struct scnhdr *sh;
	char *buf, *libs_buf;

	/*  Main binary + SHLIB_MAX   */
	struct bin_info bin_info[SHLIB_MAX + 1];

/*  Cheking accessable headers by bprm->buf (128 bytes).   */

	fh = (struct filehdr *) bprm->buf;
	if (fh->f_magic != MC68MAGIC ||
	    fh->f_opthdr < AOUTSZ ||
	    fh->f_nscns < 3 ||
	    !(fh->f_flags & F_AR32W) ||
	    !(fh->f_flags & F_EXEC)
	)  return -ENOEXEC;

	ah = (struct aouthdr *) ((char *) bprm->buf + FILHSZ);
	if (ah->magic == SHMAGIC)  return -ELIBEXEC;
	if ((ah->magic != DMAGIC && ah->magic != ZMAGIC) ||
	    !ah->tsize ||
	    ah->tsize + ah->dsize + FILHSZ + fh->f_opthdr +
		 SCNHSZ * fh->f_nscns > bprm->inode->i_size ||
	    ah->text_start + ah->tsize > ah->data_start
	)  return -ENOEXEC;

	if (fh->f_nscns > 24) {
	    printk ("Too many sections in svr3 binary file\n");
	    return -ENOEXEC;
	}


/*      Touch main binary file (which has # 0).  */

	fd[0] = open_inode (bprm->inode, O_RDONLY);
	if (fd[0] < 0)  return fd[0];

	buf = (char *) kmalloc (2*1024, GFP_KERNEL);
	if (!buf)  { sys_close (fd[0]);  return -ENOMEM; }
	libs_buf = buf + 1024;

	retval = touch_svr3_binary (fd[0], buf, &bin_info[0], 0);
	if (retval < 0)  { sys_close(fd[0]); kfree (buf); return retval; }

/*      Looking for STYP_LIB section for shared libraries.  */

	sh = (struct scnhdr *) (buf + FILHSZ + fh->f_opthdr);

	for (i = 0; i < fh->f_nscns; i++)
	    if (sh[i].s_flags == STYP_LIB)  break;

	if (i == fh->f_nscns)  shlibs = 0;
	else  shlibs = sh[i].s_nlib;

/*      Touch target shared library binary files (## 1--SHLIB_MAX).  */

	if (shlibs) {
	    void *p;
	    int slib_size = sh[i].s_size;

	    if (shlibs > SHLIB_MAX)  { retval = -ELIBMAX; goto error_close; }

	    file = bin_info[0].file;

	    retval = sys_lseek (fd[0], sh[i].s_scnptr, 0);
	    if (retval < 0)  goto error_close;
	    if (retval != sh[i].s_scnptr) {
		retval = -EACCES;
		goto error_close;
	    }

	    set_fs (KERNEL_DS);
	    retval = file->f_op->read (file->f_inode, file, libs_buf, 1024);
	    set_fs (USER_DS);
	    if (retval < 0)  goto error_close;
	    if (retval < slib_size) {
		retval = -ELIBSCN;
		goto error_close;
	    }

	    for (p = libs_buf, j = 1; j <= shlibs; j++) {
		int len;
		char *name;
		struct slib *slibh = (struct slib *) p;

		p += slibh->sl_pathndx * 4;
		len = (slibh->sl_entsz - slibh->sl_pathndx) * 4;
		if (len <= 0 || p + len > (void *) libs_buf + slib_size) {
		    retval = -ELIBSCN;
		    goto error_close;
		}

		/* Target shared library path name. Must be
		  followed by one or more zeroes.          */
		name = (char *) p;

		/* Try to access this library.  */

		set_fs (KERNEL_DS);
		fd[j] = sys_open (name, 0, 0);
		set_fs (USER_DS);
		if (fd[j] < 0)  { retval = fd[j]; goto error_close; }

		retval = touch_svr3_binary (fd[j],buf,&bin_info[j],SHMAGIC);
		if (retval < 0)  {
		    /*  Renumbering for shared library context.  */
		    if (retval == -ENOEXEC)  retval = -ELIBBAD;
		    else if (retval == -EACCES)  retval = -ELIBACC;

		    goto error_close;
		}

		p += len;
	    }

	} /*  if (shlibs) ....   */

	/* Check initial limits. This avoids letting people circumvent
	 * size limits imposed on them by creating programs with large
	 * arrays in the data or bss.
	 */
	rlim = current->rlim[RLIMIT_DATA].rlim_cur;
	if (rlim >= RLIM_INFINITY)  rlim = ~0;
	if (ah->dsize + ah->bsize > rlim) {     /*  XXX: but in shlibs too  */
		retval = -ENOMEM;
		goto error_close;
	}

	kfree (buf);

	/*  OK, this is the point of noreturn.  */

	entry = ah->entry & ~0x1;   /* Avoids possibly hult after `rte' ??? */

	flush_old_exec (bprm);

	current->personality = PER_SVR3;

	current->mm->end_code = bin_info[0].text_len +
		(current->mm->start_code = bin_info[0].text_addr);
	current->mm->end_data = bin_info[0].data_len +
		(current->mm->start_data = bin_info[0].data_addr);
	current->mm->brk = bin_info[0].bss_len +
		(current->mm->start_brk = current->mm->end_data);

	current->mm->rss = 0;
	current->mm->mmap = NULL;
	current->suid = current->euid = current->fsuid = bprm->e_uid;
	current->sgid = current->egid = current->fsgid = bprm->e_gid;
	current->flags &= ~PF_FORKNOEXEC;

	/*  mmap all binaries    */

	for (i = 0; i < 1 + shlibs; i++) {
	    struct bin_info *binf = &bin_info[i];
	    unsigned int blocksize = binf->file->f_inode->i_sb->s_blocksize;
	    unsigned int start_bss, end_bss;

	    if (binf->text_addr & (PAGE_SIZE - 1) ||
		binf->data_addr & (PAGE_SIZE - 1) ||
		binf->text_offs & (blocksize - 1) ||
		binf->data_offs & (blocksize - 1) ||
		!binf->file->f_op->mmap
	    ) {
		/*  cannot mmap immediatly   */
		do_mmap (NULL, PAGE_ROUND(binf->text_addr),
			 binf->text_len + (binf->text_addr -
					   PAGE_ROUND(binf->text_addr)),
			 PROT_READ | PROT_WRITE | PROT_EXEC,
			 MAP_FIXED | MAP_PRIVATE, 0);
		read_exec (binf->file->f_inode, binf->text_offs,
			    (char *) binf->text_addr, binf->text_len, 0);

		do_mmap (NULL, PAGE_ROUND(binf->data_addr),
			 binf->data_len + (binf->data_addr -
					   PAGE_ROUND(binf->data_addr)),
			 PROT_READ | PROT_WRITE | PROT_EXEC,
			 MAP_FIXED | MAP_PRIVATE, 0);
		read_exec (binf->file->f_inode, binf->data_offs,
			    (char *) binf->data_addr, binf->data_len, 0);

		/* there's no nice way of flushing a number of
		   user pages to ram 8*( */
		flush_cache_all();

	    } else {

		error = do_mmap (binf->file, binf->text_addr, binf->text_len,
				 PROT_READ | PROT_EXEC,
				 MAP_FIXED | MAP_PRIVATE |
					MAP_DENYWRITE | MAP_EXECUTABLE,
				 binf->text_offs);
		if (error != binf->text_addr)  goto error_kill_close;

		error = do_mmap (binf->file, binf->data_addr, binf->data_len,
				 PROT_READ | PROT_WRITE | PROT_EXEC,
				 MAP_FIXED | MAP_PRIVATE |
					MAP_DENYWRITE | MAP_EXECUTABLE,
				 binf->data_offs);
		if (error != binf->data_addr)  goto error_kill_close;

#ifdef DMAGIC_NODEMAND
		/*  DMAGIC  is for pure executable (not demand loading).
		  But let the shared libraries be demand load ???   */
		if (i == 0 && ah->magic == DMAGIC) {
		    volatile char c;
		    unsigned long addr;

		    /*  Touch all pages in .text and .data segments.  */
		    for (addr = binf->text_addr;
			    addr < binf->text_addr + binf->text_len;
				addr += PAGE_SIZE
		    )  c = get_fs_byte ((char *) addr);
		    for (addr = binf->data_addr;
			    addr < binf->data_addr + binf->data_len;
				addr += PAGE_SIZE
		    )  c = get_fs_byte ((char *) addr);
		}
#endif
	    }

	    sys_close (fd[i]);

	    start_bss = PAGE_ALIGN(binf->data_addr + binf->data_len);
	    end_bss = PAGE_ALIGN(binf->data_addr + binf->data_len +
							binf->bss_len);

	    /*  svr3 binaries very hope that .bss section
	       had been initialized by zeroes. Oh...    */

	    if (binf->bss_len != 0) {
		/*  Because there may be skipped heap by alignment. */
		int addr = binf->data_addr + binf->data_len;
		int i = start_bss - addr;

		/*  start_bss is aligned, addr may be no   */
		while (i & 0x3) {
		    put_fs_byte (0, (char *) addr);
		    addr++; i--;
		}
		i >>= 2;
		while (i--) {
		    put_fs_long (0, (long *) addr);
		    addr += sizeof (long);
		}
	    }

	    if (end_bss >= start_bss)
		    do_mmap (NULL, start_bss, end_bss - start_bss,
			     PROT_READ | PROT_WRITE | PROT_EXEC,
			     MAP_FIXED | MAP_PRIVATE, 0);

#ifdef DMAGIC_NODEMAND
	    /*  The same reason as above.  */
	    if (i == 0 && ah->magic == DMAGIC) {
		volatile char c;
		unsigned long addr;

		for (addr = start_bss; addr < end_bss; addr += PAGE_SIZE)
			c = get_fs_byte ((char *) addr);
	    }
#endif

	    /*  OK, now all is mmapped for binary # i   */
	}  /*  for (i = ... )   */


	if (current->exec_domain && current->exec_domain->use_count)
		(*current->exec_domain->use_count)--;
	if (current->binfmt && current->binfmt->use_count)
		(*current->binfmt->use_count)--;
	current->exec_domain = lookup_exec_domain (current->personality);
	current->binfmt = &svr3_format;
	if (current->exec_domain && current->exec_domain->use_count)
		(*current->exec_domain->use_count)++;
	if (current->binfmt && current->binfmt->use_count)
		(*current->binfmt->use_count)++;


	p = svr3_setup_arg_pages(p, bprm);

	p = (unsigned long) create_svr3_tables((char *)p, bprm);

	current->mm->start_stack = p;

	start_thread(regs, entry, p);

	/*  svr3 clears %d0-%d7/%a0-%a5 at this point,
	  sometimes svr3 programs depend by this ( `time ((void *) gabage)').
	    It is easy to clear registers present in pt_regs struct,
	  d1-d5/a0-a1, but d6-d7/a2-a6 are stored someware unknown...
	  We should use a separate return code in `entry.S' to clear
	  all registers violation svr3 behavior.
	*/
	{   extern void ret_after_svr3_exec (void);
	    extern void *orig_ret_pc;
	    void **ret_pc_addr;

	    ret_pc_addr = (void **) (((unsigned long) regs) - sizeof (void *));
	    orig_ret_pc = *ret_pc_addr;
	    *ret_pc_addr = ret_after_svr3_exec;

	    /*  XXX:  and what about cacheflush hear??? (`040/`060 ???)   */

	    /*  this registers clear immediately...  */
	    regs->d1 = regs->d2 = regs->d3 = regs->d4 = regs->d5 =
						regs->a0 = regs->a1 = 0;
	}

	/*  svr3 don`t understand job controls, but may be SIGSTOP ...  */
	current->sig->action[SIGTSTP-1].sa_flags = SA_NOCLDSTOP;
	current->sig->action[SIGTTIN-1].sa_flags = SA_NOCLDSTOP;
	current->sig->action[SIGTTOU-1].sa_flags = SA_NOCLDSTOP;

	if (current->flags & PF_PTRACED)
		send_sig(SIGTRAP, current, 0);

	return 0;

error_close:
	kfree (buf);
	for (j = 0; j < sizeof(fd)/sizeof(fd[0]); j++)
					sys_close (fd[j]);
	return retval;

error_kill_close:
	kfree (buf);
	for (j = 0; j < sizeof(fd)/sizeof(fd[0]); j++)
					sys_close (fd[j]);
	send_sig (SIGKILL, current, 0);

	return error;
}

static int load_svr3_library (int fd) {

	return -ENOEXEC;    /* Cannot load shared library separately.  */

}

static int svr3_core_dump (long signr, struct pt_regs *regs) {

	/* Currently unimplemented, because sdb is not useful
	   under the Linux kernel (another data structures etc.)
	   May be hear should be used  native Linux`s  aout_core_dump()
	   to use gdb  etc.                             */

#if 1
	struct switch_stack *sw = ((struct switch_stack *) regs) - 1;
	unsigned long usp = rdusp();

	printk ("svr3 process dump (%s)\n sig = %ld\n",
						current->comm, signr);

	printk("Format %02x  Vector: %04x  PC: %08lx  Status: %04x\n",
	       regs->format, regs->vector, regs->pc, regs->sr);
	if (!(regs->sr & PS_S))  printk("USP: %08lx  ", usp);
	printk("ORIG_D0: %08lx\n", regs->orig_d0);
	printk ("D0: %08lx  D1: %08lx  D2: %08lx  D3: %08lx\n",
		regs->d0, regs->d1, regs->d2, regs->d3);
	printk ("D4: %08lx  D5: %08lx  D6: %08lx  D7: %08lx\n",
		regs->d4, regs->d5, sw->d6, sw->d7);
	printk ("A0: %08lx  A1: %08lx  A2: %08lx  A3: %08lx\n",
		regs->a0, regs->a1, sw->a2, sw->a3);
	printk ("A4: %08lx  A5: %08lx  A6: %08lx  A7: %08lx\n",
		sw->a4, sw->a5, sw->a6, usp);

	do {
	    int err;
	    unsigned long retpc, oldfp;

	    err = verify_area (VERIFY_READ, (void *) usp, 4);
	    if (err)  break;
	    retpc = get_user ((int *) usp);
	    printk ("(sp) = 0x%08lx\n", retpc);

	    err = verify_area (VERIFY_READ, (void *) (sw->a6 + 4), 4);
	    if (err)  break;
	    retpc = get_user ((int *) (sw->a6 + 4));
	    printk ("4(fp) = 0x%08lx\n", retpc);

	    err = verify_area (VERIFY_READ, (void *) (sw->a6), 4);
	    if (err)  break;
	    oldfp = get_user ((int *) sw->a6);
	    err = verify_area (VERIFY_READ, (void *) (oldfp + 4), 4);
	    if (err)  break;
	    retpc = get_user ((int *) (oldfp + 4));
	    printk ("4((fp)) = 0x%08lx\n", retpc);

	    err = verify_area (VERIFY_READ, (void *) oldfp, 4);
	    if (err)  break;
	    oldfp = get_user ((int *) oldfp);

	    err = verify_area (VERIFY_READ, (void *) (oldfp + 4), 4);
	    if (err)  break;
	    retpc = get_user ((int *) (oldfp + 4));
	    printk ("4(((fp))) = 0x%08lx\n", retpc);
	} while (0);
#endif

	return 0;
}

