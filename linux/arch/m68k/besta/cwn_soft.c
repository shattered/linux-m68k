/*
 *	Experimental !!!  Not ended !!!
 * besta/cwn_soft.c -- Local multithread software for CWN board.
 *		       SCSI only, floppy stuff not released yet.
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

#include "cwn_soft.h"

struct ncr5385e_struct {
	char          r0;
	unsigned char data;     /*  rw, data register   */
	char          r2;
	unsigned char cmd;      /*  rw, command register   */
	char          r4;
	unsigned char cntl;     /*  rw, control register   */
	char          r6;
	unsigned char target;   /*  rw, destination id   */
	char          r8;
	unsigned char aux_stat; /*  r,  auxiliary status   */
	char          ra;
	unsigned char id;       /*  r,  device id   */
	char          rc;
	unsigned char intr;     /*  r,  interrupt register   */
	char          re;
	unsigned char src_id;   /*  r,  source id   */
	char          r10;
	char          r11;
	char          r12;
	unsigned char diagn_stat;/* r,  diagnostic status   */
	char          r14;
	char          r15;
	char          r16;
	char          r17;
	char          r18;
	unsigned char cnt_h;    /*  rw,  transfer counter (msb)  */
	char          r1a;
	unsigned char cnt_m;    /*  rw,  transfer counter (2nd)  */
	char          r1c;
	unsigned char cnt_l;    /*  rw,  transfer counter (lsb)  */
	char          r1e;
	char          r1f;      /*  rw,  reserved for testability   */
};

/*  bits for cmd register   */
#define CMD_CHIP_RESET          0x00    /*  imm dit  */
#define CMD_DISCONNECT          0x01    /*  imm  it  */
#define CMD_PAUSE               0x02    /*  imm d t  */
#define CMD_SET_ATN             0x03    /*  imm  i   */
#define CMD_MESSAGE_ACCEPTED    0x04    /*  imm  i   */
#define CMD_CHIP_DISABLED       0x05    /*  imm dit  */
#define CMD_SELECT_W_ATN        0x08    /*  int d    */
#define CMD_SELECT_WO_ATN       0x09    /*  int d    */
#define CMD_RESELECT            0x0a    /*  int d    */
#define CMD_DIAGN_TURNAROUND    0x0b    /*  int d    */
#define CMD_RCV_CMD             0x0c    /*  int  t   */
#define CMD_RCV_DATA            0x0d    /*  int  t   */
#define CMD_RCV_MSG             0x0e    /*  int  t   */
#define CMD_RCV_UNSPEC_INFO     0x0f    /*  int  t   */
#define CMD_SEND_STATUS         0x10    /*  int  t   */
#define CMD_SEND_DATA           0x11    /*  int  t   */
#define CMD_SEND_MSG            0x12    /*  int  t   */
#define CMD_SEND_UNSPEC_INFO    0x13    /*  int  t   */
#define CMD_TRANSFER_INFO       0x14    /*  int   i  */
#define CMD_TRANSFER_PAD        0x15    /*  int   i  */

#define CMD_SINGLE_BYTE         0x40
#define CMD_DMA_MODE            0x80

/*  bits for cntl register   */
#define CNTL_SELECT_ENA         0x01
#define CNTL_RESELECT_ENA       0x02
#define CNTL_PARITY_ENA         0x04

/*  bits for aux_stat register   */
#define AUX_CNT_ZERO    0x02
#define AUX_PAUSED      0x04
#define AUX_IO          0x08
#define AUX_CD          0x10
#define AUX_MSG         0x20
#define AUX_PARITY_ERR  0x40
#define AUX_DATA_FULL   0x80

#define PHASE(x)        (((x) >> 3) & 0x7)

#define DATA_OUT        PHASE (0)
#define DATA_IN         PHASE (AUX_IO)
#define CMD_OUT         PHASE (AUX_CD)
#define STAT_IN         PHASE (AUX_CD | AUX_IO)
#define MSG_OUT         PHASE (AUX_MSG | AUX_CD)
#define MSG_IN          PHASE (AUX_MSG | AUX_CD | AUX_IO)

/*  bits for intr register   */
#define INTR_FUNC_COMPLETE      0x01
#define INTR_BUS_SERVICE        0x02
#define INTR_DISCONNECTED       0x04
#define INTR_SELECTED           0x08
#define INTR_RESELECTED         0x10
#define INTR_INVAL_CMD          0x40

/*  bits for src_id register   */
#define SRC_ID_VALID            0x80

/*  bits for diagn_stat register   */
#define DIA_SUCCESS             0x00
#define DIA_UNCOND_BRANCH_FAIL  0x01
#define DIA_DATA_FULL_FAIL      0x02
#define DIA_BAD_INI_COND        0x03
#define DIA_BAD_INI_CMD_BITS    0x04
#define DIA_DIAGN_FLAG_FAIL     0x05
#define DIA_DATA_TURN_FAIL      0x06
#define DIA_TURN_MISC_INI       0x08
#define DIA_TURN_MISC_FIN       0x10
#define DIA_TURN_GOOD_PARITY    0x18
#define DIA_TURN_BAD_PARITY     0x20
#define DIA_DIAGN_COMPLETE      0x80


struct mc68450_struct {
	struct channel {
	    unsigned char  status;
	    unsigned char  err;
	    short          r2;
	    unsigned char  dev_cntl;
	    unsigned char  oper_cntl;
	    unsigned char  seq_cntl;
	    unsigned char  cntl;
	    short          r8;
	    unsigned short count;
	    unsigned long  mem_addr;
	    long           r10;
	    unsigned long  dev_addr;
	    short          r18;
	    unsigned short base_count;
	    unsigned long  base_addr;
	    long           r20;
	    char           r24;
	    unsigned char  vect;
	    char           r26;
	    unsigned char  err_vect;
	    char           r28;
	    unsigned char  fc_mem;
	    char           r2a[3];
	    unsigned char  prio;
	    char           r2e[3];
	    unsigned char  fc_dev;
	    char           r32[7];
	    unsigned char  fc_base;
	    char           r3a[5];
	    unsigned char  gen_cntl;    /*  valid only in last chan (4)  */
	} chan[4];
};

/*  bits for dev_cntl   */
#define DCNTL_BURST_MODE        0x00    /*  burst transfer mode   */
#define DCNTL_CS_NO_HOLD        0x80    /*  cycle steal without hold   */
#define DCNTL_CS_HOLD           0xc0    /*  cycle steal with hold   */

#define DCNTL_EA_68000          0x00    /*  m68000 compatible,
						    explicitly addressed  */
#define DCNTL_EA_6800           0x10    /*  m6800 compatible,
						    explicitly addressed  */
#define DCNTL_IA_ACK            0x20    /*  device with ACK,
						    implicitly addressed  */
#define DCNTL_IA_ACK_RDY        0x30    /*  device with ACK and RDY,
						    implicitly addressed  */

#define DCNTL_PORT_BYTE         0x00    /*  8 bit device port   */
#define DCNTL_PORT_WORD         0x08    /*  16 bit device port   */

#define DCNTL_STAT_INPUT        0x00    /*  status input (level read in CSR) */
#define DCNTL_STAT_INPUT_INTR   0x01    /*  status input with interrupt   */
#define DCNTL_START_PULSE_OUT   0x02    /*  start pulse output   */
#define DCNTL_ABORT_INPUT       0x03    /*  abort input   */

/*  bits for oper_cntl   */
#define OCNTL_MEM_TO_DEV        0x00    /*  memory --> device transfer   */
#define OCNTL_DEV_TO_MEM        0x80    /*  device --> memory transfer   */

#define OCNTL_OPER_BYTE         0x00    /*  8 bit operand   */
#define OCNTL_OPER_WORD         0x10    /*  16 bit operand   */
#define OCNTL_OPER_LONG         0x20    /*  32 bit operand   */
#define OCNTL_BYTE_NOPACK       0x30    /*  byte, no packing   */

