/*
 * besta/scsi_sense.c -- SCSI `get sense', `mode sense' and `mode select'
 *			 routines for Bestas (both HCPU30 and CWN and...).
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

#include <linux/blkdev.h>
#include <linux/locks.h>
#include <linux/config.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/genhd.h>
#include <linux/delay.h>
#include <linux/config.h>
#include <linux/mm.h>
#include <linux/malloc.h>

#include <linux/interrupt.h>

#include <asm/setup.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/segment.h>

#include "besta.h"
#include "scsi.h"


/*   Print GET SENSE info  stuff   */

/*  This code extracted from drivers/scsi/constants.c  */

#define D 0x001  /* DIRECT ACCESS DEVICE (disk) */
#define T 0x002  /* SEQUENTIAL ACCESS DEVICE (tape) */
#define L 0x004  /* PRINTER DEVICE */
#define P 0x008  /* PROCESSOR DEVICE */
#define W 0x010  /* WRITE ONCE READ MULTIPLE DEVICE */
#define R 0x020  /* READ ONLY (CD-ROM) DEVICE */
#define S 0x040  /* SCANNER DEVICE */
#define O 0x080  /* OPTICAL MEMORY DEVICE */
#define M 0x100  /* MEDIA CHANGER DEVICE */
#define C 0x200  /* COMMUNICATION DEVICE */

struct error_info {
    unsigned char code1, code2;
    unsigned short int devices;
    const char * text;
};

struct error_info2 {
    unsigned char code1, code2_min, code2_max;
    unsigned short int devices;
    const char * text;
};

static struct error_info2 additional2[] = {
  {0x40,0x00,0x7f,D,"Ram failure"},
  {0x40,0x80,0xff,D|T|L|P|W|R|S|O|M|C,"Diagnostic failure on component"},
  {0x41,0x00,0xff,D,"Data path failure"},
  {0x42,0x00,0xff,D,"Power-on or self-test failure"},
  {0, 0, 0, 0, NULL}
};

