/*
 *	Experimental !!!  Not ended !!!
 * besta/cwn_soft.h -- Interface definition for cwn_soft.c/cwn_after.c .
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

struct cwn_control_block {
	unsigned short cntl;
	char           cmd_data[254];
	short          msg_valid;
	char           msg_state;
	char           msg_event;
	unsigned char  msg_aux_stat;
	unsigned char  msg_stat;
	char           msg_reserved[10];
	char           msg_string[112];
	char           reserved[1024 - 256 - 128];
};

/*  values for cntl   */
#define SCSI_INIT       1
#define FLOP_INIT       2
#define FLASHES_INIT    3
#define INTR_INIT       4

/*  XXX:  `scsi_' prefix is not very good, better cwn_....  */

struct scsi_init_block {
	unsigned char   own_id;
	unsigned char   it;     /*  0 -- initiator, 1 -- target   */
	unsigned int    scsi_base;      /*  scsi_cmd chain base   */
	unsigned short  scsi_chain_size;
};


/*  0 -- use separate interrupts for each completed command (if enabled),
    1 -- if previous interrupt is not acknowledged, don`t generate new...
*/
struct intr_init_block {
	unsigned short common_intrs;
};


struct scsi_cmd {
	unsigned short cntl;    /*  controller cmd + state + status   */
	unsigned char status;   /*  SCSI cmd status   */
	unsigned char target;   /*  destination   */
	unsigned int addr;      /*  cwn memory offset for data_in/out...  */
	unsigned short size;    /*  ...bytes reserved in this area   */
	unsigned short timeout; /*  in seconds   */
	unsigned char cmd[12];  /*  SCSI cmd (6,10 or 12 bytes)   */
};

/*  values for cntl   */
#define SC_DO_CMD       0x01    /*  a new cmd in area   */
#define SC_ABORT_CMD    0x02    /*  abort cmd, specified by this area   */
#define SC_RESET        0x03    /*  reset a target, specified by this area   */
#define SC_CMD_MASK     0x07    /*  be careful about future expansion...  */
#define SC_BLOCK_XFER   0x08    /*  transfer per full blocks...  */
#define SC_DISCONNECTED 0x10    /*  target temporary disconnected...  */
#define SC_MAY_DISCONN  0x20    /*  allow to disconnect   */
#define SC_INTR_ON_EOP  0x40    /*  enable `end of operation' interrupt   */
#define SC_OWN_BOARD    0x80    /*  operation in progress now   */

#define SC_MASK         0xff

/*  bits for state   */

#define SS_OWN_USER     0x8000  /*  no more interest this area   */

/*  definitions for returned status...  */
#define RES_BAD_PARAM   1
#define RES_BAD_CMD     2
#define RES_BAD_TARGET  3
#define RES_NOSYS       4
#define RES_BUSY        5
#define RES_ABORTED     6
#define RES_RESET       7
#define RES_BAD_BEHAV   8

#define SS_RES(X)       (((X) >> 8) & 0x7f)


#define SCSI_INTR_CHAN      0
#define FLOPPY_INTR_CHAN    1
#define GLOBAL_INTR_CHAN    2