#define OCNTL_ARRAY_CHAIN_ENA   0x08    /*  array chaining enabled   */
#define OCNTL_LINK_CHAIN_ENA    0x0c    /*  linked array chaining enabled   */

#define OCNTL_INT_LIM           0x00    /*  internal request at limited rate */
#define OCNTL_INT_MAX           0x01    /*  internal request at maximum rate */
#define OCNTL_EXT_REQ           0x02    /*  external request   */
#define OCNTL_EXT_REQ_AUTO      0x03    /*  autostart, external request   */

/*  bits for seq_cntl   */
#define SCNTL_MEM_SAME          0x00    /*  mem_addr does not count   */
#define SCNTL_MEM_UP            0x04    /*  mem_addr counts up   */
#define SCNTL_MEM_DOWN          0x08    /*  mem_addr counts down   */

#define SCNTL_DEV_SAME          0x00    /*  dev_addr does not count   */
#define SCNTL_DEV_UP            0x01    /*  dev_addr counts up   */
#define SCNTL_DEV_DOWN          0x02    /*  dev_addr counts down   */

/*  bits for cntl   */
#define CCNTL_START             0x80    /*  start channel   */
#define CCNTL_CONT_EOB          0x40    /*  continue operation at end of block*/
#define CCNTL_HALT              0x20    /*  halt (suspend) operation  */
#define CCNTL_ABORT             0x10    /*  abort operation   */
#define CCNTL_INTR_ENA          0x08    /*  interrupts enable   */

/*  bits for status   */
#define STAT_OPER_COMPLETED     0x80    /*  channel operation has completed  */
#define STAT_BLTR_COMPLETED     0x40    /*  continued block transfer
							    has completed   */
#define STAT_DEV_TERM_OK        0x20    /*  device terminated operation
								with DONE   */
#define STAT_ERROR              0x10    /*  error has occured   */
#define STAT_ACTIVE             0x08    /*  channel is active   */
#define STAT_PCL_TRANSITION     0x02    /*  high-to-low PCL transition occured*/
#define STAT_PCL_HIGH           0x01    /*  PCL line is high   */

/*  values for err   */
#define ERR_CONFIG              0x01    /*  configuretion error   */
#define ERR_TIMING              0x02    /*  operation timing error   */
#define ERR_ADDR                0x04    /*  address error  */
#define ERR_BUS                 0x08    /*  bus error   */
#define ERR_COUNT               0x0c    /*  count error   */
#define ERR_EXTERN_ABORT        0x10    /*  external abort   */
#define ERR_SOFT_ABORT          0x11    /*  software abort   */
/*  register codes for address/bus/count errors   */
#define ERR_MEM                 0x01    /*  mem_addr or fc_mem   */
#define ERR_DEV                 0x02    /*  dev_addr   */
#define ERR_BASE                0x03    /*  base_addr or fc_base   */

/*  bits for gen_cntl   */
/*  burst transfer time   */
#define GCNTL_BT16              0x00    /*  16 clocks   */
#define GCNTL_BT32              0x04    /*  32 clocks   */
#define GCNTL_BT64              0x08    /*  64 clocks   */
#define GCNTL_BT128             0x0c    /*  128 clocks   */
/*  bandwidth available   */
#define GCNTL_BWA2              0x00    /*  50.00 %   */
#define GCNTL_BWA4              0x01    /*  25.00 %   */
#define GCNTL_BWA8              0x02    /*  12.50 %   */
#define GCNTL_BWA16             0x03    /*   6.25 %   */


struct wd1772_struct {
	char          r0;
	unsigned char cmd;      /*  0x7c  */
	char          r2;
	unsigned char track;    /*  0xff  */
	char          r4;
	unsigned char sector;   /*  0x01  */
	char          r6;
	unsigned char data;     /*  0x00  */
	char          r8;
};

struct mc68230_struct {
	char          r0;
	unsigned char gen_cntl;
	char          r2;
	unsigned char serv_req;
	char          r4;
	unsigned char a_dir;
	char          r6;
	unsigned char b_dir;
	char          r8;
	unsigned char c_dir;
	char          ra;
	unsigned char vect;
	char          rc;
	unsigned char a_cntl;
	char          re;
	unsigned char b_cntl;
	char          r10;
	unsigned char a_data;
	char          r12;
	unsigned char b_data;
	char          r14;
	unsigned char a_alt;
	char          r16;
	unsigned char b_alt;
	char          r18;
	unsigned char c_data;
	char          r1a;
	unsigned char status;
	char          r1c[5];
	unsigned char timer_cntl;
	char          r22;
	unsigned char timer_vect;
	char          r24;
	unsigned char timer_start0;
	char          r26;
	unsigned char timer_start1;
	char          r28;
	unsigned char timer_start2;
	char          r2a;
	unsigned char timer_start3;
	char          r2c;
	unsigned char timer_count0;
	char          r2e;
	unsigned char timer_count1;
	char          r30;
	unsigned char timer_count2;
	char          r32;
	unsigned char timer_count3;
	char          r34;
	unsigned char timer_status;
};

#define NULL    ((void *) 0)


static void (** const vector) (void) = (void (**) (void)) 0x000000;

volatile static struct ncr5385e_struct * const ncr =
					(struct ncr5385e_struct *) 0xc40000;
volatile static struct mc68450_struct  * const dma =
					(struct mc68450_struct *)  0xc80000;
volatile static struct wd1772_struct   * const flop =
					(struct wd1772_struct *)   0xcc0000;
volatile static unsigned char          * const cc0009 =
					(unsigned char *)          0xcc0009;
volatile static struct mc68230_struct  * const pit =
					(struct mc68230_struct *)  0xd00000;
static struct cwn_control_block        * const cwn_cntl =
					(struct cwn_control_block *) 0x2000;


struct scsi_cmd *scsi_base;
struct scsi_cmd *ncr_curr_cmd;
short scsi_chain_size = 0;
short common_intrs = 0;

struct scsi_cmd *saved_ptrs[8][8] = { { NULL, }, };
unsigned char busy_flags[8] = { 0, };

#define SHOW(x)     pit->b_data = (pit->b_data & 0xf0) | (~(x) & 0x0f)


#if 0
#define NUM_STATES      10

unsigned char events[NUM_STATES] = { 0, };
unsigned char states[NUM_STATES] = { 0, };
#endif

/*  values for ncr_state   */
#define NCR_FREE        0
#define TRY_SELECT      1
#define WAIT_FOR_BUS    2
#define CMD_SENDED      3
#define DATA_OK         4
#define STAT_OK         5
#define WAIT_FOR_FREE   6
#define WAIT_FOR_ID     7

volatile short ncr_state = NCR_FREE;

/*  possibly events...   */
#define E_FUNC_COMPLETE     1
#define E_RESELECTED        2
#define E_DISCONNECTED      3
#define E_DATA_OUT          4
#define E_DATA_IN           5
#define E_CMD_OUT           6
#define E_STAT_IN           7
#define E_MSG_OUT           8
#define E_MSG_IN            0   /*  hardcoded   */
#define E_MSG_COMPLETE      9
#define E_MSG_DISCONN       10
#define E_MSG_IGNORE        11
#define E_MSG_IDENTIFY      12
#define E_MSG_UNSUPP        13
#define E_BAD_INTR_AUX      14
#define E_TIMER_EXPIRED     15


#define NUM_EVENTS_SHIFT    4
#define NUM_EVENTS          (1 << NUM_EVENTS_SHIFT)
#define ES(EVENT,STATE)     ((STATE << NUM_EVENTS_SHIFT) | EVENT)

#define NCR_IO_FREE     0
#define NCR_IO_READ     1
#define NCR_IO_WRITE    2

