/*
 * besta/svr3.h -- Header for `besta/svr3.c' and may be someone else.
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

/* The stuff for svr3 m68k a.out COFF format.
  Non-actual things are stripped.                       */

/*          F I L E H D R           */

struct filehdr {
	unsigned short  f_magic;        /* magic number */
	unsigned short  f_nscns;        /* number of sections */
	long            f_timdat;       /* time & date stamp */
	long            f_symptr;       /* file pointer to symtab */
	long            f_nsyms;        /* number of symtab entries */
	unsigned short  f_opthdr;       /* sizeof(optional hdr) */
	unsigned short  f_flags;        /* flags */
	};


/*
 *   Bits for f_flags:
 *
 *      F_RELFLG        relocation info stripped from file
 *      F_EXEC          file is executable  (i.e. no unresolved
 *                              externel references)
 *      F_LNNO          line nunbers stripped from file
 *      F_LSYMS         local symbols stripped from file
 *      F_MINMAL        this is a minimal object file (".m") output of fextract
 *      F_UPDATE        this is a fully bound update file, output of ogen
 *      F_SWABD         this file has had its bytes swabbed (in names)
 *      F_AR16WR        this file has the byte ordering of an AR16WR (e.g. 11/70) machine
 *                              (it was created there, or was produced by conv)
 *      F_AR32WR        this file has the byte ordering of an AR32WR machine(e.g. vax)
 *      F_AR32W         this file has the byte ordering of an AR32W machine (e.g. 3b,maxi)
 *      F_PATCH         file contains "patch" list in optional header
 *      F_NODF          (minimal file only) no decision functions for
 *                              replaced functions
 */

#define  F_RELFLG       0000001
#define  F_EXEC         0000002
#define  F_LNNO         0000004
#define  F_LSYMS        0000010
#define  F_MINMAL       0000020
#define  F_UPDATE       0000040
#define  F_SWABD        0000100
#define  F_AR16WR       0000200
#define  F_AR32WR       0000400
#define  F_AR32W        0001000
#define  F_PATCH        0002000
#define  F_NODF         0002000

	/* Motorola 68000/68008/68010/68020 */
#define MC68NONBCSMAGIC 0520    /* AT&T UNIX System V/68 3.2 */
#define MC68BCSMAGIC    0554    /* AT&T UNIX System V/68 3.2 BCS */
#define MC68MAGICV4     03146   /* AT&T UNIX System V/68 4.0 */

#if defined(SVR4)               /* is system V release 4.0 */
#define MC68MAGIC       MC68MAGICV4
#else
#if defined(BCS)                /* is BCS */
#define MC68MAGIC       MC68BCSMAGIC
#else
#define MC68MAGIC       MC68NONBCSMAGIC
#endif
#endif
#define MC68KWRMAGIC    0520    /* writeable text segments */
#define MC68TVMAGIC     0521
#define MC68KROMAGIC    0521    /* readonly shareable text segments */
#define MC68KPGMAGIC    0522    /* demand paged text segments */
#define M68MAGIC        0210
#define M68TVMAGIC      0211

#define FILHDR  struct filehdr
#define FILHSZ  sizeof(FILHDR)


/*          A O U T H D R           */

typedef struct aouthdr {
	short   magic;          /* see magic.h                          */
	short   vstamp;         /* version stamp                        */
	long    tsize;          /* text size in bytes, padded to FW
				   bdry                                 */
	long    dsize;          /* initialized data "  "                */
	long    bsize;          /* uninitialized data "   "             */
	long    entry;          /* entry pt.                            */
	long    text_start;     /* base of text used for this file      */
	long    data_start;     /* base of data used for this file      */
} AOUTHDR;

#define AOUTSZ (sizeof(AOUTHDR))

#define JMAGIC     0407    /* dirty text and data image, can't share  */
#define DMAGIC     0410    /* dirty text segment, data aligned        */
#define ZMAGIC     0413    /* The proper magic number for executables  */
#define SHMAGIC    0443    /* shared library header                   */


/*          S C N H D R             */

struct scnhdr {
	char            s_name[8];      /* section name */
	long            s_paddr;        /* physical address, aliased s_nlib */
	long            s_vaddr;        /* virtual address */
	long            s_size;         /* section size */
	long            s_scnptr;       /* file ptr to raw data for section */
	long            s_relptr;       /* file ptr to relocation */
	long            s_lnnoptr;      /* file ptr to line numbers */
	unsigned short  s_nreloc;       /* number of relocation entries */
	unsigned short  s_nlnno;        /* number of line number entries */
	long            s_flags;        /* flags */
	};

/* the number of shared libraries in a .lib section in an absolute output file
 * is put in the s_paddr field of the .lib section header, the following define
 * allows it to be referenced as s_nlib
 */

#define s_nlib  s_paddr
#define SCNHDR  struct scnhdr
#define SCNHSZ  sizeof(SCNHDR)

/*
 * The low 2 bytes of s_flags is used as a section "type"
 */

#define STYP_REG        0x00            /* "regular" section:
						allocated, relocated, loaded */
#define STYP_DSECT      0x01            /* "dummy" section:
						not allocated, relocated,
						not loaded */
#define STYP_NOLOAD     0x02            /* "noload" section:
						allocated, relocated,
						 not loaded */
#define STYP_GROUP      0x04            /* "grouped" section:
						formed of input sections */
#define STYP_PAD        0x08            /* "padding" section:
						not allocated, not relocated,
						 loaded */
#define STYP_COPY       0x10            /* "copy" section:
						for decision function used
						by field update;  not
						allocated, not relocated,
						loaded;  reloc & lineno
						entries processed normally */
#define STYP_INFO       0x200           /* comment section : not allocated
						not relocated, not loaded */
#define STYP_LIB        0x800           /* for .lib section : same as INFO */
#define STYP_OVER       0x400           /* overlay section : relocated
						not allocated or loaded */
#define STYP_TEXT       0x20            /* section contains text only */
#define STYP_DATA       0x40            /* section contains data only */
#define STYP_BSS        0x80            /* section contains bss only */


/*          S H L I B H D R         */

/*
 * Shared libraries have the following section header in the data field for
 * each library.
 */

struct slib {
  long          sl_entsz;    /* Size of this entry               */
  long          sl_pathndx;  /* size of the header field         */
};

#define SLIBHD          struct slib
#define SLIBSZ          sizeof(SLIBHD)

#define SVR3_STACK_TOP  0x80000000
