/*
 * besta/stream.h -- Common header for sysv-stream-interfaced sources.
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
 * These values currently are only for binary compatibility with svr3.
 * Should this feature work under Linux ?
 */
#ifndef _SYSV_STREAM_H
#define _SYSV_STREAM_H

#undef STR
#define STR             ('S'<<8)
#define I_NREAD         (STR|01)
#define I_PUSH          (STR|02)
#define I_POP           (STR|03)
#define I_LOOK          (STR|04)
#define I_FLUSH         (STR|05)
#define I_SRDOPT        (STR|06)
#define I_GRDOPT        (STR|07)
#define I_STR           (STR|010)
#define I_SETSIG        (STR|011)
#define I_GETSIG        (STR|012)
#define I_FIND          (STR|013)
#define I_LINK          (STR|014)
#define I_UNLINK        (STR|015)
#define I_PEEK          (STR|017)
#define I_FDINSERT      (STR|020)
#define I_SENDFD        (STR|021)
#define I_RECVFD        (STR|022)

/*  Read options  */
#define RNORM   0       /* read msg norm */
#define RMSGD   1       /* read msg discard */
#define RMSGN   2       /* read msg no discard */

/*  Events for which to be sent SIGPOLL signal  */
#define S_INPUT         001             /* regular priority msg on read Q */
#define S_HIPRI         002             /* high priority msg on read Q */
#define S_OUTPUT        004             /* write Q no longer full */
#define S_MSG           010             /* signal msg at front of read Q */

/*  Flush options   */
#define FLUSHR  1       /* flush read queue */
#define FLUSHW  2       /* flush write queue */
#define FLUSHRW 3       /* flush both queues */

/*  Flags returned as value of recv() syscall   */
#define MORECTL         0x1     /* more ctl info is left in message */
#define MOREDATA        0x2     /* more data is left in message */


/*  User level ioctl format for ioctl that go downstream I_STR   */
struct strioctl {
	int     ic_cmd;                 /* command */
	int     ic_timeout;             /* timeout value */
	int     ic_len;                 /* length of data */
	void   *ic_dp;                  /* pointer to data */
};

/*  Stream buffer structure for send and recv system calls  */
struct strbuf {
	int     maxlen;                 /* no. of bytes in buffer */
	int     len;                    /* no. of bytes returned */
	char    *buf;                   /* pointer to data */
};

/*  stream I_PEEK ioctl format   */
struct strpeek {
	struct strbuf ctlbuf;
	struct strbuf databuf;
	int           flags;
};

/*  stream I_FDINSERT ioctl format   */
struct strfdinsert {
	struct strbuf ctlbuf;
	struct strbuf databuf;
	long          flags;
	int           fildes;
	int           offset;
};

/*  receive file descriptor structure   */
struct strrecvfd {
	int fd;
	unsigned short uid;
	unsigned short gid;
	char fill[8];
};

#define CTL_MAX         PAGE_SIZE

#ifdef __KERNEL__

/*  This pointers are wanted by getmsg(2)/putmsg(2) svr3 syscalls.  */
extern int (*stream_getmsg_func) (struct file *, struct strbuf *,
						struct strbuf *, int *);
extern int (*stream_putmsg_func) (struct file *, struct strbuf *,
						struct strbuf *, int);

struct stream_info {
	struct module_operations *m_ops;
	unsigned  read_opts;
	unsigned  sig_mask;
	unsigned  poll_mask;
	unsigned  min;
	unsigned  max;
	void     *module_data;      /*  private stream module data ptr   */
};

struct module_operations {
	char    *name;
	struct module_operations *next;
	int     (*open) (struct stream_info *stream_info, struct file *file,
				struct module_operations *prev_module);
	void    (*close) (struct stream_info *stream_info, struct file *file);
	int     (*find) (struct stream_info *stream_info, char *name);
	int     (*ioctl) (struct stream_info *stream_info, int cmd,
					void *buf, int *buflen, int timeout);
	int     (*flush) (struct stream_info *stream_info, int rw);
	int     (*nread) (struct stream_info *stream_info, int *nbytes);
	int     (*getmsg) (struct stream_info *stream_info,
				void *buf_ctl, int *len_ctl, void *buf_data,
				int *len_data, int nonblocks, int *flags);
	int     (*peekmsg) (struct stream_info *stream_info,
				void *buf_ctl, int *len_ctl, void *buf_data,
				int *len_data, int *flags);
	int     (*read) (struct stream_info *stream_info, void *buf,
						int count, int nonblocks);
	int     (*putmsg) (struct stream_info *stream_info,
				void *buf_ctl, int len_ctl, void *buf_data,
				int len_data, int nonblocks, int flags);
	int     (*select) (struct stream_info *stream_info, struct file *file,
					    int sel_type, select_table *wait);
	int     (*link) (struct stream_info *stream_info, struct file *file,
								int fd);
	int     (*unlink) (struct stream_info *stream_info, struct file *file,
								int id);
	int     (*sendfd) (struct stream_info *stream_info, struct file *file,
								int fd);
	int     (*recvfd) (struct stream_info *stream_info, struct file *file,
					    int *uid, int *gid, char *fill);
};

extern int reguster_stream_module (struct module_operations *new);
extern int unregister_stream_module (struct module_operations *old);

#endif  /*  __KERNEL__   */

#endif  /*  _SYSV_STREAM_H   */