volatile short ncr_io = NCR_IO_FREE;

short timeout_timer = 0;


/*  XXX:  or include ordinary scsi.h header ???   */
/*  various SCSI messages   */
#define COMMAND_COMPLETE        0x00
#define EXTENDED_MESSAGE        0x01
#define SAVE_POINTERS           0x02
#define RESTORE_POINTERS        0x03
#define DISCONNECT              0x04
#define INITIATOR_ERROR         0x05
#define ABORT                   0x06
#define MESSAGE_REJECT          0x07
#define NOP                     0x08
#define MSG_PARITY_ERROR        0x09
#define LINKED_CMD_COMPLETE     0x0a
#define LINKED_FLG_CMD_COMPLETE 0x0b
#define BUS_DEVICE_RESET        0x0c
#define INITIATE_RECOVERY       0x0f    /* SCSI-II only */
#define RELEASE_RECOVERY        0x10    /* SCSI-II only */
#define SIMPLE_QUEUE_TAG        0x20
#define HEAD_OF_QUEUE_TAG       0x21
#define ORDERED_QUEUE_TAG       0x22

unsigned char ncr_msg = NOP;


struct intr_chan {
	short count;
	unsigned char ack_bit;
	unsigned char intr_bit;
} intr_chan[4] = {
	{ 0, 0x10, 0x01 },
	{ 0, 0x20, 0x02 },
	{ 0, 0x40, 0x04 },
	{ 0, 0x80, 0x08 }
};


/*  tasks` indexes...  */
#define IDLE_TASK       0
#define SCSI_TASK       1
#define SCSI_IO_TASK    2

#define IDLE_PRIO       10
#define SCSI_PRIO       10
#define SCSI_IO_PRIO    10

void idle_task (void);
void scsi_task (void);
void scsi_io_task (void);

struct task {
	unsigned short wait_for;
	unsigned char prio;
	unsigned char prio_base;
	unsigned long regs[2];      /*  %fp, %sp   */
	unsigned long stack;
	void (*entry) (void);
} tasks[] = {
	[IDLE_TASK] = { 0, 0, IDLE_PRIO,    { 0, 0 }, 0, idle_task },
	[SCSI_TASK] = { 0, 0, SCSI_PRIO,    { 0, 0 }, 0, scsi_task },
	[SCSI_IO_TASK] = { 0, 0, SCSI_IO_PRIO, { 0, 0 }, 0, scsi_io_task },
};
#define NUM_TASKS       (sizeof (tasks) / sizeof (*tasks))

struct task *curr_task;

struct frame {
	unsigned long regs[4];      /*  hardcoded: saved %d0-%d1/%a0-%a1   */
	unsigned short sr;          /*  saved  %sr   */
	unsigned long pc;           /*  saved  %pc   */
	unsigned int format: 4;     /*  format frame specifier   */
	unsigned int vector: 10;    /*  vector number   */
	unsigned int filler: 2;
} __attribute__ ((packed));


#define cli()   __asm volatile ("mov.w  &0x2700,%%sr" : : );
#define sti()   __asm volatile ("mov.w  &0x2000,%%sr" : : );
#define save_flags(x)       __asm volatile ("mov.w  %%sr,%0" : "=d" (x) : );
#define restore_flags(x)    __asm volatile ("mov.w  %0,%%sr" : : "d" (x) );

__inline static void push (struct scsi_cmd *cmd) {
	unsigned char target = cmd->target & 0x07;
	unsigned char lun = (cmd->cmd[1] >> 5) & 0x07;

	saved_ptrs[target][lun] = cmd;
	busy_flags[target] |= (1 << lun);

	return;
}

__inline static struct scsi_cmd *pop (int target, int lun) {
	struct scsi_cmd *ptr;

	ptr = saved_ptrs[target][lun];
	saved_ptrs[target][lun] = NULL;
	busy_flags[target] &= ~(1 << lun);

	return ptr;
}

#define put_one_byte(BYTE)      \
    ({                          \
	ncr->cmd = CMD_TRANSFER_INFO | CMD_SINGLE_BYTE; \
							\
	while (ncr->aux_stat & AUX_DATA_FULL)           \
	    if (ncr_state == NCR_FREE)  return;         \
				\
	ncr->data = (BYTE);     \
    })

#define get_one_byte()  \
    ({                  \
	ncr->cmd = CMD_TRANSFER_INFO | CMD_SINGLE_BYTE; \
							\
	while (!(ncr->aux_stat & AUX_DATA_FULL))        \
	    if (ncr_state == NCR_FREE)  return;         \
			\
	ncr->data;      \
    })


void schedule (void);
void switch_to (void *old_regs, void *new_regs);
void panic (const char *str);
void clear_scsi_stuff (void);
int scsi_init (void *data);
void end_scsi_cmd (struct scsi_cmd *cmd, int res);
void wake_up (int task_num);
void set_intr (unsigned int channel);
void scsi_intr (void);

/*      GLOBAL  SOFTWARE  +  SCHEDULING  STUFF      */

#define DECL_INTH(A,B)  \
extern void A (void);   \
			\
__asm ("\n"             \
"text\n"                \
#A ":\n\t"                                      \
	"movm.l  %d0-%d1/%a0-%a1,-(%sp)\n\t"    \
	"jsr    " #B "\n\t"                     \
	"movm.l  (%sp)+,%d0-%d1/%a0-%a1\n\t"    \
	"rte");

DECL_INTH (bad_inth, bad_intr)
DECL_INTH (dma_inth, bad_intr)
DECL_INTH (scsi_inth, scsi_intr)
DECL_INTH (floppy_inth, bad_intr)
DECL_INTH (timer_inth, timer_intr)
DECL_INTH (abort_inth, bad_intr)

#define HZ      100
#define TIMEOUT_TICK    HZ
/*  one select tick is 1024 clock intervals... (10 MHz, I hope...)   */
#define SELECT_HZ       (10000000 / 1024)
/*  for select, we should wait 250ms, or more...  */
#define SELECT_WAIT     ((SELECT_HZ * 25) / 100)

#define VEC_UNINT   (15)
#define VEC_SPUR    (24)
#define VEC_INT1    (25)
#define VEC_INT2    (26)
#define VEC_INT3    (27)
#define VEC_INT4    (28)
#define VEC_INT5    (29)
#define VEC_INT6    (30)
#define VEC_INT7    (31)

#define NUM_VECTORS     32

#define STACK_SIZE      256
#define INI_STACK_START ((4 * NUM_VECTORS) + STACK_SIZE)
#define CODE_OFFSET     2048

#define SET_FOR_LINKER(A,B)     SET_FOR_LINKER1(A,B)
#define SET_FOR_LINKER1(A,B)    __asm (#A " = " #B)
SET_FOR_LINKER (cwn_soft_ini_stack, INI_STACK_START);
SET_FOR_LINKER (cwn_soft_code_start, CODE_OFFSET);