static struct error_info additional[] = {
  {0x00,0x01,T,"Filemark detected"},
  {0x00,0x02,T|S,"End-of-partition/medium detected"},
  {0x00,0x03,T,"Setmark detected"},
  {0x00,0x04,T|S,"Beginning-of-partition/medium detected"},
  {0x00,0x05,T|S,"End-of-data detected"},
  {0x00,0x06,D|T|L|P|W|R|S|O|M|C,"I/O process terminated"},
  {0x00,0x11,R,"Audio play operation in progress"},
  {0x00,0x12,R,"Audio play operation paused"},
  {0x00,0x13,R,"Audio play operation successfully completed"},
  {0x00,0x14,R,"Audio play operation stopped due to error"},
  {0x00,0x15,R,"No current audio status to return"},
  {0x01,0x00,D|W|O,"No index/sector signal"},
  {0x02,0x00,D|W|R|O|M,"No seek complete"},
  {0x03,0x00,D|T|L|W|S|O,"Peripheral device write fault"},
  {0x03,0x01,T,"No write current"},
  {0x03,0x02,T,"Excessive write errors"},
  {0x04,0x00,D|T|L|P|W|R|S|O|M|C,
     "Logical unit not ready, cause not reportable"},
  {0x04,0x01,D|T|L|P|W|R|S|O|M|C,
     "Logical unit is in process of becoming ready"},
  {0x04,0x02,D|T|L|P|W|R|S|O|M|C,
     "Logical unit not ready, initializing command required"},
  {0x04,0x03,D|T|L|P|W|R|S|O|M|C,
     "Logical unit not ready, manual intervention required"},
  {0x04,0x04,D|T|L|O,"Logical unit not ready, format in progress"},
  {0x05,0x00,D|T|L|W|R|S|O|M|C,"Logical unit does not respond to selection"},
  {0x06,0x00,D|W|R|O|M,"No reference position found"},
  {0x07,0x00,D|T|L|W|R|S|O|M,"Multiple peripheral devices selected"},
  {0x08,0x00,D|T|L|W|R|S|O|M|C,"Logical unit communication failure"},
  {0x08,0x01,D|T|L|W|R|S|O|M|C,"Logical unit communication time-out"},
  {0x08,0x02,D|T|L|W|R|S|O|M|C,"Logical unit communication parity error"},
  {0x09,0x00,D|T|W|R|O,"Track following error"},
  {0x09,0x01,W|R|O,"Tracking servo failure"},
  {0x09,0x02,W|R|O,"Focus servo failure"},
  {0x09,0x03,W|R|O,"Spindle servo failure"},
  {0x0A,0x00,D|T|L|P|W|R|S|O|M|C,"Error log overflow"},
  {0x0C,0x00,T|S,"Write error"},
  {0x0C,0x01,D|W|O,"Write error recovered with auto reallocation"},
  {0x0C,0x02,D|W|O,"Write error - auto reallocation failed"},
  {0x10,0x00,D|W|O,"Id crc or ecc error"},
  {0x11,0x00,D|T|W|R|S|O,"Unrecovered read error"},
  {0x11,0x01,D|T|W|S|O,"Read retries exhausted"},
  {0x11,0x02,D|T|W|S|O,"Error too long to correct"},
  {0x11,0x03,D|T|W|S|O,"Multiple read errors"},
  {0x11,0x04,D|W|O,"Unrecovered read error - auto reallocate failed"},
  {0x11,0x05,W|R|O,"L-ec uncorrectable error"},
  {0x11,0x06,W|R|O,"Circ unrecovered error"},
  {0x11,0x07,W|O,"Data resynchronization error"},
  {0x11,0x08,T,"Incomplete block read"},
  {0x11,0x09,T,"No gap found"},
  {0x11,0x0A,D|T|O,"Miscorrected error"},
  {0x11,0x0B,D|W|O,"Unrecovered read error - recommend reassignment"},
  {0x11,0x0C,D|W|O,"Unrecovered read error - recommend rewrite the data"},
  {0x12,0x00,D|W|O,"Address mark not found for id field"},
  {0x13,0x00,D|W|O,"Address mark not found for data field"},
  {0x14,0x00,D|T|L|W|R|S|O,"Recorded entity not found"},
  {0x14,0x01,D|T|W|R|O,"Record not found"},
  {0x14,0x02,T,"Filemark or setmark not found"},
  {0x14,0x03,T,"End-of-data not found"},
  {0x14,0x04,T,"Block sequence error"},
  {0x15,0x00,D|T|L|W|R|S|O|M,"Random positioning error"},
  {0x15,0x01,D|T|L|W|R|S|O|M,"Mechanical positioning error"},
  {0x15,0x02,D|T|W|R|O,"Positioning error detected by read of medium"},
  {0x16,0x00,D|W|O,"Data synchronization mark error"},
  {0x17,0x00,D|T|W|R|S|O,"Recovered data with no error correction applied"},
  {0x17,0x01,D|T|W|R|S|O,"Recovered data with retries"},
  {0x17,0x02,D|T|W|R|O,"Recovered data with positive head offset"},
  {0x17,0x03,D|T|W|R|O,"Recovered data with negative head offset"},
  {0x17,0x04,W|R|O,"Recovered data with retries and/or circ applied"},
  {0x17,0x05,D|W|R|O,"Recovered data using previous sector id"},
  {0x17,0x06,D|W|O,"Recovered data without ecc - data auto-reallocated"},
  {0x17,0x07,D|W|O,"Recovered data without ecc - recommend reassignment"},
  {0x18,0x00,D|T|W|R|O,"Recovered data with error correction applied"},
  {0x18,0x01,D|W|R|O,"Recovered data with error correction and retries applied"},
  {0x18,0x02,D|W|R|O,"Recovered data - data auto-reallocated"},
  {0x18,0x03,R,"Recovered data with circ"},
  {0x18,0x04,R,"Recovered data with lec"},
  {0x18,0x05,D|W|R|O,"Recovered data - recommend reassignment"},
  {0x19,0x00,D|O,"Defect list error"},
  {0x19,0x01,D|O,"Defect list not available"},
  {0x19,0x02,D|O,"Defect list error in primary list"},
  {0x19,0x03,D|O,"Defect list error in grown list"},
  {0x1A,0x00,D|T|L|P|W|R|S|O|M|C,"Parameter list length error"},
  {0x1B,0x00,D|T|L|P|W|R|S|O|M|C,"Synchronous data transfer error"},
  {0x1C,0x00,D|O,"Defect list not found"},
  {0x1C,0x01,D|O,"Primary defect list not found"},
  {0x1C,0x02,D|O,"Grown defect list not found"},
  {0x1D,0x00,D|W|O,"Miscompare during verify operation"},
  {0x1E,0x00,D|W|O,"Recovered id with ecc correction"},
  {0x20,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid command operation code"},
  {0x21,0x00,D|T|W|R|O|M,"Logical block address out of range"},
  {0x21,0x01,M,"Invalid element address"},
  {0x22,0x00,D,"Illegal function (should use 20 00, 24 00, or 26 00)"},
  {0x24,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid field in cdb"},
  {0x25,0x00,D|T|L|P|W|R|S|O|M|C,"Logical unit not supported"},
  {0x26,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid field in parameter list"},
  {0x26,0x01,D|T|L|P|W|R|S|O|M|C,"Parameter not supported"},
  {0x26,0x02,D|T|L|P|W|R|S|O|M|C,"Parameter value invalid"},
  {0x26,0x03,D|T|L|P|W|R|S|O|M|C,"Threshold parameters not supported"},
  {0x27,0x00,D|T|W|O,"Write protected"},
  {0x28,0x00,D|T|L|P|W|R|S|O|M|C,"Not ready to ready transition (medium may have changed)"},
  {0x28,0x01,M,"Import or export element accessed"},
  {0x29,0x00,D|T|L|P|W|R|S|O|M|C,"Power on, reset, or bus device reset occurred"},
  {0x2A,0x00,D|T|L|W|R|S|O|M|C,"Parameters changed"},
  {0x2A,0x01,D|T|L|W|R|S|O|M|C,"Mode parameters changed"},
  {0x2A,0x02,D|T|L|W|R|S|O|M|C,"Log parameters changed"},
  {0x2B,0x00,D|T|L|P|W|R|S|O|C,"Copy cannot execute since host cannot disconnect"},
  {0x2C,0x00,D|T|L|P|W|R|S|O|M|C,"Command sequence error"},
  {0x2C,0x01,S,"Too many windows specified"},
  {0x2C,0x02,S,"Invalid combination of windows specified"},
  {0x2D,0x00,T,"Overwrite error on update in place"},
  {0x2F,0x00,D|T|L|P|W|R|S|O|M|C,"Commands cleared by another initiator"},
  {0x30,0x00,D|T|W|R|O|M,"Incompatible medium installed"},
  {0x30,0x01,D|T|W|R|O,"Cannot read medium - unknown format"},
  {0x30,0x02,D|T|W|R|O,"Cannot read medium - incompatible format"},
  {0x30,0x03,D|T,"Cleaning cartridge installed"},
  {0x31,0x00,D|T|W|O,"Medium format corrupted"},
  {0x31,0x01,D|L|O,"Format command failed"},
  {0x32,0x00,D|W|O,"No defect spare location available"},
  {0x32,0x01,D|W|O,"Defect list update failure"},
  {0x33,0x00,T,"Tape length error"},
  {0x36,0x00,L,"Ribbon, ink, or toner failure"},
  {0x37,0x00,D|T|L|W|R|S|O|M|C,"Rounded parameter"},
  {0x39,0x00,D|T|L|W|R|S|O|M|C,"Saving parameters not supported"},
  {0x3A,0x00,D|T|L|W|R|S|O|M,"Medium not present"},
  {0x3B,0x00,T|L,"Sequential positioning error"},
  {0x3B,0x01,T,"Tape position error at beginning-of-medium"},
  {0x3B,0x02,T,"Tape position error at end-of-medium"},
  {0x3B,0x03,L,"Tape or electronic vertical forms unit not ready"},
  {0x3B,0x04,L,"Slew failure"},
  {0x3B,0x05,L,"Paper jam"},
  {0x3B,0x06,L,"Failed to sense top-of-form"},
  {0x3B,0x07,L,"Failed to sense bottom-of-form"},
  {0x3B,0x08,T,"Reposition error"},
  {0x3B,0x09,S,"Read past end of medium"},
  {0x3B,0x0A,S,"Read past beginning of medium"},
  {0x3B,0x0B,S,"Position past end of medium"},
  {0x3B,0x0C,S,"Position past beginning of medium"},
  {0x3B,0x0D,M,"Medium destination element full"},
  {0x3B,0x0E,M,"Medium source element empty"},
  {0x3D,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid bits in identify message"},
  {0x3E,0x00,D|T|L|P|W|R|S|O|M|C,"Logical unit has not self-configured yet"},
  {0x3F,0x00,D|T|L|P|W|R|S|O|M|C,"Target operating conditions have changed"},
  {0x3F,0x01,D|T|L|P|W|R|S|O|M|C,"Microcode has been changed"},
  {0x3F,0x02,D|T|L|P|W|R|S|O|M|C,"Changed operating definition"},
  {0x3F,0x03,D|T|L|P|W|R|S|O|M|C,"Inquiry data has changed"},
  {0x43,0x00,D|T|L|P|W|R|S|O|M|C,"Message error"},
  {0x44,0x00,D|T|L|P|W|R|S|O|M|C,"Internal target failure"},
  {0x45,0x00,D|T|L|P|W|R|S|O|M|C,"Select or reselect failure"},
  {0x46,0x00,D|T|L|P|W|R|S|O|M|C,"Unsuccessful soft reset"},
  {0x47,0x00,D|T|L|P|W|R|S|O|M|C,"Scsi parity error"},
  {0x48,0x00,D|T|L|P|W|R|S|O|M|C,"Initiator detected error message received"},
  {0x49,0x00,D|T|L|P|W|R|S|O|M|C,"Invalid message error"},
  {0x4A,0x00,D|T|L|P|W|R|S|O|M|C,"Command phase error"},
  {0x4B,0x00,D|T|L|P|W|R|S|O|M|C,"Data phase error"},
  {0x4C,0x00,D|T|L|P|W|R|S|O|M|C,"Logical unit failed self-configuration"},
  {0x4E,0x00,D|T|L|P|W|R|S|O|M|C,"Overlapped commands attempted"},
  {0x50,0x00,T,"Write append error"},
  {0x50,0x01,T,"Write append position error"},
  {0x50,0x02,T,"Position error related to timing"},
  {0x51,0x00,T|O,"Erase failure"},
  {0x52,0x00,T,"Cartridge fault"},
  {0x53,0x00,D|T|L|W|R|S|O|M,"Media load or eject failed"},
  {0x53,0x01,T,"Unload tape failure"},
  {0x53,0x02,D|T|W|R|O|M,"Medium removal prevented"},
  {0x54,0x00,P,"Scsi to host system interface failure"},
  {0x55,0x00,P,"System resource failure"},
  {0x57,0x00,R,"Unable to recover table-of-contents"},
  {0x58,0x00,O,"Generation does not exist"},
  {0x59,0x00,O,"Updated block read"},
  {0x5A,0x00,D|T|L|P|W|R|S|O|M,"Operator request or state change input (unspecified)"},
  {0x5A,0x01,D|T|W|R|O|M,"Operator medium removal request"},
  {0x5A,0x02,D|T|W|O,"Operator selected write protect"},
  {0x5A,0x03,D|T|W|O,"Operator selected write permit"},
  {0x5B,0x00,D|T|L|P|W|R|S|O|M,"Log exception"},
  {0x5B,0x01,D|T|L|P|W|R|S|O|M,"Threshold condition met"},
  {0x5B,0x02,D|T|L|P|W|R|S|O|M,"Log counter at maximum"},
  {0x5B,0x03,D|T|L|P|W|R|S|O|M,"Log list codes exhausted"},
  {0x5C,0x00,D|O,"Rpl status change"},
  {0x5C,0x01,D|O,"Spindles synchronized"},
  {0x5C,0x02,D|O,"Spindles not synchronized"},
  {0x60,0x00,S,"Lamp failure"},
  {0x61,0x00,S,"Video acquisition error"},
  {0x61,0x01,S,"Unable to acquire video"},
  {0x61,0x02,S,"Out of focus"},
  {0x62,0x00,S,"Scan head positioning error"},
  {0x63,0x00,R,"End of user area encountered on this track"},
  {0x64,0x00,R,"Illegal mode for this track"},
  {0, 0, 0, NULL}
};

static const char *snstext[] = {
    "None",                     /* There is no sense information */
    "Recovered Error",          /* The last command completed successfully
				   but used error correction */
    "Not Ready",                /* The addressed target is not ready */
    "Medium Error",             /* Data error detected on the medium */
    "Hardware Error",           /* Controller or device failure */
    "Illegal Request",
    "Unit Attention",           /* Removable medium was changed, or
				   the target has been reset */
    "Data Protect",             /* Access to the data is blocked */
    "Blank Check",              /* Reached unexpected written or unwritten
				   region of the medium */
    "Key=9",                    /* Vendor specific */
    "Copy Aborted",             /* COPY or COMPARE was aborted */
    "Aborted Command",          /* The target aborted the command */
    "Equal",                    /* A SEARCH DATA command found data equal */
    "Volume Overflow",          /* Medium full with still data to be written */
    "Miscompare",               /* Source data and data on the medium
				   do not agree */
    "Key=15"                    /* Reserved */
};

void scsi_print_sense (const char *name, unsigned char *sense_buffer) {
	int i, sense_class, valid, code;

	sense_class = (sense_buffer[0] >> 4) & 0x07;
	code = sense_buffer[0] & 0xf;
	valid = sense_buffer[0] & 0x80;

	printk ("%s: ", name);

	if (sense_class == 7) {     /* extended sense data */

	    if (code == 0x0)
		/* error concerns current command */
		printk ("(current) ");
	    else if (code == 0x1)
		/* error concerns some earlier command,
		  e.g., an earlier write to disk cache
		  succeeded, but now the disk discovers
		  that it cannot write the data */
		printk ("(deferred) ");
	    else {
		printk ("bad extension sense code 0x%x\n", code);
		return;
	    }

	    if (sense_buffer[2] & 0x80)
		printk ("FMK ");    /* current command has read a filemark */
	    else if (sense_buffer[2] & 0x40)
		printk ("EOM ");    /* end-of-medium condition exists */
	    else if (sense_buffer[2] & 0x20)
		printk ("ILI ");    /* incorrect block length requested */

	    printk ("%s ", snstext[sense_buffer[2] & 0xf]);

	    /* Check to see if additional sense information is available */
	    if (sense_buffer[7] + 7 < 13 ||
	       (sense_buffer[12] == 0  && sense_buffer[13] ==  0)) goto done;

	    for (i=0; additional[i].text; i++)
		if (additional[i].code1 == sense_buffer[12] &&
		    additional[i].code2 == sense_buffer[13]
		)  printk ("(%s) ", additional[i].text);

	    for (i=0; additional2[i].text; i++)
		if (additional2[i].code1 == sense_buffer[12] &&
		    additional2[i].code2_min >= sense_buffer[13] &&
		    additional2[i].code2_max <= sense_buffer[13]
		)  printk ("(%s 0x%x)", additional2[i].text, sense_buffer[13]);

	} else {    /* non-extended sense data */

	     /*
	      * Standard says:
	      *    sense_buffer[0] & 0200 : address valid
	      *    sense_buffer[0] & 0177 : vendor-specific error code
	      *    sense_buffer[1] & 0340 : vendor-specific
	      *    sense_buffer[1..3] : 21-bit logical block address
	      */

	    printk ("(non-extended sense class 0x%0x) ", code);

	    if (sense_buffer[0] < 15)
		printk ("%s ", snstext[sense_buffer[0] & 0xf]);
	    else
		printk("sns = %2x %2x", sense_buffer[0], sense_buffer[2]);
	}

done:
	printk ("\n");

	return;
}


/*   Check MODE SENSE and MODE SELECT if is needable  stuff   */

/*  MODE  SENSE/SELECT  definitions   */

#define PC_CURRENT      0x00
#define PC_CHANGEABLE   0x40
#define PC_DEFAULT      0x80
#define PC_SAVED        0xc0

/*   Page Codes   */
#define NO_PAGES                0x0
#define ALL_PAGES               0x3f

struct sense_hdr {
	unsigned char   length;
	unsigned char   medium_type;
	unsigned char   misc;
	unsigned char   bd_len;
};

struct block_desc {
	unsigned char   density_code;
	unsigned char   blocks[3];
	unsigned char   resv;
	unsigned char   block_len[3];
};

struct page_desc {
	unsigned char   page_code;
	unsigned char   page_len;
};


#define ERR_RECOV_PAGE          0x1
/*  Bits for error recovery parameters (error_bits)  */
#define DCR     0x01            /*  Disable correction   */
#define DTE     0x02            /*  Disable transfer on error   */
#define PER     0x04            /*  Post error report   */
#define EEC     0x08            /*  Enable early correction  */
#define RC      0x10            /*  Read continuous   */
#define TB      0x20            /*  Transfer block   */
#define ARRE    0x40            /*  Automatic read reallocation enabled   */
#define AWRE    0x80            /*  Automatic write reallocation enabled   */

struct error_recovery {
	unsigned char   error_bits;
	unsigned char   retry_cnt;
	unsigned char   correction;
	char            head_offset;
	char            data_strobe_offset;
	unsigned char   resv;
	unsigned short  recovery_time;
};

#define DISC_RECON_PAGE         0x2
struct disc_recon {
	unsigned char   bfr_full_ratio;
	unsigned char   bfr_empty_ratio;
	unsigned short  bus_inactive;
	unsigned short  discon_time;
	unsigned short  connect_time;
	unsigned char   resv[2];
};


#define FORMAT_DEV_PAGE         0x3
/*  Bits for parms field   */
#define SSEC    0x80
#define HSEC    0x40
#define RMB     0x20
#define SURF    0x10
#define INS     0x08

struct dad_form_parms {
	unsigned short  tracks_per_zone;
	unsigned short  alt_sectors_per_zone;
	unsigned short  alt_tracks_per_zone;
	unsigned short  alt_tracks_per_vol;
	unsigned short  sectors_per_track;
	unsigned short  bytes_per_phys_sector;
	unsigned short  interleave;
	unsigned short  track_skew_factor;
	unsigned short  cyl_skew_factor;
	unsigned char   parms;
	unsigned char   resv[3];
};

#define DISK_GEOMETRY_PAGE      0x4
struct geometry_parms {
	unsigned char   num_cyl[3];
	unsigned char   num_heads;
	unsigned char   write_precomp[3];
	unsigned char   write_current[3];
	unsigned short  drive_step_rate;
	unsigned char   land_zone_cyl[3];
	unsigned char   resv[3];
};

#define FLEX_PAGE               0x5
struct flex_geometry_parms {
	unsigned char transfer_rate[2];
	unsigned char heads;
	unsigned char sectors;
	unsigned char sector_size[2];
	unsigned char tracks[2];
	unsigned char write_precomp[2];
	unsigned char reduced_write_current[2];
	unsigned char step_rate[2];
	unsigned char step_pulse_width;
	unsigned char head_settle_delay[2];
	unsigned char motor_on_delay;
	unsigned char motor_off_delay;
	unsigned char special_mode;
	unsigned char steps_per_track;
	unsigned char write_comp;
	unsigned char head_load_delay;
	unsigned char head_unload_delay;
	unsigned char pins_34_and_2;
	unsigned char pins_4_and_1;
	unsigned char rotation_rate[2];
	unsigned char reserved[2];
};


#define CACHE_PAGE              0x8
/*  Bit fields for cache_parms (control_bits)  */
#define RCD     0x01
#define MF      0x02
#define WCE     0x04

struct cache_parms {
	unsigned char   control_bits;
	unsigned char   rd_wr_priority;
	unsigned short  disable_prefetch_len;
	unsigned short  min_prefetch;
	unsigned short  max_prefetch;
	unsigned short  max_prefetch_ceiling;
};

#define COMPRESSION_PAGE        0x0f
struct compression_page {
	unsigned int    dce:1;
	unsigned int    dcc:1;
	unsigned int    byte_2_reserved:6;
	unsigned int    dde:1;
	unsigned int    red:2;
	unsigned int    byte_3_reserved:5;
	unsigned char   compression_algorithm[4];
	unsigned char   decompression_algorithm[4];
	unsigned char   reserved[4];
};

#define DEVICE_CONFIG_PAGE      0x10
struct config_page {
	unsigned int    byte_2_reserved:1;
	unsigned int    cap:1;
	unsigned int    caf:1;
	unsigned int    active_format:5;
	unsigned char   active_partition;
	unsigned char   write_buffer_full_ratio;
	unsigned char   read_buffer_empty_ratio;
	unsigned char   write_delay_time[2];
	unsigned int    dbr:1;
	unsigned int    bis:1;
	unsigned int    RSmk:1;
	unsigned int    avc:1;
	unsigned int    socf:2;
	unsigned int    rbo:1;
	unsigned int    rew:1;
	unsigned char   gap_size;
	unsigned int    eod_defined:3;
	unsigned int    eeg:1;
	unsigned int    sew:1;
	unsigned int    reserved2:3;
	unsigned char   buffer_size_at_EW[3];
	unsigned char   select_dc_algorithm;
	unsigned char   reserved3;
};

#define READ_AHEAD_PAGE         0x38
struct read_ahead_parms {
	unsigned int    rsv:3;
	unsigned int    cache_enable:1;
	unsigned int    cache_table_size:4;
	unsigned char   prefetch_threshold;
	unsigned char   max_prefetch;
};

int scsi_disk_mode (int target, struct scsi_info_struct *scsi_info) {
	char *buf, *pcr, *pch, *pcr_end, *sel_ptr;
	int err, len, sense_blksize;
	char mode_sense[] = scsi_mode_sense;
	char mode_select[] = scsi_mode_select;
	int page_changed = 0;
	struct sense_hdr *sense_hdr;
	struct block_desc *block_desc;
	struct page_desc *page_desc;
	struct dad_form_parms *dad_form = NULL;

	buf = (char *) kmalloc (512, GFP_KERNEL);
	if (!buf)  return 1;

	pcr = buf;              /*  current parameters ptr   */
	pch = buf + 256;        /*  changeable parameters ptr   */

	/*  get current parameters...   */
	mode_sense[2] = PC_CURRENT | ALL_PAGES;
	err = do_cmd_in_init (target, mode_sense, 0, pcr, 255, 0);
	if (err)  goto bad_sense;

	/*  get changeable parameter masks...   */
	mode_sense[2] = PC_CHANGEABLE | ALL_PAGES;
	err = do_cmd_in_init (target, mode_sense, 0, pch, 255, 0);
	if (err)  goto bad_sense;

	/*  checking, improvements...   */
	sense_hdr = (struct sense_hdr *) pcr;
	pcr += sizeof (*sense_hdr);
	pch += sizeof (*sense_hdr);

	len = sense_hdr->length + 1;
	if (len < sizeof (*sense_hdr) + sizeof (*block_desc) ||
	    sense_hdr->medium_type != scsi_info[target].type
	)  goto bad_sense;
	pcr_end = pcr + len;

	block_desc = (struct block_desc *) pcr;
	pcr += sense_hdr->bd_len;       /*  more accuracy   */
	pch += sense_hdr->bd_len;

	sense_blksize = (block_desc->block_len[0] << 16) +
			    (block_desc->block_len[1] << 8) +
				block_desc->block_len[2];

	if (sense_blksize != scsi_info[target].blksize) {
	    printk ("\n              (logical blksize %d differs physycal %d)",
				    sense_blksize, scsi_info[target].blksize);
	    scsi_info[target].blksize = sense_blksize;
	}

	sel_ptr = pcr;  /*  ptr to place where changed pages go to  */

	while (pcr < pcr_end) {
	    struct geometry_parms *geometry;
	    struct cache_parms *cache;
	    struct read_ahead_parms *read_ahead;

	    page_desc = (struct page_desc *) pcr;
	    pcr += sizeof (*page_desc);
	    pch += sizeof (*page_desc);

	    switch (page_desc->page_code & 0x3f) {

		case 0x3:       /*  FORMAT_DEV_PAGE   */
		    if (page_desc->page_len < 10)  break;

		    dad_form = (struct dad_form_parms *) pcr;
		    disk_info(target).sects_per_track =
					dad_form->sectors_per_track;
		    break;

		case 0x4:       /*  DISK_GEOMETRY_PAGE   */
		    if (page_desc->page_len < 4)  break;

		    geometry = (struct geometry_parms *) pcr;
		    disk_info(target).cylinders = (geometry->num_cyl[0] << 16) +
						  (geometry->num_cyl[1] << 8) +
						  geometry->num_cyl[2];
		    disk_info(target).heads = geometry->num_heads;

		    if (!dad_form)  break;

		    /*  compute useful cylinder size   */
		    disk_info(target).sects_per_cylinder =
			geometry->num_heads * dad_form->sectors_per_track -
			(geometry->num_heads / dad_form->tracks_per_zone) *
			(dad_form->alt_sectors_per_zone +
				dad_form->alt_tracks_per_zone *
					dad_form->sectors_per_track);

		    /*  thats all   */
		    break;

		case 0x8:       /*  CACHE_PAGE   */
		    if (page_desc->page_len < 4)  break;

		    cache = (struct cache_parms *) pcr;
		    if (cache->control_bits & RCD) {
			/*  Read cache disabled. We want to enable.  */
			/*  Is it possible ?  */
			if (((struct cache_parms *) pch)->control_bits & RCD) {
			    cache->control_bits &= ~RCD;
			    page_changed = 1;
			}
		    }

		    /*  currently no idea about other...  */
		    break;

		case 0x38:      /*  READ_AHEAD_PAGE   */
		    if (page_desc->page_len < 4)  break;

		    read_ahead = (struct read_ahead_parms *) pcr;
		    if (!read_ahead->cache_enable) {
			/*  Read cache disabled. We want to enable.  */
			/*  Is it possible ?  */
			if (((struct read_ahead_parms *) pch)->cache_enable) {
			    read_ahead->cache_enable = 1;
			    page_changed = 1;
			}
		    }

		    /*  currently no idea about other...  */
		    break;

		default:
		    break;
	    }

	    pcr += page_desc->page_len;
	    pch += page_desc->page_len;

	    if (page_changed) {
		/*  page should go to sel_ptr for mode select later   */
		char *p;

		page_changed = 0;

		page_desc->page_code &= 0x3f;   /*  clear extra bits   */
		p = (char *) page_desc;
		while (p < pcr)  *sel_ptr++ = *p++;
	    }
	}

	/*  sholud we run mode select ?  */
	if (sel_ptr <= buf + (sizeof (*sense_hdr) + sense_hdr->bd_len)) {
		/*  OK,  no page changed   */
		kfree (buf);

		return 0;
	}

	/*  generate mode select cmd   */
	sense_hdr = (struct sense_hdr *) buf;
	sense_hdr->length = 0;      /*  clear   */

	len = sel_ptr - buf;

	mode_select[4] = len;
	err = do_cmd_in_init (target, mode_select, 1, buf, len, 0);
	if (err) {
		printk ("\n             (bad mode select res=%x)", err);
		kfree (buf);
		return 1;
	}

	kfree (buf);
	return 0;

bad_sense:
	printk ("\n                (bad mode sense)");

	return 1;
}
