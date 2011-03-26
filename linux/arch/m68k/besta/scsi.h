/*
 * besta/scsi.h -- Common header for SCSI sources.
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

#ifdef __KERNEL__

#include <scsi/scsi.h>

#ifndef TYPE_NONE
#define TYPE_NONE       (-1)
#endif

#define STATE_NOT_READY 0xff
#define STATE_FREE      0
#define STATE_IO        1
#define STATE_CMD       2
#define STATE_REVALIDATE        3


struct scsi_info_struct {
	unsigned type;          /*  type of device   */
	unsigned lun;           /*  LUN in target   */
	unsigned major;         /*  major device   */
	unsigned state;         /*  free, busy, io, ...   */
	unsigned req_sense;     /*  requested sense flag   */
	unsigned blksize;       /*  block size...   */
	int      access_cnt;    /*  access count...   */
	int      cmd_ret;       /*  do_cmd() return value  */
	void    *buffer;        /*  kmalloced buffer for io, misc etc.  */
	char    *name;          /*  name for device   */
	unsigned board;         /*  number for the same type boards   */
	struct wait_queue *wait;
	void (*inthandler) (int, int, struct scsi_info_struct *);
					/*  interrupt handler routine   */

	/*  this four is hardware specific...  */
	int (*do_cmd_wait) (int, int, char [], int, void *, int, int);
				/*  do scsi cmd `wait immediately' method   */
	int (*do_cmd) (int, int, char [], int, void *, int);
				/*  do scsi cmd `sleep_on/wake_up' method   */
	void (*do_request) (int, int, int);
				/*  scsi get/handle request method   */
	void (*end_request) (int, int, int, int);
				/*  scsi end/finish request method   */

	unsigned char sense_buf[16];    /*  sense buffer for reqsense cmd  */
	union {
	    struct scsi_disk_struct {
		struct hd_struct *part;
		unsigned size;
		unsigned sects_per_track;
		unsigned cylinders;
		unsigned heads;
		unsigned sects_per_cylinder;
	    } disk;
	    struct scsi_tape_struct {
		unsigned written;       /*  tape is written   */
		unsigned ended;         /*  tape is vasya   */
		unsigned resid;         /*  not spaced bytes # */
	    } tape;
	} u_scsi;
};

struct scsi_init_struct {
	char *name;
	void (*init) (int, char *, struct scsi_info_struct *);
};
extern struct scsi_init_struct scsi_inits[];
#define FIRST_UNKNOWN_TYPE  10

extern char scsi_targets[];
extern struct scsi_info_struct *scsi_infos[];
extern int num_scsi;

#define disk_info(A)    scsi_info[(A)].u_scsi.disk
#define tape_info(A)    scsi_info[(A)].u_scsi.tape

#define name(TARGET)    (scsi_info[(TARGET)].name)

#define do_cmd_in_init(A,B,C,D,E,F)     \
	(scsi_info[A].do_cmd_wait (scsi_info[A].board,A,B,C,D,E,F))
#define do_scsi_cmd(A,B,C,D,E)          \
	(scsi_info[A].do_cmd (scsi_info[A].board,A,B,C,D,E))

extern int scsi_ioctl (int target, struct scsi_info_struct *scsi_info,
		    struct file *file, unsigned int cmd, unsigned long arg);
extern void scsi_print_sense (const char *name, unsigned char *sense_buf);
extern int scsi_disk_mode (int target, struct scsi_info_struct *scsi_info);

#define TRY_FOR_SENSE   1
#define SOFT_TIMEOUT    2
#define CTL_READ_ERROR  3
#define CTL_WRITE_ERROR 4
#define CTL_PARITY      5
#define CTL_RES_CONFL   6
#define CTL_TIMEOUT     7
#define CTL_ILL_CMD     8
#define CTL_ILL_PARAM   9
#define CTL_BAD_STATUS  10
#define CTL_ERROR       11


#define scsi_test_unit_ready    \
	((char []) {  0x0, 0, 0, 0,  0x0, 0,0,0,0,0,0,0,0,0 } )
#define scsi_inquiry            \
	((char []) { 0x12, 0, 0, 0, 0xff, 0,0,0,0,0,0,0,0,0 } )
#define scsi_read_capacity      \
	((char []) { 0x25, 0, 0, 0,  0x0, 0,0,0,0,0,0,0,0,0 } )
#define scsi_mode_sense         \
	((char []) { 0x1a, 0, 0, 0, 0xff, 0,0,0,0,0,0,0,0,0 } )
#define scsi_mode_select        \
	((char []) { 0x15, 0, 0, 0, 0xff, 0,0,0,0,0,0,0,0,0 } )
#define scsi_request_sense      \
	((char []) {  0x3, 0, 0, 0, 0x10, 0,0,0,0,0,0,0,0,0 } )
#define scsi_start_stop         \
	((char []) { 0x1b, 0, 0, 0,  0x1, 0,0,0,0,0,0,0,0,0 } )

#define scsi_disall_md_removal  \
	((char []) { 0x1e, 0, 0, 0, 1, 0,0,0,0,0,0,0,0,0 } )
#define scsi_all_md_removal     \
	((char []) { 0x1e, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0 } )
#define scsi_rezero_unit        \
	((char []) {  0x1, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0 } )
#define scsi_write_filemark     \
	((char []) { 0x10, 0, 0, 0, 1, 0,0,0,0,0,0,0,0,0 } )
#define scsi_format_unit        \
	((char []) {  0x4, 0, 0, 0, 0, 0,0,0,0,0,0,0,0,0 } )

#endif  /*  __KERNEL__   */


#ifndef SIOCFORMAT
/*  compatibility with __besta__ SVR3.1   */

#define SIOCBD00        (('s'<<8)|1)    /* ioctl: set badblk valid,but none */
#define SIOCBASS        (('s'<<8)|2)    /* ioctl: ass. badtracks        */
#define SIOCFORMAT      (('s'<<8)|3)    /* ioctl: format                */
#define SIOCBDBK        (('s'<<8)|4)    /* ioctl: set badblock illegal  */
#define SIOCSETP        (('s'<<8)|5)    /* ioctl: set testmode          */
#define SIOCGETP        (('s'<<8)|6)    /* ioctl: get volume-infos      */
#define SIOCTRSPMOD     (('s'<<8)|18)   /* ioctl: transparent mode      */
#endif  /*  SIOCFORMAT   */

#ifndef HDIO_GETGEO
/*  compatibility with some Linux`s style...   */
/*  is it better to include linux/hdreg.h immediately???   */

#define HDIO_GETGEO     0x0301          /* get device geometry */

struct hd_geometry {
      unsigned char heads;
      unsigned char sectors;
      unsigned short cylinders;
      unsigned long start;
};

#endif  /*  HDIO_GETGEO   */


/*  for partitions...   */
struct hard_disk_partition {
    char             misc_info[256];
    unsigned int     magic;
    unsigned int     mtime;
    unsigned short   num_parts;
    unsigned short   blksize;
    unsigned int     root_part;
    struct hd_partition {
	unsigned int start_sect;
	unsigned int nr_sects;
    } parts[30];
};

#define HDP_MAGIC       0xbd021268

#define MAX_PARTS       8       /* Max partitions per target  */