void _start (void) {
	volatile static short initialized = 0;
	int i, stack_area, preload;

	/*  many things already initialized by ordinary software...  */

	for (i = 2; i < NUM_VECTORS; i++)  vector[i] = bad_inth;
	vector[VEC_INT2] = dma_inth;
	vector[VEC_INT3] = scsi_inth;
	vector[VEC_INT4] = floppy_inth;
	vector[VEC_INT5] = timer_inth;
	vector[VEC_INT7] = abort_inth;


	pit->timer_status = 0x01;       /*  reset interrupt status   */
	pit->timer_cntl = 0xa0;         /*  stop timer   */

	preload = (8064000 / HZ / 32) + 1;

	pit->timer_start0 = (preload >> 24) & 0xff;
	pit->timer_start1 = (preload >> 16) & 0xff;
	pit->timer_start2 = (preload >> 8) & 0xff;
	pit->timer_start3 = preload & 0xff;

	pit->timer_vect = VEC_INT5;     /*  but it produce VEC_SPUR instead  */
	pit->vect = VEC_INT7;

	pit->timer_cntl = 0xa1;         /*  start timer   */

	pit->c_data &= ~0x02;   /*  disable sysfail   */

	dma->chan[0].status = 0xff;
	dma->chan[0].seq_cntl = SCNTL_MEM_UP | SCNTL_DEV_SAME;
	dma->chan[0].dev_addr = (unsigned long) &ncr->data;
	dma->chan[0].fc_mem = dma->chan[0].fc_dev = dma->chan[0].fc_base = 0x5;
	dma->chan[0].dev_cntl = DCNTL_IA_ACK_RDY | DCNTL_PORT_BYTE |
							DCNTL_STAT_INPUT;
	dma->chan[3].gen_cntl = GCNTL_BT128 | GCNTL_BWA2;


	stack_area = INI_STACK_START;

	for (i = 0; i < NUM_TASKS; i++) {

	    tasks[i].stack = (stack_area += STACK_SIZE);

	    /*  The easiest way to init task->regs by this point.
	       Current %fp/%sp  are stored in the `task->regs',
	       and then restored from the same `task->regs'.
	    */
	    switch_to (tasks[i].regs, tasks[i].regs);

	    /*  `initialized != 0' means we are restored by `schedule()',
	       so, go to the appropriate stuff...
	    */
	    if (initialized)  goto jump;
	}

	initialized = 1;

	curr_task = &tasks[0];

	cwn_cntl->cntl = SS_OWN_USER;

	timeout_timer = TIMEOUT_TICK;

jump:
	__asm volatile ("" : : : "memory" );  /*  paranoia ???   */

	__asm volatile ("mov.l %0,%%sp\n\t"         /*  set stack   */
			"mov.w &0x2000,%%sr\n\t"    /*  sti()   */
			"jsr   (%1)"                /*  jump   */
			:
			: "a" (curr_task->stack), "a" (curr_task->entry)
			: "memory"
	);

	/*  should not be reached   */
	panic ("task exit");
}

void bad_intr (int unused) {

	panic ("bad interrupt");
}


#if 0
static char symbol[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
			   '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
};
#endif

void timer_intr (void) {

	pit->timer_status = 0x01;       /*  reset interrupt status   */
	pit->timer_cntl = 0xa1;         /*  set timer again   */

	/*  retrigger the watch dog   */
	pit->c_data &= ~0x10;
	pit->c_data |= 0x10;

	/*  Run flashes stuff, if enabled   */
	/*  XXX:  May be read run/local, etc.   */

	if (curr_task->prio > 0)  curr_task->prio--;

	timeout_timer--;
	if (timeout_timer == 0) {
	    struct scsi_cmd *cmd;
	    int i;

	    timeout_timer = TIMEOUT_TICK;

	    if (ncr_state != NCR_FREE &&
		--ncr_curr_cmd->timeout == 0
	    )  scsi_intr ();

	    for (i = 0, cmd = scsi_base; i < scsi_chain_size; i++, cmd++) {

		if ((signed short) cmd->cntl <= 0 ||
		    !(cmd->cntl & SC_DISCONNECTED) ||
		    --cmd->timeout != 0
		)  continue;

		cmd->cntl = (cmd->cntl & ~(SC_OWN_BOARD | SC_CMD_MASK)) |
								SC_ABORT_CMD;
	    }
	}

	return;
}


/*  All unices  hardcore...  The task which never wait...  */
void idle_task (void) {

	while (1) {
	    short sended;

	    /*  first, check for waiting interrupts...  */
	    if (!common_intrs) {
		do {
		    short i;

		    sended = 0;
		    for (i = 0; i < 4; i++) {
			if (!intr_chan[i].count)  continue;

			if (!(pit->a_data & intr_chan[i].ack_bit))  continue;

			pit->a_data &= ~intr_chan[i].intr_bit;
			pit->a_data |= intr_chan[i].intr_bit;

			intr_chan[i].count--;
			sended = 1;
		    }
		} while (sended) ;
	    }

	    /*  handle global board cmd port...  */

	    if ((cwn_cntl->cntl & SC_MASK) > 0) {
		unsigned short res;

		switch (cwn_cntl->cntl & SC_CMD_MASK) {

		    case SCSI_INIT:
			res = scsi_init ((void *) cwn_cntl->cmd_data);
			break;

		    case FLOP_INIT:
			res = 0;
			break;

		    case FLASHES_INIT:
			res = 0;
			break;

		    case INTR_INIT:
			common_intrs = ((struct intr_init_block *)
					    cwn_cntl->cmd_data)->common_intrs;
			res = 0;
			break;

		    default:
			res = RES_BAD_CMD;
			break;
		}

		if (cwn_cntl->cntl & SC_INTR_ON_EOP) {
		    cwn_cntl->cntl = SS_OWN_USER | (res << 8);

		    set_intr (GLOBAL_INTR_CHAN);
		}
		else
		    cwn_cntl->cntl = SS_OWN_USER | (res << 8);
	    }

	    schedule();     /*  All unices hardcore...  */
	}
}


/*     Schedule   stuff        */

/*  when invokes, interrupts should be disabled...  */
void sleep_on (void) {

	curr_task->wait_for = 1;

	sti();
	schedule();
	cli();

	return;
}

void wake_up (int task_num) {

	tasks[task_num].wait_for = 0;

	return;
}

void schedule (void) {
	int i;
	struct task *good = NULL;

rescan_tasks:
	for (i = 0; i < NUM_TASKS; i++) {
	    if (tasks[i].wait_for)  continue;

	    if (!good ||
		tasks[i].prio > good->prio
	    )  good = &tasks[i];
	}

	if (!good)  panic ("!good");

	/*  Linux way...   */
	if (good->prio == 0) {
	    for (i = 0; i < NUM_TASKS; i++)
		    tasks[i].prio = (tasks[i].prio >> 1) + tasks[i].prio_base;
	    goto rescan_tasks;
	}

	if (curr_task != good) {
	    void *old = curr_task->regs;
	    void *new = good->regs;

	    curr_task = good;

	    cli();      /*  common stacks with interrupts...  */
	    switch_to (old, new);   /*  and we play with another stack...  */
	    sti();
	}

	return;
}

void switch_to (void *a, void *b) {
	register void *old __asm ("%a0") = a;
	register void *new __asm ("%a1") = b;

	/*  We specify that all registers would be dirtied.
	   So, compiler will store all is needable in the stack.
	    Only %sp and %fp should be handled immediately.
	*/
	__asm volatile ("mov.l  %%fp,(%0)\n\t"
			"mov.l  %%sp,4(%0)\n\t"
			"mov.l  (%1),%%fp\n\t"
			"mov.l  4(%1),%%sp"
			:
			: "a" (old), "a" (new)
			: "%d0", "%d1", "%d2", "%d3",
			  "%d4", "%d5", "%d6", "%d7",
			  "%a0", "%a1", "%a2", "%a3",
			  "%a4", "%a5", "memory"
	);

	return;
}


/*      Interrupt sending stuff        */

/*  send interrupt, if it is possible,
   or wakeup special task for send it later...
*/
void set_intr (unsigned int channel) {
	struct intr_chan *ichp = intr_chan + channel;

	if (pit->a_data & ichp->ack_bit) {
	    pit->a_data &= ~ichp->intr_bit;
	    pit->a_data |= ichp->intr_bit;

	} else
	    ichp->count++;

	return;
}


/*    Board message to system  stuff     */

void send_sys_msg (int event, int stat, int aux_stat, const char *str) {
	int i;
	char *p = cwn_cntl->msg_string;
	unsigned short flags;

	save_flags (flags);
	cli();

	i = 100000;
	while (cwn_cntl->msg_valid && i--) ;

	/*  generate a message text   */
	cwn_cntl->msg_state = ncr_state;
	cwn_cntl->msg_event = event;
	cwn_cntl->msg_aux_stat = aux_stat;
	cwn_cntl->msg_stat = stat;

#if 0
	for (i = 0; i < NUM_STATES; i++) {
	    *p++ = symbol[events[i]];
	    *p++ = '/';
	    *p++ = symbol[states[i]];
	    *p++ = ' ';
	}

	*p++ = ' ';
#endif
	while ((*p++ = *str++) != '\0') ;

	cwn_cntl->msg_valid = 1;

	set_intr (GLOBAL_INTR_CHAN);

	restore_flags (flags);

	return;
}

void panic (const char *str) {

	cli();

	send_sys_msg (-1, -1, -1, str);

	__asm volatile ("stop   &0x2700" : : );     /*  global vasya   */
}


/*     SCSI   STUFF     */

int scsi_init (void *data) {
	struct scsi_init_block *sib = (struct scsi_init_block *) data;
	static int initialized = 0;
	struct scsi_cmd *cmd;
	int i;

	if (sib->it == 1)  return RES_NOSYS;
	if (sib->own_id > 7 ||
	    sib->scsi_chain_size == 0 ||
	    sib->scsi_base < 0x2000 ||
	    sib->scsi_base +
		sib->scsi_chain_size * sizeof (struct scsi_cmd) >= 0x20000
	)  return RES_BAD_PARAM;

	cli();

	if (initialized) {
	    /*  all cmds in scsi_chain should be free...   */
	    if (ncr_state != NCR_FREE)  { sti();  return RES_BUSY; }

	    for (i = 0, cmd = scsi_base; i < scsi_chain_size; i++, cmd++)
		if (cmd->cntl & SC_OWN_BOARD) {
			sti();
			return RES_BUSY;
		}
	}


	/*  OK, nothing to loose   */

	if (sib->own_id != ncr->id) {
	    /*  set a new device scsi id...  */
	    *cc0009 = (*cc0009 & 0x1f) | ~(sib->own_id << 5);

	    /*  XXX:  should we do a hard scsi bus reset in this case ???  */
	    i = 100;
	    while (i--);    /*  XXX:  is it a paranoia ???  */

	    if (sib->own_id != ncr->id)
		    panic ("cannot set another device id");
	}

	scsi_base = (struct scsi_cmd *) sib->scsi_base;
	scsi_chain_size = sib->scsi_chain_size;

	for (i = 0, cmd = scsi_base; i < scsi_chain_size; i++, cmd++)
		cmd->cntl = 0;

	clear_scsi_stuff();

	initialized = 1;

	sti();
	return 0;
}

/*  clear all scsi stuff, except cmd areas...  */
void clear_scsi_stuff (void) {
	int i;
	struct scsi_cmd **ptr;

	intr_chan[SCSI_INTR_CHAN].count = 0;

	for (i = 0, ptr = &saved_ptrs[0][0];
		 i < sizeof (saved_ptrs) / sizeof (saved_ptrs[0][0]);
		     i++, ptr++
	)  *ptr = NULL;
	for (i = 0; i < (sizeof (busy_flags) / sizeof (*busy_flags)); i++)
		busy_flags[i] = 0;

	ncr->cntl = CNTL_RESELECT_ENA;
	ncr_state = NCR_FREE;
	wake_up (SCSI_TASK);

	ncr_io = NCR_IO_FREE;

	return;
}


void end_scsi_cmd (struct scsi_cmd *cmd, int res) {

	if (cmd->cntl & SC_INTR_ON_EOP) {
	    cmd->cntl = SS_OWN_USER | (res << 8);

	    set_intr (SCSI_INTR_CHAN);
	}
	else
	    cmd->cntl = SS_OWN_USER | (res << 8);

	return;
}


void do_hard_reset (void) {
	int i;
	struct scsi_cmd *cmd;
	unsigned short flags;

	save_flags (flags);     /*  may be invoked by intr handlers...  */
	cli();

	/*  do global SCSI bus reset...  */

	pit->b_data &= ~0x10;
	i = 100;
	while (i--) ;
	pit->b_data |= 0x10;

	i = 100000;
	while (i--) ;

	ncr->cmd = CMD_CHIP_RESET;
	while (!(ncr->diagn_stat & DIA_DIAGN_COMPLETE)) ;


	for (i = 0, cmd = scsi_base; i < scsi_chain_size; i++, cmd++)
		if (cmd->cntl & SC_OWN_BOARD)
			end_scsi_cmd (cmd, RES_RESET);

	clear_scsi_stuff();

	restore_flags (flags);

	return;
}


void scsi_task (void) {
	struct scsi_cmd *cmd;
	int i;

scsi_rescan:
	for (i = 0, cmd = scsi_base; i < scsi_chain_size; i++, cmd++) {

	    if ((signed char) (cmd->cntl & SC_MASK) <= 0)  continue;

	    if (cmd->size &&
		(cmd->addr < 0x2000 || cmd->addr + cmd->size >= 0x20000)
	    ) {
		    end_scsi_cmd (cmd, RES_BAD_PARAM);
		    continue;
	    }

	    cli();      /*  common way   */

	    switch (cmd->cntl & SC_CMD_MASK) {

		case SC_DO_CMD:

		    cmd->cntl |= SC_OWN_BOARD;

		    while (ncr_state != NCR_FREE)  sleep_on();

		    /*  XXX:  should we do this per lun separately ???  */
		    if (busy_flags[cmd->target] != 0x00) {

			cmd->cntl &= ~SC_OWN_BOARD;

			sti();
			continue;
		    }

		    /*  high level should `& 0x07'   */
		    ncr->target = cmd->target;

		    ncr->cnt_h = 0;     /*  XXX:  to omit ???  */
		    ncr->cnt_m = SELECT_WAIT >> 8;
		    ncr->cnt_l = SELECT_WAIT & 0xff;

		    /*  Omit send_identify phase when should not disconnect
		       for speedup reasons...
		    */
		    if (cmd->cntl & SC_MAY_DISCONN) {
			    ncr_msg = 0x80 | 0x40 | ((cmd->cmd[1] >> 5) & 0x7);
			    ncr->cmd = CMD_SELECT_W_ATN;
		    } else {
			    ncr_msg = 0x80 | ((cmd->cmd[1] >> 5) & 0x7);
			    ncr->cmd = CMD_SELECT_WO_ATN;
		    }

		    ncr_state = TRY_SELECT;
		    ncr_curr_cmd = cmd;

		    break;

		case SC_ABORT_CMD:
		    if (!(cmd->cntl & SC_OWN_BOARD)) {
			    end_scsi_cmd (cmd, RES_ABORTED);
			    break;
		    }

		    if (ncr_state != NCR_FREE &&
			cmd->target == ncr_curr_cmd->target
		    ) {
			/*  i.e., currently executing  */
			ncr_msg = ABORT;
			ncr->cmd = CMD_SET_ATN;

			ncr_state = WAIT_FOR_BUS;
			break;
		    }

		    while (ncr_state != NCR_FREE)  sleep_on();


		    if (!(cmd->cntl & SC_OWN_BOARD))  break;    /*  touched */

		    /*  high level should `& 0x07'   */
		    ncr->target = cmd->target;

		    ncr->cnt_h = 0;     /*  XXX:  to omit ???  */
		    ncr->cnt_m = SELECT_WAIT >> 8;
		    ncr->cnt_l = SELECT_WAIT & 0xff;

		    ncr_msg = ABORT;

		    /*  Always do select with ATN hear...  */
		    ncr->cmd = CMD_SELECT_W_ATN;

		    ncr_state = TRY_SELECT;
		    ncr_curr_cmd = cmd;

		    break;

		case SC_RESET:
		    end_scsi_cmd (cmd, 0);

		    do_hard_reset();

		    break;

		default:
		    end_scsi_cmd (cmd, RES_BAD_CMD);
		    break;
	    }

	    sti();      /*  common way   */
	}

	schedule();

	goto scsi_rescan;
}


/*   SCSI  interrupt  stuff   */

#define _0      E_BAD_INTR_AUX
#define  t      E_TIMER_EXPIRED
#define  f      E_FUNC_COMPLETE
#define  b      0               /*  continue with aux_stat ...  */
#define  d      E_DISCONNECTED
#define  s      E_BAD_INTR_AUX  /*  currently don`t allow selecting...  */
#define  r      E_RESELECTED

unsigned char ev0[128] = {
/*        0   1   2   3   4   5   6   7   8   9   a   b   c   d   e   f  */
/*0*/     t,  f,  b, _0,  d, _0, _0, _0,  s, _0, _0, _0, _0, _0, _0, _0,
/*1*/     r, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*2*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*3*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*4*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*5*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*6*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*7*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0
};

#undef  _0
#undef   t
#undef   f
#undef   b
#undef   d
#undef   s
#undef   r

#define _0      E_BAD_INTR_AUX
#define  w      E_DATA_OUT
#define  r      E_DATA_IN
#define  c      E_CMD_OUT
#define  s      E_STAT_IN
#define  o      E_MSG_OUT
#define  i      E_MSG_IN

unsigned char ev1[256] = {
/*        0   1   2   3   4   5   6   7   8   9   a   b   c   d   e   f  */
/*0*/     w,  w,  w,  w,  w,  w,  w,  w,  r,  r,  r,  r,  r,  r,  r,  r,
/*1*/     c,  c,  c,  c,  c,  c,  c,  c,  s,  s,  s,  s,  s,  s,  s,  s,
/*2*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*3*/     o,  o,  o,  o,  o,  o,  o,  o,  i,  i,  i,  i,  i,  i,  i,  i,
/*4*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*5*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*6*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*7*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*8*/     w,  w,  w,  w,  w,  w,  w,  w,  r,  r,  r,  r,  r,  r,  r,  r,
/*9*/     c,  c,  c,  c,  c,  c,  c,  c,  s,  s,  s,  s,  s,  s,  s,  s,
/*a*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*b*/     o,  o,  o,  o,  o,  o,  o,  o,  i,  i,  i,  i,  i,  i,  i,  i,
/*c*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*d*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*e*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0,
/*f*/    _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0, _0
};

#undef _0
#undef w
#undef r
#undef c
#undef s
#undef o
#undef i

unsigned char ev2[256] = {

	[COMMAND_COMPLETE] = E_MSG_COMPLETE,
	[EXTENDED_MESSAGE] = E_MSG_UNSUPP,
	[SAVE_POINTERS]    = E_MSG_IGNORE,
	[RESTORE_POINTERS] = E_MSG_IGNORE,
	[DISCONNECT]       = E_MSG_DISCONN,
	[INITIATOR_ERROR]  = E_MSG_UNSUPP,
	[ABORT]            = E_MSG_COMPLETE,
	[MESSAGE_REJECT]   = E_MSG_UNSUPP,
	[NOP]              = E_MSG_IGNORE,
	[MSG_PARITY_ERROR ... 0x7f] = E_MSG_UNSUPP,
	[0x80 ... 0xff]    = E_MSG_IDENTIFY

};


void stop_scsi_io (void) {
	unsigned short len;

	if (ncr_io == NCR_IO_FREE)  return;

#if 0
	dma->chan[0].cntl = CCNTL_ABORT;    /* XXX: or only if SC_BLOCK_XFER */
#endif


	if (ncr->aux_stat & AUX_CNT_ZERO)  len = 0;
	else  len = (ncr->cnt_m << 8) | ncr->cnt_l;     /*  leaved bytes...  */

	ncr_curr_cmd->addr += (ncr_curr_cmd->size - len);
	ncr_curr_cmd->size = len;

#if 1
	if (len)  send_sys_msg (-1, (len >> 8) & 0xff, len & 0xff, "stop io");
#endif
	ncr_io = NCR_IO_FREE;

	return;
}


void scsi_intr (void) {
	unsigned char stat, aux_stat, msg = 0;
	unsigned char event;
	struct scsi_cmd *cmd = ncr_curr_cmd;

	aux_stat = ncr->aux_stat;
	stat = ncr->intr & 0x7f;

scsi_intr_restart:

	event = ev0[stat];
	if (!event)  event = ev1[aux_stat];     /*  i.e., bus_service   */

scsi_intr_rescan:

#if 0
	{ int i;

	  for (i = 0; i < NUM_STATES - 1; i++) {
		events[i] = events[i + 1];
		states[i] = states[i + 1];
	  }
	  events[i] = event;
	  states[i] = ncr_state;
	}
#endif

	switch (ES (event, ncr_state)) {

	    case ES (E_FUNC_COMPLETE, TRY_SELECT):

		ncr_state = WAIT_FOR_BUS;
		break;

	    case ES (E_FUNC_COMPLETE, DATA_OK):

		stop_scsi_io();

	    case ES (E_FUNC_COMPLETE, NCR_FREE):
	    case ES (E_FUNC_COMPLETE, WAIT_FOR_BUS):
	    case ES (E_FUNC_COMPLETE, CMD_SENDED):
	    case ES (E_FUNC_COMPLETE, STAT_OK):
	    case ES (E_FUNC_COMPLETE, WAIT_FOR_ID):

		break;      /*  just ignore it ???  */

	    case ES (E_FUNC_COMPLETE, WAIT_FOR_FREE):

		break;

	    case ES (E_DISCONNECTED, DATA_OK):

		stop_scsi_io();

	    case ES (E_DISCONNECTED, CMD_SENDED):

		send_sys_msg (event, stat, aux_stat, "disconnected");

		/*  fall through...  */

	    case ES (E_DISCONNECTED, TRY_SELECT):
	    case ES (E_DISCONNECTED, WAIT_FOR_BUS):

		end_scsi_cmd (cmd, RES_BAD_TARGET);

		ncr_state = NCR_FREE;
		wake_up (SCSI_TASK);

		break;

	    case ES (E_DISCONNECTED, STAT_OK):

		end_scsi_cmd (cmd, 0);      /*  is it a good idea ???  */

		ncr_state = NCR_FREE;
		wake_up (SCSI_TASK);

		break;

	    case ES (E_DISCONNECTED, NCR_FREE):
	    case ES (E_DISCONNECTED, WAIT_FOR_FREE):
	    case ES (E_DISCONNECTED, WAIT_FOR_ID):

		ncr_state = NCR_FREE;
		wake_up (SCSI_TASK);

		break;

	    case ES (E_RESELECTED, TRY_SELECT):

		cmd->cntl &= ~SC_OWN_BOARD;     /*  uncatch cmd area   */

		/*  fall throw   */

	    case ES (E_RESELECTED, NCR_FREE):

		ncr_state = WAIT_FOR_ID;
		break;

	    case ES (E_RESELECTED, DATA_OK):
	    case ES (E_BAD_INTR_AUX, DATA_OK):

		stop_scsi_io();

	    case ES (E_RESELECTED, WAIT_FOR_BUS):
	    case ES (E_RESELECTED, CMD_SENDED):
	    case ES (E_RESELECTED, STAT_OK):
	    case ES (E_RESELECTED, WAIT_FOR_FREE):
	    case ES (E_RESELECTED, WAIT_FOR_ID):
	    case ES (E_DATA_OUT, TRY_SELECT):
	    case ES (E_DATA_OUT, NCR_FREE):
	    case ES (E_DATA_IN, TRY_SELECT):
	    case ES (E_DATA_IN, NCR_FREE):
	    case ES (E_CMD_OUT, TRY_SELECT):
	    case ES (E_CMD_OUT, NCR_FREE):
	    case ES (E_STAT_IN, TRY_SELECT):
	    case ES (E_STAT_IN, NCR_FREE):
	    case ES (E_MSG_OUT, TRY_SELECT):
	    case ES (E_MSG_OUT, NCR_FREE):
	    case ES (E_MSG_IN, NCR_FREE):
	    case ES (E_MSG_IN, TRY_SELECT):
	    case ES (E_BAD_INTR_AUX, NCR_FREE):
	    case ES (E_BAD_INTR_AUX, TRY_SELECT):
	    case ES (E_BAD_INTR_AUX, WAIT_FOR_BUS):
	    case ES (E_BAD_INTR_AUX, CMD_SENDED):
	    case ES (E_BAD_INTR_AUX, STAT_OK):
	    case ES (E_BAD_INTR_AUX, WAIT_FOR_FREE):
	    case ES (E_BAD_INTR_AUX, WAIT_FOR_ID):

		send_sys_msg (event, stat, aux_stat, "bug board");
		do_hard_reset();
		break;

	    case ES (E_DATA_OUT, DATA_OK):

		stop_scsi_io();

	    case ES (E_DATA_OUT, CMD_SENDED):

		ncr_io = NCR_IO_WRITE;

		wake_up (SCSI_IO_TASK);

		ncr_state = DATA_OK;
		break;

	    case ES (E_DATA_IN, DATA_OK):

		stop_scsi_io();

	    case ES (E_DATA_IN, CMD_SENDED):

		ncr_io = NCR_IO_READ;

		wake_up (SCSI_IO_TASK);

		ncr_state = DATA_OK;
		break;

	    case ES (E_DATA_OUT, WAIT_FOR_BUS):
	    case ES (E_DATA_OUT, STAT_OK):
	    case ES (E_DATA_OUT, WAIT_FOR_ID):
	    case ES (E_DATA_OUT, WAIT_FOR_FREE):
	    case ES (E_CMD_OUT, STAT_OK):
	    case ES (E_CMD_OUT, WAIT_FOR_ID):
	    case ES (E_CMD_OUT, WAIT_FOR_FREE):
	    case ES (E_MSG_OUT, WAIT_FOR_FREE):

		/*  transfer pad `out' to make target happy...  */

		ncr->cnt_h = 0;     /*  XXX to omit ??? */
		ncr->cnt_m = 0xff;
		ncr->cnt_l = 0xff;

		ncr->cmd = CMD_TRANSFER_PAD;

		ncr->data = 0;

		break;

	    case ES (E_DATA_IN, WAIT_FOR_BUS):
	    case ES (E_DATA_IN, STAT_OK):
	    case ES (E_DATA_IN, WAIT_FOR_ID):
	    case ES (E_STAT_IN, STAT_OK):
	    case ES (E_STAT_IN, WAIT_FOR_ID):
	    case ES (E_DATA_IN, WAIT_FOR_FREE):
	    case ES (E_STAT_IN, WAIT_FOR_FREE):
	    case ES (E_MSG_IN, WAIT_FOR_FREE):

		/*  transfer pad `in' to make target happy...  */

		ncr->cnt_h = 0;     /*  XXX to omit ??? */
		ncr->cnt_m = 0xff;
		ncr->cnt_l = 0xff;

		/*  XXX: but what if MSG_IN should be ACCEPTED ???  */
		ncr->cmd = CMD_TRANSFER_PAD;

		break;

	    case ES (E_CMD_OUT, WAIT_FOR_BUS):
		{   unsigned char cmd_len;
		    unsigned char *cmdp = cmd->cmd;

		    cmd_len = ((char []) { 6,10,10,12,12,12,10,10 })
						[(cmdp[0] >> 5) & 0x7];
		    ncr->cnt_h = 0;     /*  XXX to omit ??? */
		    ncr->cnt_m = 0;     /*  XXX to omit ??? */
		    ncr->cnt_l = cmd_len;

		    ncr->cmd = CMD_TRANSFER_INFO;

		    while (cmd_len--) {
			while (ncr->aux_stat & AUX_DATA_FULL)
				if (ncr_state == NCR_FREE)  return;

			ncr->data = *cmdp++;
		    }
		}

		ncr_state = CMD_SENDED;
		break;

	    case ES (E_CMD_OUT, DATA_OK):

		stop_scsi_io();

	    case ES (E_CMD_OUT, CMD_SENDED):

		/*  atn, pad, abort  */

		ncr->cmd = CMD_SET_ATN;

		ncr->cnt_h = 0;     /*  XXX to omit ??? */
		ncr->cnt_m = 0xff;
		ncr->cnt_l = 0xff;

		ncr->cmd = CMD_TRANSFER_PAD;
		put_one_byte (0);

		ncr_msg = ABORT;
		ncr_state = WAIT_FOR_BUS;

		break;

	    case ES (E_STAT_IN, DATA_OK):

		stop_scsi_io();

	    case ES (E_STAT_IN, WAIT_FOR_BUS):
	    case ES (E_STAT_IN, CMD_SENDED):

		cmd->status = get_one_byte();

		ncr_state = STAT_OK;
		break;

	    case ES (E_MSG_OUT, WAIT_FOR_BUS):

		put_one_byte (ncr_msg);
		break;


	    case ES (E_MSG_OUT, DATA_OK):

		stop_scsi_io();

	    case ES (E_MSG_OUT, CMD_SENDED):
	    case ES (E_MSG_OUT, STAT_OK):

		put_one_byte (MSG_PARITY_ERROR);    /*  XXX: or NOP ???  */
		break;

	    case ES (E_MSG_OUT, WAIT_FOR_ID):

		put_one_byte (ABORT);   /*  what can I say also ?!?!?!  */
		break;

	    case ES (E_MSG_IN, DATA_OK):

		stop_scsi_io ();

	    case ES (E_MSG_IN, WAIT_FOR_BUS):
	    case ES (E_MSG_IN, CMD_SENDED):
	    case ES (E_MSG_IN, STAT_OK):
	    case ES (E_MSG_IN, WAIT_FOR_ID):

		/*  read a message...  */
		msg = get_one_byte();

		event = ev2[msg];       /*  new event   */

		if (msg == EXTENDED_MESSAGE) {
		    ncr->cnt_h = 0;
		    ncr->cnt_m = 0xff;
		    ncr->cnt_l = 0xff;

		    ncr->cmd = CMD_TRANSFER_PAD;
		}

		/*  to catch E_FUNC_COMPLETE on end of message...  */
		while (!(ncr->intr & INTR_FUNC_COMPLETE))
			if (ncr_state == NCR_FREE)  return;

		goto  scsi_intr_rescan;
		break;

	    case ES (E_MSG_COMPLETE, STAT_OK):

		ncr->cmd = CMD_MESSAGE_ACCEPTED;

		end_scsi_cmd (cmd, 0);

		ncr_state = WAIT_FOR_FREE;
		break;

	    case ES (E_MSG_COMPLETE, DATA_OK):
	    case ES (E_MSG_IDENTIFY, DATA_OK):

		/*  scsi_io_task already stopped...  */

	    case ES (E_MSG_COMPLETE, WAIT_FOR_BUS):
	    case ES (E_MSG_COMPLETE, CMD_SENDED):
	    case ES (E_MSG_COMPLETE, WAIT_FOR_ID):
	    case ES (E_MSG_DISCONN, WAIT_FOR_BUS):
	    case ES (E_MSG_DISCONN, STAT_OK):
	    case ES (E_MSG_DISCONN, WAIT_FOR_ID):
	    case ES (E_MSG_IDENTIFY, WAIT_FOR_BUS):
	    case ES (E_MSG_IDENTIFY, CMD_SENDED):
	    case ES (E_MSG_IDENTIFY, STAT_OK):

		/*  abort   */

		ncr->cmd = CMD_SET_ATN;
		ncr_msg = ABORT;    /*  message to send   */
		ncr->cmd = CMD_MESSAGE_ACCEPTED;

		ncr_state = WAIT_FOR_BUS;
		break;

	    case ES (E_MSG_DISCONN, DATA_OK):

		/*  scsi_io_task already stopped...  */

	    case ES (E_MSG_DISCONN, CMD_SENDED):

		/*  XXX: may be reject, if should not disconnect ???  */
		ncr->cmd = CMD_MESSAGE_ACCEPTED;

		push (cmd);
		cmd->cntl |= SC_DISCONNECTED;

		ncr_state = WAIT_FOR_FREE;
		break;

	    case ES (E_MSG_IGNORE, DATA_OK):

		/*  scsi_io_task already stopped...  */

	    case ES (E_MSG_IGNORE, WAIT_FOR_BUS):
	    case ES (E_MSG_IGNORE, CMD_SENDED):
	    case ES (E_MSG_IGNORE, STAT_OK):
	    case ES (E_MSG_IGNORE, WAIT_FOR_ID):

		ncr->cmd = CMD_MESSAGE_ACCEPTED;

		break;      /*  just ignore it...  */

	    case ES (E_MSG_IDENTIFY, WAIT_FOR_ID):

		ncr->cmd = CMD_MESSAGE_ACCEPTED;

		if (!(ncr->src_id & SRC_ID_VALID)) {
		    send_sys_msg (event, stat, aux_stat, "src_id not valid");
		    do_hard_reset();
		}
		else {
		    unsigned char lun = msg & 0x07;
		    unsigned char target = ncr->src_id & 0x07;

		    ncr_curr_cmd = pop (target, lun);

		    if (!ncr_curr_cmd) {
			send_sys_msg (event, stat, aux_stat,
						"bad reselect identify");
			do_hard_reset();
		    }
		    else {
			ncr->target = target;   /*  XXX:  paranoia ???   */

			ncr_state = CMD_SENDED;
			ncr_curr_cmd->cntl &= ~SC_DISCONNECTED;
		    }
		}
		break;

	    case ES (E_MSG_UNSUPP, DATA_OK):

		/*  scsi_io_task already stopped...  */

	    case ES (E_MSG_UNSUPP, WAIT_FOR_BUS):
	    case ES (E_MSG_UNSUPP, CMD_SENDED):
	    case ES (E_MSG_UNSUPP, STAT_OK):
	    case ES (E_MSG_UNSUPP, WAIT_FOR_ID):

		/*  reject   */

		ncr->cmd = CMD_SET_ATN;
		ncr_msg = MESSAGE_REJECT;   /*  message to send   */

		ncr->cmd = CMD_MESSAGE_ACCEPTED;

		ncr_state = WAIT_FOR_BUS;
		break;

	    case ES (E_TIMER_EXPIRED, TRY_SELECT):
		{   short i;

		    ncr->cmd = CMD_PAUSE;

		    i = 10000;
		    while (!(ncr->aux_stat & AUX_PAUSED) && i--) ;

		    if (i <= 0)  break;     /*  i.e., somewhat occured   */

		    do_hard_reset();
		}
		break;

	    case ES (E_TIMER_EXPIRED, NCR_FREE):

		break;      /*  just ignore it   */

	    case ES (E_TIMER_EXPIRED, DATA_OK):

		stop_scsi_io();

	    case ES (E_TIMER_EXPIRED, CMD_SENDED):
	    case ES (E_TIMER_EXPIRED, WAIT_FOR_BUS):
	    case ES (E_TIMER_EXPIRED, STAT_OK):

		end_scsi_cmd (cmd, RES_BAD_TARGET);

		/*  fall throw   */

	    case ES (E_TIMER_EXPIRED, WAIT_FOR_FREE):
	    case ES (E_TIMER_EXPIRED, WAIT_FOR_ID):

		send_sys_msg (event, stat, aux_stat, "timer expired");

		ncr->cmd = CMD_DISCONNECT;

		ncr_state = NCR_FREE;       /*  XXX: is it a good way ???  */
		wake_up (SCSI_TASK);
		break;

	    default:

		send_sys_msg (event, stat, aux_stat, "impossible state");
		panic ("stop");
		break;
	}

	aux_stat = ncr->aux_stat;
	stat = ncr->intr & 0x7f;
	if (stat != 0)  goto scsi_intr_restart;

	return;
}

void scsi_io_task (void) {
	volatile unsigned char *ncr_io_addr;
	unsigned short ncr_io_len;

	while (1) {

	    cli();

	    while (ncr_io == NCR_IO_FREE)
		    sleep_on();

	    ncr_io_addr = (unsigned char *) ncr_curr_cmd->addr;
	    ncr_io_len = ncr_curr_cmd->size;

	    ncr->cnt_h = 0;
	    ncr->cnt_m = (ncr_io_len >> 8) & 0xff;
	    ncr->cnt_l = ncr_io_len & 0xff;

	    if (ncr_curr_cmd->cntl & SC_BLOCK_XFER) {

		dma->chan[0].status = 0xff;
		dma->chan[0].oper_cntl =
			((ncr_io == NCR_IO_WRITE) ? OCNTL_MEM_TO_DEV
						  : OCNTL_DEV_TO_MEM) |
			OCNTL_OPER_BYTE | OCNTL_INT_MAX;

#define BLOCK_INCR      256

		ncr->cmd = CMD_TRANSFER_INFO | CMD_DMA_MODE;

		while (ncr_io_len > 0) {
		    dma->chan[0].count = BLOCK_INCR;
		    /* XXX: omit it ? */
		    dma->chan[0].mem_addr = (int) ncr_io_addr;

		    /*  ...and the bus go away...  */
		    dma->chan[0].cntl = CCNTL_START;

		    /*  XXX:  paranoia for  OCNTL_INT_MAX  ???  */
		    while (!(dma->chan[0].status & STAT_OPER_COMPLETED)) ;

#if 0
		    if (ncr->cmd) ;
#endif

		    dma->chan[0].status = 0xff;

		    sti();
		    cli();
		    if (ncr_io == NCR_IO_FREE)  break;

		    ncr_io_addr += BLOCK_INCR;
		    ncr_io_len -= BLOCK_INCR;
		}

		sti();

	    } else {

		ncr->cmd = CMD_TRANSFER_INFO;

		sti();

		if (ncr_io == NCR_IO_WRITE) {
		    __asm volatile ("bra    1f
			      2: mov.b  (%0)+,(%3)
			      1: mov.b  (%2),%%d0
				 bpl.b  2b
				 tst.w  %4
				 beq.b  3f
				 btst   &1,%%d0
				 beq.b  1b
			      3:"
				: "=a" (ncr_io_addr)
				: "0" (ncr_io_addr), "a" (&ncr->aux_stat),
				  "a" (&ncr->data), "m" (ncr_io)
				: "%d0", "memory"
		    );
		} else {
		    __asm volatile ("bra    1f
			      2: mov.b  (%3),(%0)+
			      1: mov.b  (%2),%%d0
				 bmi.b  2b
				 tst.w  %4
				 beq.b  3f
				 btst   &1,%%d0
				 beq.b  1b
			      3:"
				: "=a" (ncr_io_addr)
				: "0" (ncr_io_addr), "a" (&ncr->aux_stat),
				  "a" (&ncr->data), "m" (ncr_io)
				: "%d0", "memory"
		    );

		}
	    }

	    ncr_io = NCR_IO_FREE;       /*  XXX:  be careful...  */
	}

	/*  not reached   */
}


