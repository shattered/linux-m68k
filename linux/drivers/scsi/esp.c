/* esp.c:  EnhancedScsiProcessor Sun SCSI driver code.
 *
 * Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 *
 * Ugly generalization hacks by Jesper Skov.
 */

/* TODO:
 *
 * 1) Maybe disable parity checking in config register one for SCSI1
 *    targets.
 * 2) Add support for DMA2 pipelining.
 * 3) Add tagged queueing.
 */

#ifdef MODULE
#include <linux/module.h>
#endif

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/malloc.h>
#include <linux/blk.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>

#include "scsi.h"
#include "hosts.h"
#include "esp.h"

#ifdef CONFIG_CYBERSTORM_SCSI
#include "cyber_esp.h"
#endif

#ifdef CONFIG_BLZ2060_SCSI
#include "blz2060_esp.h"
#endif

#ifdef CONFIG_FERRET_SCSI
#include "ferret_esp.h"
#endif

#ifdef CONFIG_SCSI_SUNESP
#include "sparc_esp.h"
#include <asm/sbus.h>
#include <asm/dma.h>
#include <asm/machines.h>
#include <asm/oplib.h>
#include <asm/vaddrs.h>
#endif

#if defined(CONFIG_AMIGA)
#define SYMBIOS_HACK
#endif

#include <asm/system.h>
#include <asm/ptrace.h>
#include <asm/pgtable.h>
#include <asm/io.h>

/* Command phase enumeration. */
enum {
	not_issued    = 0x00,  /* Still in the issue_SC queue.          */

	/* Various forms of selecting a target. */
#define in_slct_mask    0x10
	in_slct_norm  = 0x10,  /* ESP is arbitrating, normal selection  */
	in_slct_stop  = 0x11,  /* ESP will select, then stop with IRQ   */
	in_slct_msg   = 0x12,  /* select, then send a message           */
	in_slct_tag   = 0x13,  /* select and send tagged queue msg      */
	in_slct_sneg  = 0x14,  /* select and acquire sync capabilities  */

	/* Any post selection activity. */
#define in_phases_mask  0x20
	in_datain     = 0x20,  /* Data is transferring from the bus     */
	in_dataout    = 0x21,  /* Data is transferring to the bus       */
	in_data_done  = 0x22,  /* Last DMA data operation done (maybe)  */
	in_msgin      = 0x23,  /* Eating message from target            */
	in_msgincont  = 0x24,  /* Eating more msg bytes from target     */
	in_msgindone  = 0x25,  /* Deciding to do with what we were sent */
	in_msgout     = 0x26,  /* Sending message to target             */
	in_msgoutdone = 0x27,  /* Done sending msg out                  */
	in_cmdbegin   = 0x28,  /* Sending cmd after abnormal selection  */
	in_cmdend     = 0x29,  /* Done sending slow cmd                 */
	in_status     = 0x2a,  /* Was in status phase, finishing cmd    */
	in_freeing    = 0x2b,  /* freeing the bus for cmd cmplt or disc */
	in_the_dark   = 0x2c,  /* Don't know what bus phase we are in   */

	/* Special states, ie. not normal bus transitions... */
#define in_spec_mask    0x80
	in_abortone   = 0x80,  /* Aborting one command currently        */
	in_abortall   = 0x81,  /* Blowing away all commands we have     */
	in_resetdev   = 0x82,  /* SCSI target reset in progress         */
	in_resetbus   = 0x83,  /* SCSI bus reset in progress            */
	in_tgterror   = 0x84,  /* Target did something stupid           */
};

struct proc_dir_entry proc_scsi_esp = {
	PROC_SCSI_ESP, 3, "esp",
	S_IFDIR | S_IRUGO | S_IXUGO, 2
};

struct Sparc_ESP *espchain = 0;
int nesps = 0, esps_in_use = 0, esps_running = 0;

void esp_intr(int irq, void *dev_id, struct pt_regs *pregs);
static void esp_done(struct Sparc_ESP *esp, int error);

/* Debugging routines */
struct esp_cmdstrings {
	unchar cmdchar;
	char *text;
} esp_cmd_strings[] = {
	/* Miscellaneous */
	{ ESP_CMD_NULL, "ESP_NOP", },
	{ ESP_CMD_FLUSH, "FIFO_FLUSH", },
	{ ESP_CMD_RC, "RSTESP", },
	{ ESP_CMD_RS, "RSTSCSI", },
	/* Disconnected State Group */
	{ ESP_CMD_RSEL, "RESLCTSEQ", },
	{ ESP_CMD_SEL, "SLCTNATN", },
	{ ESP_CMD_SELA, "SLCTATN", },
	{ ESP_CMD_SELAS, "SLCTATNSTOP", },
	{ ESP_CMD_ESEL, "ENSLCTRESEL", },
	{ ESP_CMD_DSEL, "DISSELRESEL", },
	{ ESP_CMD_SA3, "SLCTATN3", },
	{ ESP_CMD_RSEL3, "RESLCTSEQ", },
	/* Target State Group */
	{ ESP_CMD_SMSG, "SNDMSG", },
	{ ESP_CMD_SSTAT, "SNDSTATUS", },
	{ ESP_CMD_SDATA, "SNDDATA", },
	{ ESP_CMD_DSEQ, "DISCSEQ", },
	{ ESP_CMD_TSEQ, "TERMSEQ", },
	{ ESP_CMD_TCCSEQ, "TRGTCMDCOMPSEQ", },
	{ ESP_CMD_DCNCT, "DISC", },
	{ ESP_CMD_RMSG, "RCVMSG", },
	{ ESP_CMD_RCMD, "RCVCMD", },
	{ ESP_CMD_RDATA, "RCVDATA", },
	{ ESP_CMD_RCSEQ, "RCVCMDSEQ", },
	/* Initiator State Group */
	{ ESP_CMD_TI, "TRANSINFO", },
	{ ESP_CMD_ICCSEQ, "INICMDSEQCOMP", },
	{ ESP_CMD_MOK, "MSGACCEPTED", },
	{ ESP_CMD_TPAD, "TPAD", },
	{ ESP_CMD_SATN, "SATN", },
	{ ESP_CMD_RATN, "RATN", },
};
#define NUM_ESP_COMMANDS  ((sizeof(esp_cmd_strings)) / (sizeof(struct esp_cmdstrings)))

/* Print textual representation of an ESP command */
static inline void esp_print_cmd(unchar espcmd)
{
	unchar dma_bit = espcmd & ESP_CMD_DMA;
	int i;

	espcmd &= ~dma_bit;
	for(i=0; i<NUM_ESP_COMMANDS; i++)
		if(esp_cmd_strings[i].cmdchar == espcmd)
			break;
	if(i==NUM_ESP_COMMANDS)
		printk("ESP_Unknown");
	else
		printk("%s%s", esp_cmd_strings[i].text,
		       ((dma_bit) ? "+DMA" : ""));
}

/* Print the status register's value */
static inline void esp_print_statreg(unchar statreg)
{
	unchar phase;

	printk("STATUS<");
	phase = statreg & ESP_STAT_PMASK;
	printk("%s,", (phase == ESP_DOP ? "DATA-OUT" :
		       (phase == ESP_DIP ? "DATA-IN" :
			(phase == ESP_CMDP ? "COMMAND" :
			 (phase == ESP_STATP ? "STATUS" :
			  (phase == ESP_MOP ? "MSG-OUT" :
			   (phase == ESP_MIP ? "MSG_IN" :
			    "unknown")))))));
	if(statreg & ESP_STAT_TDONE)
		printk("TRANS_DONE,");
	if(statreg & ESP_STAT_TCNT)
		printk("TCOUNT_ZERO,");
	if(statreg & ESP_STAT_PERR)
		printk("P_ERROR,");
	if(statreg & ESP_STAT_SPAM)
		printk("SPAM,");
	if(statreg & ESP_STAT_INTR)
		printk("IRQ,");
	printk(">");
}

/* Print the interrupt register's value */
static inline void esp_print_ireg(unchar intreg)
{
	printk("INTREG< ");
	if(intreg & ESP_INTR_S)
		printk("SLCT_NATN ");
	if(intreg & ESP_INTR_SATN)
		printk("SLCT_ATN ");
	if(intreg & ESP_INTR_RSEL)
		printk("RSLCT ");
	if(intreg & ESP_INTR_FDONE)
		printk("FDONE ");
	if(intreg & ESP_INTR_BSERV)
		printk("BSERV ");
	if(intreg & ESP_INTR_DC)
		printk("DISCNCT ");
	if(intreg & ESP_INTR_IC)
		printk("ILL_CMD ");
	if(intreg & ESP_INTR_SR)
		printk("SCSI_BUS_RESET ");
	printk(">");
}

/* Print the sequence step registers contents */
static inline void esp_print_seqreg(unchar stepreg)
{
	stepreg &= ESP_STEP_VBITS;
	printk("STEP<%s>",
	       (stepreg == ESP_STEP_ASEL ? "SLCT_ARB_CMPLT" :
		(stepreg == ESP_STEP_SID ? "1BYTE_MSG_SENT" :
		 (stepreg == ESP_STEP_NCMD ? "NOT_IN_CMD_PHASE" :
		  (stepreg == ESP_STEP_PPC ? "CMD_BYTES_LOST" :
		   (stepreg == ESP_STEP_FINI4 ? "CMD_SENT_OK" :
		    "UNKNOWN"))))));
}

#ifdef DEBUG_ESP
static char *phase_string(int phase)
{
	switch(phase) {
	case not_issued:
		return "UNISSUED";
	case in_slct_norm:
		return "SLCTNORM";
	case in_slct_stop:
		return "SLCTSTOP";
	case in_slct_msg:
		return "SLCTMSG";
	case in_slct_tag:
		return "SLCTTAG";
	case in_slct_sneg:
		return "SLCTSNEG";
	case in_datain:
		return "DATAIN";
	case in_dataout:
		return "DATAOUT";
	case in_data_done:
		return "DATADONE";
	case in_msgin:
		return "MSGIN";
	case in_msgincont:
		return "MSGINCONT";
	case in_msgindone:
		return "MSGINDONE";
	case in_msgout:
		return "MSGOUT";
	case in_msgoutdone:
		return "MSGOUTDONE";
	case in_cmdbegin:
		return "CMDBEGIN";
	case in_cmdend:
		return "CMDEND";
	case in_status:
		return "STATUS";
	case in_freeing:
		return "FREEING";
	case in_the_dark:
		return "CLUELESS";
	case in_abortone:
		return "ABORTONE";
	case in_abortall:
		return "ABORTALL";
	case in_resetdev:
		return "RESETDEV";
	case in_resetbus:
		return "RESETBUS";
	case in_tgterror:
		return "TGTERROR";
	default:
		return "UNKNOWN";
	};
}
#endif

static inline void esp_advance_phase(Scsi_Cmnd *s, int newphase)
{
#ifdef DEBUG_STATE_MACHINE
	ESPLOG(("<%s>", phase_string(newphase)));
#endif
	s->SCp.sent_command = s->SCp.phase;
	s->SCp.phase = newphase;
}

extern inline void esp_cmd(struct Sparc_ESP *esp, struct ESP_regs *eregs,
			   unchar cmd)
{
#ifdef DEBUG_ESP_CMDS
	esp->espcmdlog[esp->espcmdent] = cmd;
	esp->espcmdent = (esp->espcmdent + 1) & 31;
#endif
	eregs->esp_cmd = cmd;
}

/* How we use the various Linux SCSI data structures for operation.
 *
 * struct scsi_cmnd:
 *
 *   We keep track of the syncronous capabilities of a target
 *   in the device member, using sync_min_period and
 *   sync_max_offset.  These are the values we directly write
 *   into the ESP registers while running a command.  If offset
 *   is zero the ESP will use asynchronous transfers.
 *   If the borken flag is set we assume we shouldn't even bother
 *   trying to negotiate for synchronous transfer as this target
 *   is really stupid.  If we notice the target is dropping the
 *   bus, and we have been allowing it to disconnect, we clear
 *   the disconnect flag.
 *
 *   XXX When and how to first set the disconnect flag initially? XXX
 *
 *   XXX Tagged commands?  Linked commands, if at all? XXX
 *
 *   The scsi_pointer member of a command structure is used for a few
 *   things.  We hold the scatter list, if any, here first of all.  Since
 *   we may need to virtually remap some transfers to different addresses
 *   we keep track of this in the dvma_address member per scatter, before
 *   we call the completion routine we call the mmu specific unmapping
 *   routine which is expected to set all the dvma_address fields to NULL
 *   so that the target driver does not get confused.
 *   As expected the Status and Message members of the scsi_pointer struct
 *   hold the information few to us by the target when a command completes.
 *   The have_data_in field is used as our "save/restore pointers" save area.
 *   To make scatter and non-scatter work correctly, this value is said
 *   to be the logic offset from the beggining of this data transfer.  So
 *   if for example, the target disconnects (legally) in the middle of
 *   the third scatter list element we will save have_data_in to be the
 *   (length_of_scatter_element1 + length_of_scatter_element2 +
 *    how_far_we_got_in_scatter_element3).  This works out perfectly because
 *   this is what the numbers mean when the target gives us a modify data
 *   pointers message.
 *   The phase keeps track of the state of the ESP driver for this particular
 *   command.  We hold the previous phase in the sent_command field.
 */


/* Manipulation of the ESP command queues.  Thanks to the aha152x driver
 * and its author, Juergen E. Fischer, for the methods used here.
 * Note that these are per-ESP queues, not global queues like
 * the aha152x driver uses.
 */
static inline void append_SC(Scsi_Cmnd **SC, Scsi_Cmnd *new_SC)
{
	Scsi_Cmnd *end;
	unsigned long flags;

	save_flags(flags); cli();
	new_SC->host_scribble = (unsigned char *) NULL;
	if(!*SC)
		*SC = new_SC;
	else {
		for(end=*SC;end->host_scribble;end=(Scsi_Cmnd *)end->host_scribble)
			;
		end->host_scribble = (unsigned char *) new_SC;
	}
	restore_flags(flags);
}

static inline void prepend_SC(Scsi_Cmnd **SC, Scsi_Cmnd *new_SC)
{
	unsigned long flags;

	save_flags(flags); cli();
	new_SC->host_scribble = (unsigned char *) *SC;
	*SC = new_SC;
	restore_flags(flags);
}

static inline Scsi_Cmnd *remove_first_SC(Scsi_Cmnd **SC)
{
	Scsi_Cmnd *ptr;
	unsigned long flags;

	save_flags(flags); cli();
	ptr = *SC;
	if(ptr)
		*SC = (Scsi_Cmnd *) (*SC)->host_scribble;
	restore_flags(flags);
	return ptr;
}

static inline Scsi_Cmnd *remove_SC(Scsi_Cmnd **SC, int target, int lun)
{
	Scsi_Cmnd *ptr, *prev;
	unsigned long flags;

	save_flags(flags); cli();
	for(ptr = *SC, prev = NULL;
	    ptr && ((ptr->target != target) || (ptr->lun != lun));
	    prev = ptr, ptr = (Scsi_Cmnd *) ptr->host_scribble)
		;
	if(ptr) {
		if(prev)
			prev->host_scribble=ptr->host_scribble;
		else
			*SC=(Scsi_Cmnd *)ptr->host_scribble;
	}
	restore_flags(flags);
	return ptr;
}

/* Resetting various pieces of the ESP scsi driver chipset */

/* Reset the ESP chip, _not_ the SCSI bus. */
static inline void esp_reset_esp(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	int family_code, version, i;
	volatile int trash;

	/* Now reset the ESP chip */

	esp_cmd(esp, eregs, ESP_CMD_RC);
	esp_cmd(esp, eregs, ESP_CMD_NULL | ESP_CMD_DMA);
	esp_cmd(esp, eregs, ESP_CMD_NULL | ESP_CMD_DMA);

	/* Reload the configuration registers */
	eregs->esp_cfact = esp->cfact;
	eregs->esp_stp   = 0;
	eregs->esp_soff  = 0;
	eregs->esp_timeo = esp->neg_defp;

	/* This is the only point at which it is reliable to read
	 * the ID-code for a fast ESP chip variant.
	 */
	if(esp->erev == fast) {
		version = eregs->esp_uid;
		family_code = (version & 0xf8) >> 3;
#ifdef SYMBIOS_HACK
		if (version == 0 && family_code == 0)
		{
			printk ("Detected SymBIOS chip with no family code.\n");
			version = 3;
			family_code = 2;
		}
#endif
		if(family_code != 2)
			esp->erev = fas100a;
		else
			esp->erev = fas236;
		printk("esp%d: FAST chip is %s (family=%d, version=%d)\n",
		       esp->esp_id,
		       (esp->erev == fas236) ? "fas236" : "fas100a",
		       family_code, (version & 7));


		esp->max_period = ((35 * esp->ccycle) / 1000);
		esp->min_period = ((4 * esp->ccycle) / 1000);
	} else {
		esp->max_period = ((35 * esp->ccycle) / 1000);
		esp->min_period = ((5 * esp->ccycle) / 1000);
	}
	esp->max_period = (esp->max_period + 3)>>2;
	esp->min_period = (esp->min_period + 3)>>2;

	eregs->esp_cfg1  = esp->config1;
	switch(esp->erev) {
	case esp100:
		/* nothing to do */
		break;
	case esp100a:
		eregs->esp_cfg2 = esp->config2;
		break;
	case esp236:
		/* Slow 236 */
		eregs->esp_cfg2 = esp->config2;
		eregs->esp_cfg3 = esp->config3[0];
		break;
	case fas236:
		/* Fast 236 */
		eregs->esp_cfg2 = esp->config2;
		for(i=0; i<8; i++)
			esp->config3[i] |= ESP_CONFIG3_FCLK;
		eregs->esp_cfg3 = esp->config3[0];
		if(esp->diff)
			esp->radelay = 0;
		else
			esp->radelay = 96;
		break;
	case fas100a:
		/* Fast 100a */
		eregs->esp_cfg2 = esp->config2;
		for(i=0; i<8; i++)
			esp->config3[i] |= ESP_CONFIG3_FCLOCK;
		eregs->esp_cfg3 = esp->config3[0];
		esp->radelay = 32;
		break;
	default:
		panic("esp: what could it be... I wonder...");
		break;
	};

	/* Eat any bitrot in the chip */
	trash = eregs->esp_intrpt;
}

/* This places the ESP into a known state at boot time. */
inline void esp_bootup_reset(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	volatile unchar trash;

	/* Reset the DMA */
	if(esp->dma_reset)
		esp->dma_reset(esp);

	/* Reset the ESP */
	esp_reset_esp(esp, eregs);

	/* Reset the SCSI bus, but tell ESP not to generate an irq */
	eregs->esp_cfg1 |= ESP_CONFIG1_SRRDISAB;
	esp_cmd(esp, eregs, ESP_CMD_RS);
	udelay(400);
	eregs->esp_cfg1 = esp->config1;

	/* Eat any bitrot in the chip and we are done... */
	trash = eregs->esp_intrpt;
}

/* Allocate structure and insert basic data such as SCSI chip frequency
 * data and a pointer to the device
 */
struct Sparc_ESP* esp_allocate(Scsi_Host_Template *tpnt, void *esp_dev)
{
	struct Sparc_ESP *esp, *elink;
	struct Scsi_Host *esp_host;

	esp_host = scsi_register(tpnt, sizeof(struct Sparc_ESP));
	if(!esp_host)
		panic("Cannot register ESP SCSI host");
	esp = (struct Sparc_ESP *) esp_host->hostdata;
	if(!esp)
		panic("No esp in hostdata");
	esp->ehost = esp_host;
	esp->edev = esp_dev;
	esp->esp_id = nesps++;
	/* Put into the chain of esp chips detected */
	if(espchain) {
		elink = espchain;
		while(elink->next) elink = elink->next;
		elink->next = esp;
	} else {
		espchain = esp;
	}
	esp->next = 0;

	return esp;
}

/* Complete initialization of ESP structure and device
 * Caller must have initialized appropriate parts of the ESP structure
 * between the call to esp_allocate and this function.
 */
void esp_initialize(struct Sparc_ESP *esp)
{
	struct ESP_regs *eregs = esp->eregs;
	unsigned int fmhz;
	unchar ccf;
	int i;
	
	/* Check out the clock properties of the chip. */

	/* This is getting messy but it has to be done
	 * correctly or else you get weird behavior all
	 * over the place.  We are trying to basically
	 * figure out three pieces of information.
	 *
	 * a) Clock Conversion Factor
	 *
	 *    This is a representation of the input
	 *    crystal clock frequency going into the
	 *    ESP on this machine.  Any operation whose
	 *    timing is longer than 400ns depends on this
	 *    value being correct.  For example, you'll
	 *    get blips for arbitration/selection during
	 *    high load or with multiple targets if this
	 *    is not set correctly.
	 *
	 * b) Selection Time-Out
	 *
	 *    The ESP isn't very bright and will arbitrate
	 *    for the bus and try to select a target
	 *    forever if you let it.  This value tells
	 *    the ESP when it has taken too long to
	 *    negotiate and that it should interrupt
	 *    the CPU so we can see what happened.
	 *    The value is computed as follows (from
	 *    NCR/Symbios chip docs).
	 *
	 *          (Time Out Period) *  (Input Clock)
	 *    STO = ----------------------------------
	 *          (8192) * (Clock Conversion Factor)
	 *
	 *    You usually want the time out period to be
	 *    around 250ms, I think we'll set it a little
	 *    bit higher to account for fully loaded SCSI
	 *    bus's and slow devices that don't respond so
	 *    quickly to selection attempts. (yeah, I know
	 *    this is out of spec. but there is a lot of
	 *    buggy pieces of firmware out there so bite me)
	 *
	 * c) Imperical constants for synchronous offset
	 *    and transfer period register values
	 *
	 *    This entails the smallest and largest sync
	 *    period we could ever handle on this ESP.
	 */
	
	fmhz = esp->cfreq;

	if(fmhz <= (5000000))
		ccf = 0;
	else
		ccf = (((5000000 - 1) + (fmhz))/(5000000));
	if(!ccf || ccf > 8) {
		/* If we can't find anything reasonable,
		 * just assume 20MHZ.  This is the clock
		 * frequency of the older sun4c's where I've
		 * been unable to find the clock-frequency
		 * PROM property.  All other machines provide
		 * useful values it seems.
		 */
		ccf = ESP_CCF_F4;
		fmhz = (20000000);
	}
	if(ccf==(ESP_CCF_F7+1))
		esp->cfact = ESP_CCF_F0;
	else if(ccf == ESP_CCF_NEVER)
		esp->cfact = ESP_CCF_F2;
	else
		esp->cfact = ccf;
	esp->cfreq = fmhz;
	esp->ccf = ccf;
	esp->ccycle = ESP_MHZ_TO_CYCLE(fmhz);
	esp->ctick = ESP_TICK(ccf, esp->ccycle);
	esp->neg_defp = ESP_NEG_DEFP(fmhz, ccf);
	esp->sync_defp = SYNC_DEFP_SLOW;


	/* Fill in ehost data */
	esp->ehost->base = (unsigned char *) eregs;
	esp->ehost->io_port = (unsigned int) eregs;
	esp->ehost->this_id = esp->scsi_id;
	esp->ehost->irq = esp->irq;

	/* SCSI id mask */
	esp->scsi_id_mask = (1 << esp->scsi_id);

	/* Probe the revision of this esp */
	esp->config1 = (ESP_CONFIG1_PENABLE | (esp->scsi_id & 7));
	esp->config2 = (ESP_CONFIG2_SCSI2ENAB | ESP_CONFIG2_REGPARITY);
	eregs->esp_cfg2 = esp->config2;
#ifndef SYMBIOS_HACK
	if((eregs->esp_cfg2 & ~(ESP_CONFIG2_MAGIC)) !=
	   (ESP_CONFIG2_SCSI2ENAB | ESP_CONFIG2_REGPARITY)) {
		printk("NCR53C90(esp100) detected\n");
		esp->erev = esp100;
	} else {
#endif
		eregs->esp_cfg2 = esp->config2 = 0;
		eregs->esp_cfg3 = 0;
		eregs->esp_cfg3 = esp->config3[0] = 5;
#ifndef SYMBIOS_HACK
		if(eregs->esp_cfg3 != 5) {
			printk("NCR53C90A(esp100a) detected\n");
			esp->erev = esp100a;
		} else {
#else
		{
#endif
			int target;
			
			for(target=0; target<8; target++)
				esp->config3[target] = 0;
			eregs->esp_cfg3 = 0;
#ifndef SYMBIOS_HACK
			if((esp->ccf) > ESP_CCF_F5) {
#endif
				printk("NCR53C9XF(espfast) detected\n");
				esp->erev = fast;
				eregs->esp_cfg2 = esp->config2 = 0;
				esp->sync_defp = SYNC_DEFP_FAST;
#ifndef SYMBIOS_HACK
			} else {
				printk("NCR53C9x(esp236) detected\n");
				esp->erev = esp236;
				eregs->esp_cfg2 = esp->config2 = 0;
			}
		}
#endif
	}				
	
	/* Initialize the command queues */
	esp->current_SC = 0;
	esp->disconnected_SC = 0;
	esp->issue_SC = 0;

	/* Clear the state machines. */
	esp->targets_present = 0;
	esp->being_reselected = 0;
	esp->resetting_bus = 0;
	esp->aborting_cmd = 0;
	esp->aborting_all = 0;
	esp->snip = 0;
	esp->targets_present = 0;
	for(i = 0; i < 32; i++)
		esp->espcmdlog[i] = 0;
	esp->espcmdent = 0;
	for(i = 0; i < 16; i++) {
		esp->cur_msgout[i] = 0;
		esp->cur_msgin[i] = 0;
	}
	esp->prevmsgout = esp->prevmsgin = 0;
	esp->msgout_len = esp->msgin_len = 0;
	
	/* Reset the thing before we try anything... */
	esp_bootup_reset(esp, eregs);
	
	esps_in_use++;

	printk("SCSI ID %d  Clock %d MHz CCF=%d Time-Out %d ",
	       esp->scsi_id, (esp->cfreq / 1000000),
	       esp->ccf, (int) esp->neg_defp);

#ifdef THREADED_ESP_DRIVER
	kernel_thread(esp_kernel_thread, esp, 0);
#endif
}

/*
 * The info function will return whatever useful
 * information the developer sees fit.  If not provided, then
 * the name field will be used instead.
 */
const char *esp_info(struct Scsi_Host *host)
{
	struct Sparc_ESP *esp;

	esp = (struct Sparc_ESP *) host->hostdata;
	switch(esp->erev) {
	case esp100:
		return "Sparc ESP100 (NCR53C90)";
	case esp100a:
		return "Sparc ESP100A (NCR53C90A)";
	case esp236:
		return "Sparc ESP236";
	case fas236:
		return "Sparc ESP236-FAST";
	case fas100a:
		return "Sparc ESP100A-FAST";
	default:
		panic("Bogon ESP revision");
	};
}

/* Some rules:
 *
 *   1) Never ever panic while something is live on the bus.
 *      If there is to be any chance of syncing the disks this
 *      rule is to be obeyed.
 *
 *   2) Any target that causes a foul condition will no longer
 *      have synchronous transfers done to it, no questions
 *      asked.
 *
 *   3) Keep register accesses to a minimum.  Think about some
 *      day when we have Xbus machines this is running on and
 *      the ESP chip is on the other end of the machine on a
 *      different board from the cpu where this is running.
 */

/* Fire off a command.  We assume the bus is free and that the only
 * case where we could see an interrupt is where we have disconnected
 * commands active and they are trying to reselect us.
 */
static inline void esp_check_cmd(struct Sparc_ESP *esp, Scsi_Cmnd *sp)
{
	switch(sp->cmd_len) {
	case 6:
	case 10:
	case 12:
		esp->esp_slowcmd = 0;
		break;

	default:
		esp->esp_slowcmd = 1;
		esp->esp_scmdleft = sp->cmd_len;
		esp->esp_scmdp = &sp->cmnd[0];
		break;
	};
}

static inline void build_sync_nego_msg(struct Sparc_ESP *esp, int period, int offset)
{
	esp->cur_msgout[0] = EXTENDED_MESSAGE;
	esp->cur_msgout[1] = 3;
	esp->cur_msgout[2] = EXTENDED_SDTR;
	esp->cur_msgout[3] = period;
	esp->cur_msgout[4] = offset;
	esp->msgout_len = 5;
}

static inline void esp_exec_cmd(struct Sparc_ESP *esp)
{
	struct ESP_regs *eregs = esp->eregs;
	Scsi_Cmnd *SCptr;
	Scsi_Device *SDptr;
	volatile unchar *cmdp = esp->esp_command;
	unsigned char the_esp_command;
	int lun, target;
	int i;

	/* Try to save the register access. */
	if(esp->disconnected_SC && (esp->dma_irq_p(esp)))
		return; /* Hold off, we've been reselected. */

	/* Grab first member of the issue queue. */
	SCptr = esp->current_SC = remove_first_SC(&esp->issue_SC);

	/* Safe to panic here because current_SC is null. */
	if(!SCptr)
		panic("esp: esp_exec_cmd and issue queue is NULL");

	SDptr = SCptr->device;
	lun = SCptr->lun;
	target = SCptr->target;

	esp->snip = 0;
	esp->msgout_len = 0;

	/* Send it out whole, or piece by piece?   The ESP
	 * only knows how to automatically send out 6, 10,
	 * and 12 byte commands.  I used to think that the
	 * Linux SCSI code would never throw anything other
	 * than that to us, but then again there is the
	 * SCSI generic driver which can send us anything.
	 */
	esp_check_cmd(esp, SCptr);

	/* If arbitration/selection is successful, the ESP will leave
	 * ATN asserted, causing the target to go into message out
	 * phase.  The ESP will feed the target the identify and then
	 * the target can only legally go to one of command,
	 * datain/out, status, or message in phase, or stay in message
	 * out phase (should we be trying to send a sync negotiation
	 * message after the identify).  It is not allowed to drop
	 * BSY, but some buggy targets do and we check for this
	 * condition in the selection complete code.  Most of the time
	 * we'll make the command bytes available to the ESP and it
	 * will not interrupt us until it finishes command phase, we
	 * cannot do this for command sizes the ESP does not
	 * understand and in this case we'll get interrupted right
	 * when the target goes into command phase.
	 *
	 * It is absolutely _illegal_ in the presence of SCSI-2 devices
	 * to use the ESP select w/o ATN command.  When SCSI-2 devices are
	 * present on the bus we _must_ always go straight to message out
	 * phase with an identify message for the target.  Being that
	 * selection attempts in SCSI-1 w/o ATN was an option, doing SCSI-2
	 * selections should not confuse SCSI-1 we hope.
	 */

	if(SDptr->sync) {
		/* this targets sync is known */
		if(SDptr->disconnect)
			*cmdp++ = IDENTIFY(1, lun);
		else
			*cmdp++ = IDENTIFY(0, lun);

		if(esp->esp_slowcmd) {
			the_esp_command = (ESP_CMD_SELAS | ESP_CMD_DMA);
			esp_advance_phase(SCptr, in_slct_stop);
		} else {
			the_esp_command = (ESP_CMD_SELA | ESP_CMD_DMA);
			esp_advance_phase(SCptr, in_slct_norm);
		}
	} else if(!(esp->targets_present & (1<<target)) || !(SDptr->disconnect)) {
		/* After the bootup SCSI code sends both the
		 * TEST_UNIT_READY and INQUIRY commands we want
		 * to at least attempt allowing the device to
		 * disconnect.
		 */
		ESPMISC(("esp: Selecting device for first time. target=%d "
			 "lun=%d\n", target, SCptr->lun));
		if(!SDptr->borken && !SDptr->disconnect)
			SDptr->disconnect = 1;

		*cmdp++ = IDENTIFY(0, lun);
		esp->prevmsgout = NOP;
		esp_advance_phase(SCptr, in_slct_norm);
		the_esp_command = (ESP_CMD_SELA | ESP_CMD_DMA);

		/* Take no chances... */
		SDptr->sync_max_offset = 0;
		SDptr->sync_min_period = 0;
	} else {
		int toshiba_cdrom_hwbug_wkaround = 0;

		/* We've talked to this guy before,
		 * but never negotiated.. lets try
		 */

		if(!SDptr->borken) {
			if((SDptr->type == TYPE_ROM) &&
			   (!strncmp(SDptr->vendor, "TOSHIBA", 7))) {
				/* Nice try sucker... */
				printk("esp%d: Disabling sync for buggy "
				       "Toshiba CDROM.\n", esp->esp_id);
				toshiba_cdrom_hwbug_wkaround = 1;
				build_sync_nego_msg(esp, 0, 0);
			} else {
				build_sync_nego_msg(esp, esp->sync_defp, 15);
			}
		} else {
			build_sync_nego_msg(esp, 0, 0);
		}
		SDptr->sync = 1;
		esp->snip = 1;

		/* A fix for broken SCSI1 targets, when they disconnect
		 * they lock up the bus and confuse ESP.  So disallow
		 * disconnects for SCSI1 targets for now until we
		 * find a better fix.
		 *
		 * Addendum: This is funny, I figured out what was going
		 *           on.  The blotzed SCSI1 target would disconnect,
		 *           one of the other SCSI2 targets or both would be
		 *           disconnected as well.  The SCSI1 target would
		 *           stay disconnected long enough that we start
		 *           up a command on one of the SCSI2 targets.  As
		 *           the ESP is arbitrating for the bus the SCSI1
		 *           target begins to arbitrate as well to reselect
		 *           the ESP.  The SCSI1 target refuses to drop it's
		 *           ID bit on the data bus even though the ESP is
		 *           at ID 7 and is the obvious winner for any
		 *           arbitration.  The ESP is a poor sport and refuses
		 *           to lose arbitration, it will continue indefinately
		 *           trying to arbitrate for the bus and can only be
		 *           stopped via a chip reset or SCSI bus reset.
		 *           Therefore _no_ disconnects for SCSI1 targets
		 *           thank you very much. ;-)
		 */
		if((SDptr->scsi_level < SCSI_2) || toshiba_cdrom_hwbug_wkaround ||
		   SDptr->borken) {
			printk("esp%d: Disabling DISCONNECT for target %d lun %d\n",
			       esp->esp_id, SCptr->target, SCptr->lun);
			SDptr->disconnect = 0;
			*cmdp++ = IDENTIFY(0, lun);
		} else {
			*cmdp++ = IDENTIFY(1, lun);
		}

		/* ESP fifo is only so big...
		 * Make this look like a slow command.
		 */
		esp->esp_slowcmd = 1;
		esp->esp_scmdleft = SCptr->cmd_len;
		esp->esp_scmdp = &SCptr->cmnd[0];

		the_esp_command = (ESP_CMD_SELAS | ESP_CMD_DMA);
		esp_advance_phase(SCptr, in_slct_msg);
	}

	if(!esp->esp_slowcmd)
		for(i = 0; i < SCptr->cmd_len; i++)
			*cmdp++ = SCptr->cmnd[i];

	eregs->esp_busid = (target & 7);
	eregs->esp_soff = SDptr->sync_max_offset;
	eregs->esp_stp  = SDptr->sync_min_period;
	if(esp->erev > esp100a)
		eregs->esp_cfg3 = esp->config3[target];

	i = (cmdp - esp->esp_command);

	/* Set up the DMA and ESP counters */
	if(esp->do_pio_cmds){
		int j = 0;

		for(;j<i;j++)
			eregs->esp_fdata = esp->esp_command[j];
		the_esp_command &= ~ESP_CMD_DMA;
	} else {
		eregs->esp_tclow = i;
		eregs->esp_tcmed = 0;
		esp->dma_init_write(esp, (char *) esp->esp_command, i);
	}

	/* Tell ESP to "go". */
	esp_cmd(esp, eregs, the_esp_command);
}

/* Queue a SCSI command delivered from the mid-level Linux SCSI code. */
int esp_queue(Scsi_Cmnd *SCpnt, void (*done)(Scsi_Cmnd *))
{
	struct Sparc_ESP *esp;
	int don;
	unsigned long flags;

	save_flags(flags); cli();

	/* Set up func ptr and initial driver cmd-phase. */
	SCpnt->scsi_done = done;
	SCpnt->SCp.phase = not_issued;

	esp = (struct Sparc_ESP *) SCpnt->host->hostdata;

	if(esp->dma_led_on)
		esp->dma_led_on(esp);

	don = esp->dma_ports_p(esp);
	if(don)
		esp->dma_ints_off(esp);

	/* We use the scratch area. */
	ESPQUEUE(("esp_queue: target=%d lun=%d ", SCpnt->target, SCpnt->lun));
	ESPDISC(("N<%02x,%02x>", SCpnt->target, SCpnt->lun));
	if(!SCpnt->use_sg) {
		ESPQUEUE(("!use_sg\n"));
		SCpnt->SCp.this_residual    = SCpnt->request_bufflen;
		SCpnt->SCp.buffer           =
			(struct scatterlist *) SCpnt->request_buffer;
		SCpnt->SCp.buffers_residual = 0;
#ifdef CONFIG_SCSI_SUNESP
		/* Sneaky. */
		SCpnt->SCp.have_data_in = (int) SCpnt->SCp.ptr =
			mmu_get_scsi_one((char *)SCpnt->SCp.buffer,
					 SCpnt->SCp.this_residual,
					 ((struct linux_sbus_device *) (esp->edev))->my_bus);
#else
		SCpnt->SCp.have_data_in = (int) SCpnt->SCp.ptr =
			(char *)SCpnt->request_buffer;
#endif

	} else {
		ESPQUEUE(("use_sg "));
#ifdef DEBUG_ESP_SG
		printk("esp%d: sglist at %p with %d buffers\n",
		       esp->esp_id, SCpnt->buffer, SCpnt->use_sg);
#endif
		SCpnt->SCp.buffer           = (struct scatterlist *) SCpnt->buffer;
		SCpnt->SCp.buffers_residual = SCpnt->use_sg - 1;
		SCpnt->SCp.this_residual    = SCpnt->SCp.buffer->length;
#ifdef CONFIG_SCSI_SUNESP
		mmu_get_scsi_sgl((struct mmu_sglist *) SCpnt->SCp.buffer,
				 SCpnt->SCp.buffers_residual,
				 ((struct linux_sbus_device *) (esp->edev))->my_bus);
		SCpnt->SCp.ptr              = (char *) SCpnt->SCp.buffer->dvma_address;
#else
		SCpnt->SCp.ptr              = (char *) SCpnt->SCp.buffer->address;
#endif
	}
	SCpnt->SCp.Status           = CHECK_CONDITION;
	SCpnt->SCp.Message          = 0xff;
	SCpnt->SCp.sent_command     = 0;

	/* Place into our queue. */
	if(SCpnt->cmnd[0] == REQUEST_SENSE) {
		ESPQUEUE(("RQSENSE\n"));
		prepend_SC(&esp->issue_SC, SCpnt);
	} else {
		ESPQUEUE(("\n"));
		append_SC(&esp->issue_SC, SCpnt);
	}

	/* Run it now if we can. */
	if(!esp->current_SC && !esp->resetting_bus)
		esp_exec_cmd(esp);

	if(don)
		esp->dma_ints_on(esp);
	restore_flags(flags);
	return 0;
}

/* Only queuing supported in this ESP driver. */
int esp_command(Scsi_Cmnd *SCpnt)
{
#ifdef DEBUG_ESP
	struct Sparc_ESP *esp = (struct Sparc_ESP *) SCpnt->host->hostdata;
#endif

	ESPLOG(("esp%d: esp_command() called...\n", esp->esp_id));
	return -1;
}

/* Dump driver state. */
static inline void esp_dump_cmd(Scsi_Cmnd *SCptr)
{
	ESPLOG(("[tgt<%02x> lun<%02x> "
		"pphase<%s> cphase<%s>]",
		SCptr->target, SCptr->lun,
		phase_string(SCptr->SCp.sent_command),
		phase_string(SCptr->SCp.phase)));
}

static inline void esp_dump_state(struct Sparc_ESP *esp, 
				  struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;
#ifdef DEBUG_ESP_CMDS
	int i;
#endif

	ESPLOG(("esp%d: dumping state\n", esp->esp_id));
	
	/* Print DMA status */
	esp->dma_dump_state(esp);

	ESPLOG(("esp%d: SW [sreg<%02x> sstep<%02x> ireg<%02x>]\n",
		esp->esp_id, esp->sreg, esp->seqreg, esp->ireg));
	ESPLOG(("esp%d: HW reread [sreg<%02x> sstep<%02x> ireg<%02x>]\n",
		esp->esp_id, eregs->esp_status, eregs->esp_sstep, eregs->esp_intrpt));
#ifdef DEBUG_ESP_CMDS
	printk("esp%d: last ESP cmds [", esp->esp_id);
	i = (esp->espcmdent - 1) & 31;
	printk("<");
	esp_print_cmd(esp->espcmdlog[i]);
	printk(">");
	i = (i - 1) & 31;
	printk("<");
	esp_print_cmd(esp->espcmdlog[i]);
	printk(">");
	i = (i - 1) & 31;
	printk("<");
	esp_print_cmd(esp->espcmdlog[i]);
	printk(">");
	i = (i - 1) & 31;
	printk("<");
	esp_print_cmd(esp->espcmdlog[i]);
	printk(">");
	printk("]\n");
#endif /* (DEBUG_ESP_CMDS) */

	if(SCptr) {
		ESPLOG(("esp%d: current command ", esp->esp_id));
		esp_dump_cmd(SCptr);
	}
	ESPLOG(("\n"));
	SCptr = esp->disconnected_SC;
	ESPLOG(("esp%d: disconnected ", esp->esp_id));
	while(SCptr) {
		esp_dump_cmd(SCptr);
		SCptr = (Scsi_Cmnd *) SCptr->host_scribble;
	}
	ESPLOG(("\n"));
}

/* Abort a command. */
int esp_abort(Scsi_Cmnd *SCptr)
{
	struct Sparc_ESP *esp = (struct Sparc_ESP *) SCptr->host->hostdata;
	struct ESP_regs *eregs = esp->eregs;
	int don;
	unsigned long flags;

	ESPLOG(("esp%d: Aborting command\n", esp->esp_id));
	save_flags(flags); cli();

	esp_dump_state(esp, eregs);

	/* Wheee, if this is the current command on the bus, the
	 * best we can do is assert ATN and wait for msgout phase.
	 * This should even fix a hung SCSI bus when we lose state
	 * in the driver and timeout because the eventual phase change
	 * will cause the ESP to (eventually) give an interrupt.
	 */
	if(esp->current_SC == SCptr) {
		esp->cur_msgout[0] = ABORT;
		esp->msgout_len = 1;
		esp->msgout_ctr = 0;
		esp_cmd(esp, eregs, ESP_CMD_SATN);
		restore_flags(flags);
		return SCSI_ABORT_PENDING;
	}

	/* If it is still in the issue queue then we can safely
	 * call the completion routine and report abort success.
	 */
	don = esp->dma_ports_p(esp);
	if(don)
		esp->dma_ints_off(esp);
	if(esp->issue_SC) {
		Scsi_Cmnd **prev, *this;
		for(prev = (&esp->issue_SC), this = esp->issue_SC;
		    this;
		    prev = (Scsi_Cmnd **) &(this->host_scribble),
		    this = (Scsi_Cmnd *) this->host_scribble) {
			if(this == SCptr) {
				*prev = (Scsi_Cmnd *) this->host_scribble;
				this->host_scribble = NULL;
				this->result = DID_ABORT << 16;
				this->done(this);
				cli();
				if(don)
					esp->dma_ints_on(esp);
				restore_flags(flags);
				return SCSI_ABORT_SUCCESS;
			}
		}
	}

	/* Yuck, the command to abort is disconnected, it is not
	 * worth trying to abort it now if something else is live
	 * on the bus at this time.  So, we let the SCSI code wait
	 * a little bit and try again later.
	 */
	if(esp->current_SC) {
		if(don)
			esp->dma_ints_on(esp);
		restore_flags(flags);
		return SCSI_ABORT_SNOOZE;
	}

	/* It's disconnected, we have to reconnect to re-establish
	 * the nexus and tell the device to abort.
	 *
	 * XXX write this code.
	 */

	if(don)
		esp->dma_ints_on(esp);
	restore_flags(flags);
	return SCSI_ABORT_SNOOZE;
}

/* Reset ESP chip, reset hanging bus, then kill active and
 * disconnected commands for targets without soft reset.
 */
int esp_reset(Scsi_Cmnd *SCptr, unsigned int how)
{
	struct Sparc_ESP *esp = (struct Sparc_ESP *) SCptr->host->hostdata;
	struct ESP_regs *eregs = esp->eregs;

	ESPLOG(("esp%d: Resetting scsi bus\n", esp->esp_id));
	esp->resetting_bus = 1;
	esp_cmd(esp, eregs, ESP_CMD_RS);
	return SCSI_RESET_PENDING;
}

/* Internal ESP done function. */
static inline void esp_done(struct Sparc_ESP *esp, int error)
{
	unsigned long flags;
	Scsi_Cmnd *done_SC;

	if(esp->current_SC) {
		/* Critical section... */
		save_flags(flags); cli();

		done_SC = esp->current_SC;
		esp->current_SC = NULL;

		/* Free dvma entry. */
		if(!done_SC->use_sg) {
#ifdef CONFIG_SCSI_SUNESP
			/* Sneaky. */
			mmu_release_scsi_one((char *)done_SC->SCp.have_data_in,
					     done_SC->request_bufflen,
					     ((struct linux_sbus_device *) (esp->edev))->my_bus);
#endif
		} else {
#ifdef DEBUG_ESP_SG
			printk("esp%d: unmapping sg ", esp->esp_id);
#endif
#ifdef CONFIG_SCSI_SUNESP
			struct scatterlist *scl = (struct scatterlist *)done_SC->buffer;
			mmu_release_scsi_sgl((struct mmu_sglist *) scl,
					     done_SC->use_sg - 1,
					     ((struct linux_sbus_device *) (esp->edev))->my_bus);
#endif
#ifdef DEBUG_ESP_SG
			printk("done.\n");
#endif
		}

		done_SC->result = error;
		if(done_SC->scsi_done)
			done_SC->scsi_done(done_SC);
		else
			panic("esp: esp->current_SC->scsi_done() == NULL");

		/* There is a window of time within the scsi_done() path
		 * of execution where interrupts are turned back on full
		 * blast and left that way.  During that time we could
		 * reconnect to a disconnected command, then we'd bomb
		 * out below.  We could also end up executing two commands
		 * at _once_.  ...just so you know why the cli() is here...
		 */

		cli(); /* GRRR!!! */

		/* Bus is free, issue any commands in the queue. */
		if(esp->issue_SC && !esp->current_SC)
			esp_exec_cmd(esp);

		restore_flags(flags);
		/* End of critical section... */
	} else {
		/* Panic is safe as current_SC is null so we may still
		 * be able to accept more commands to sync buffers.
		 */
		ESPLOG(("panicing\n"));
		panic("esp: done() called with NULL esp->current_SC");
	}
}

#ifdef THREADED_ESP_DRIVER /* planning stage... */

/* With multiple lots of commands being processed I frequently
 * see a situation where we see galloping esp herds.  esp_done()
 * wakes the entire world up and each interrupt causes a reschedule.
 * This kernel thread fixes some of these unwanted effects during
 * IO intensive activity.... I hope...
 */

static void esp_kernel_thread(void *opaque)
{
	struct Sparc_ESP *esp = opaque;

	for(;;) {
		unsigned long flags;

		while(esp->eatme_SC) {
			struct Scsi_Cmnd *SCpnt;

			SCpnt = remove_first_SC(esp->eatme_SC);
			esp_done(esp, error, SCpnt);
		}
		sleep();
	}
}
#endif

/* Wheee, ESP interrupt engine. */  

enum {
	do_phase_determine, do_reset_bus, do_reset_complete,
	do_work_bus, do_intr_end,
};

/* Forward declarations. */
static int esp_do_phase_determine(struct Sparc_ESP *esp, 
				  struct ESP_regs *eregs);
static int esp_do_data_finale(struct Sparc_ESP *esp, struct ESP_regs *eregs);
static int esp_select_complete(struct Sparc_ESP *esp, struct ESP_regs *eregs);
static int esp_do_status(struct Sparc_ESP *esp, struct ESP_regs *eregs);
static int esp_do_msgin(struct Sparc_ESP *esp, struct ESP_regs *eregs);
static int esp_do_msgindone(struct Sparc_ESP *esp, struct ESP_regs *eregs);
static int esp_do_msgout(struct Sparc_ESP *esp, struct ESP_regs *eregs);
static int esp_do_cmdbegin(struct Sparc_ESP *esp, struct ESP_regs *eregs);


/* We try to avoid some interrupts by jumping ahead and see if the ESP
 * has gotten far enough yet.  Hence the following.
 */
static inline int skipahead1(struct Sparc_ESP *esp, struct ESP_regs *eregs,
			     Scsi_Cmnd *scp, int prev_phase, int new_phase)
{
	if(scp->SCp.sent_command != prev_phase)
		return 0;

	if(esp->dma_irq_p(esp)) {
		/* Yes, we are able to save an interrupt. */
		esp->sreg = eregs->esp_status;
		esp->ireg = eregs->esp_intrpt;

		esp->sreg &= ~(ESP_STAT_INTR);
		if(!(esp->ireg & ESP_INTR_SR))
			return 0;
		else
			return do_reset_complete;
	}
	/* Ho hum, target is taking forever... */
	scp->SCp.sent_command = new_phase; /* so we don't recurse... */
	return do_intr_end;
}

static inline int skipahead2(struct Sparc_ESP *esp,
			     struct ESP_regs *eregs,
			     Scsi_Cmnd *scp, int prev_phase1, int prev_phase2,
			     int new_phase)
{
	if(scp->SCp.sent_command != prev_phase1 &&
	   scp->SCp.sent_command != prev_phase2)
		return 0;
	if(esp->dma_irq_p(esp)) {
		/* Yes, we are able to save an interrupt. */
		esp->sreg = eregs->esp_status;
		esp->ireg = eregs->esp_intrpt;

		esp->sreg &= ~(ESP_STAT_INTR);
		if(!(esp->ireg & ESP_INTR_SR))
			return 0;
		else
			return do_reset_complete;
	}
	/* Ho hum, target is taking forever... */
	scp->SCp.sent_command = new_phase; /* so we don't recurse... */
	return do_intr_end;
}

/* Misc. esp helper routines. */
static inline void esp_setcount(struct ESP_regs *eregs, int cnt)
{
	eregs->esp_tclow = (cnt & 0xff);
	eregs->esp_tcmed = ((cnt >> 8) & 0xff);
}

static inline int esp_getcount(struct ESP_regs *eregs)
{
	return (((eregs->esp_tclow)&0xff) |
		(((eregs->esp_tcmed)&0xff) << 8));
}

static inline int fcount(struct ESP_regs *eregs)
{
	return eregs->esp_fflags & ESP_FF_FBYTES;
}

static inline int fnzero(struct ESP_regs *eregs)
{
	return eregs->esp_fflags & ESP_FF_ONOTZERO;
}

/* XXX speculative nops unnecessary when continuing amidst a data phase
 * XXX even on esp100!!!  another case of flooding the bus with I/O reg
 * XXX writes...
 */
static inline void esp_maybe_nop(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	if(esp->erev == esp100)
		esp_cmd(esp, eregs, ESP_CMD_NULL);
}

static inline int sreg_to_dataphase(unchar sreg)
{
	if((sreg & ESP_STAT_PMASK) == ESP_DOP)
		return in_dataout;
	else
		return in_datain;
}

static inline int sreg_datainp(unchar sreg)
{
	return (sreg & ESP_STAT_PMASK) == ESP_DIP;
}

static inline int sreg_dataoutp(unchar sreg)
{
	return (sreg & ESP_STAT_PMASK) == ESP_DOP;
}

/* The ESP100 when in synchronous data phase, can mistake a long final
 * REQ pulse from the target as an extra byte, it places whatever is on
 * the data lines into the fifo.  For now, we will assume when this
 * happens is that the target is a bit quirky and we don't want to
 * be talking synchronously to it anyways.  Regardless, we need to
 * tell the ESP to eat the extraneous byte so that we can proceed
 * to the next phase.
 */
static inline int esp100_sync_hwbug(struct Sparc_ESP *esp,
				    struct ESP_regs *eregs, Scsi_Cmnd *sp)
{
	/* Do not touch this piece of code. */
	if((!(esp->erev == esp100)) ||
	   (!(sreg_datainp((esp->sreg = eregs->esp_status)) && !fcount(eregs)) &&
	    !(sreg_dataoutp(esp->sreg) && !fnzero(eregs)))) {
		if(sp->SCp.phase == in_dataout)
			esp_cmd(esp, eregs, ESP_CMD_FLUSH);
		return 0;
	} else {
		/* Async mode for this guy. */
		build_sync_nego_msg(esp, 0, 0);

		/* Ack the bogus byte, but set ATN first. */
		esp_cmd(esp, eregs, ESP_CMD_SATN);
		esp_cmd(esp, eregs, ESP_CMD_MOK);

		/* We may have to sit around for a bit. */
		esp_advance_phase(sp, in_the_dark);
		return 1;
	}
}

/* This closes the window during a selection with a reselect pending, because
 * we use DMA for the selection process the FIFO should hold the correct
 * contents if we get reselected during this process.  So we just need to
 * ack the possible illegal cmd interrupt pending on the esp100.
 */
static inline int esp100_reconnect_hwbug(struct Sparc_ESP *esp,
					 struct ESP_regs *eregs)
{
	volatile unchar junk;

	if(esp->erev != esp100)
		return 0;
	junk = eregs->esp_intrpt;

	if(junk & ESP_INTR_SR)
		return 1;
	return 0;
}

/* This verifies the BUSID bits during a reselection so that we know which
 * target is talking to us.
 */
static inline int reconnect_target(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	int it, me = esp->scsi_id_mask, targ = 0;

	if(2 != fcount(eregs))
		return -1;
	it = eregs->esp_fdata;
	if(!(it & me))
		return -1;
	it &= ~me;
	if(it & (it - 1))
		return -1;
	while(!(it & 1))
		targ++, it >>= 1;
	return targ;
}

/* This verifies the identify from the target so that we know which lun is
 * being reconnected.
 */
static inline int reconnect_lun(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	int lun;

	if((esp->sreg & ESP_STAT_PMASK) != ESP_MIP)
		return -1;
	lun = eregs->esp_fdata;
	if(esp->sreg & ESP_STAT_PERR)
		return 0;
	if((lun & 0x40) || !(lun & 0x80))
		return -1;
	return lun & 7;
}

/* This puts the driver in a state where it can continue a command that
 * is being continued via reselection.
 */
static inline void esp_connect(struct Sparc_ESP *esp, struct ESP_regs *eregs,
			       Scsi_Cmnd *sp)
{
	Scsi_Device *dp = sp->device;
	eregs->esp_soff = dp->sync_max_offset;
	eregs->esp_stp  = dp->sync_min_period;
	if(esp->erev > esp100a)
		eregs->esp_cfg3 = esp->config3[sp->target];
	esp->current_SC = sp;
}

/* This will place the current working command back into the issue queue
 * if we are to receive a reselection amidst a selection attempt.
 */
static inline void esp_reconnect(struct Sparc_ESP *esp, Scsi_Cmnd *sp)
{
	if(!esp->disconnected_SC)
		printk("esp%d: Weird, being reselected but disconnected "
		       "command queue is empty.\n", esp->esp_id);
	esp->snip = 0;
	esp->current_SC = 0;
	sp->SCp.phase = not_issued;
	append_SC(&esp->issue_SC, sp);
}

/* Begin message in phase. */
static inline int esp_do_msgin(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	esp_cmd(esp, eregs, ESP_CMD_FLUSH);
	esp_maybe_nop(esp, eregs);
	esp_cmd(esp, eregs, ESP_CMD_TI);
	esp->msgin_len = 1;
	esp->msgin_ctr = 0;
	esp_advance_phase(esp->current_SC, in_msgindone);
	return do_work_bus;
}

/* Determine how many bytes were successfully sent/received by the ESP.
 */
static inline void advance_sg(Scsi_Cmnd *sp)
{
	++sp->SCp.buffer;
	--sp->SCp.buffers_residual;
	sp->SCp.this_residual = sp->SCp.buffer->length;
#ifdef CONFIG_SCSI_SUNESP
	sp->SCp.ptr = sp->SCp.buffer->dvma_address;
#else
	sp->SCp.ptr = sp->SCp.buffer->address;
#endif
}

/* Complete a command, this is the only way to safely finish
 * a command with disconnected commands or a reset interrupt
 * pending because scsi_done() can enable interrupts full blast.
 */
static inline void safe_esp_done(struct Sparc_ESP *esp, int error)
{
	int before = 0;

	if(esp->dma_ports_p(esp)){
		before = 1;
		esp->dma_ints_off(esp);
	}
	esp_done(esp, error);
	if(before)
		esp->dma_ints_on(esp);
}

/* Please note that the way I've coded these routines is that I _always_
 * check for a disconnect during any and all information transfer
 * phases.  The SCSI standard states that the target _can_ cause a BUS
 * FREE condition by dropping all MSG/CD/IO/BSY signals.  Also note
 * that during information transfer phases the target controls every
 * change in phase, the only thing the initiator can do is "ask" for
 * a message out phase by driving ATN true.  The target can, and sometimes
 * will, completely ignore this request so we cannot assume anything when
 * we try to force a message out phase to abort/reset a target.  Most of
 * the time the target will eventually be nice and go to message out, so
 * we may have to hold on to our state about what we want to tell the target
 * for some period of time.
 */

/* I think I have things working here correctly.  Even partial transfers
 * within a buffer or sub-buffer should not upset us at all no matter
 * how bad the target and/or ESP fucks things up.
 */

static inline int esp_do_data(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;
	int thisphase, hmuch;

	ESPDATA(("esp_do_data: "));
	esp_maybe_nop(esp, eregs);
	thisphase = sreg_to_dataphase(esp->sreg);
	esp_advance_phase(SCptr, thisphase);
	ESPDATA(("newphase<%s> ", (thisphase == in_datain) ? "DATAIN" : "DATAOUT"));
	hmuch = esp->dma_can_transfer(esp, SCptr);
	ESPDATA(("hmuch<%d> ", hmuch));
	esp_setcount(eregs, hmuch);

	esp->dma_setup(esp, SCptr->SCp.ptr, hmuch, (thisphase == in_datain));
	esp->current_transfer_size = hmuch;
	ESPDATA(("DMA|TI --> do_intr_end\n"));
	esp_cmd(esp, eregs, ESP_CMD_DMA | ESP_CMD_TI);
	return do_intr_end;
}

/* See how successful the data transfer was. */
static inline int esp_do_data_finale(struct Sparc_ESP *esp,
				     struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;
	int bogus_data = 0, bytes_sent = 0, fifocnt, ecount;

	if(esp->dma_led_off)
		esp->dma_led_off(esp);

	ESPDATA(("esp_do_data_finale: "));

	if(SCptr->SCp.phase == in_datain) {
		if(esp->sreg & ESP_STAT_PERR) {
			/* Yuck, parity error.  The ESP asserts ATN
			 * so that we can go to message out phase
			 * immediately and inform the target that
			 * something bad happened.
			 */
			ESPLOG(("esp%d: data bad parity detected.\n",
				esp->esp_id));
			esp->cur_msgout[0] = INITIATOR_ERROR;
			esp->msgout_len = 1;
		}
		if(esp->dma_drain)
			esp->dma_drain(esp);
	}
	if(esp->dma_invalidate)
		esp->dma_invalidate(esp);

	/* This could happen for the above parity error case. */
	if(!(esp->ireg == ESP_INTR_BSERV)) {
		/* Please go to msgout phase, please please please... */
		ESPLOG(("esp%d: !BSERV after data, probably to msgout\n",
			esp->esp_id));
		return esp_do_phase_determine(esp, eregs);
	}	

	/* Check for partial transfers and other horrible events. */
	fifocnt = fcount(eregs);
	ecount = esp_getcount(eregs);
	bytes_sent = esp->current_transfer_size;
	if(!(esp->sreg & ESP_STAT_TCNT))
		bytes_sent -= ecount;
	if(SCptr->SCp.phase == in_dataout)
		bytes_sent -= fifocnt;

	ESPDATA(("bytes_sent=%d, ", bytes_sent));

	/* If we were in synchronous mode, check for peculiarities. */
	if(SCptr->device->sync_max_offset)
		bogus_data = esp100_sync_hwbug(esp, eregs, SCptr);
	else
		esp_cmd(esp, eregs, ESP_CMD_FLUSH);

	/* Until we are sure of what has happened, we are certainly
	 * in the dark.
	 */
	esp_advance_phase(SCptr, in_the_dark);

	if(bytes_sent < 0) {
		/* I've seen this happen due to lost state in this
		 * driver.  No idea why it happened, but allowing
		 * this value to be negative caused things to
		 * lock up.  This allows greater chance of recovery.
		 */
		ESPLOG(("esp%d: yieee, bytes_sent < 0!\n", esp->esp_id));
		ESPLOG(("esp%d: csz=%d fifocount=%d ecount=%d\n",
			esp->esp_id,
			esp->current_transfer_size, fifocnt, ecount));
		ESPLOG(("esp%d: use_sg=%d ptr=%p this_residual=%d\n",
			esp->esp_id,
			SCptr->use_sg, SCptr->SCp.ptr, SCptr->SCp.this_residual));
		bytes_sent = 0;
	}

	/* Update the state of our transfer. */
	SCptr->SCp.ptr += bytes_sent;
	SCptr->SCp.this_residual -= bytes_sent;
	if(SCptr->SCp.this_residual < 0) {
		/* shit */
		printk("esp%d: Data transfer overrun (%d).\n", esp->esp_id, SCptr->SCp.this_residual);
		SCptr->SCp.this_residual = 0;
	}

	/* Maybe continue. */
	if(!bogus_data) {
		ESPDATA(("!bogus_data, "));
		/* NO MATTER WHAT, we advance the scatterlist,
		 * if the target should decide to disconnect
		 * in between scatter chunks (which is common)
		 * we could die horribly!  I used have the sg
		 * advance occur only if we are going back into
		 * (or are staying in) a data phase, you can
		 * imagine the hell I went through trying to
		 * figure this out.
		 */
		if(SCptr->use_sg && !SCptr->SCp.this_residual)
			advance_sg(SCptr);
		if(sreg_datainp(esp->sreg) || sreg_dataoutp(esp->sreg)) {
			ESPDATA(("to more data\n"));
			return esp_do_data(esp, eregs);
		}
		ESPDATA(("to new phase\n"));
		return esp_do_phase_determine(esp, eregs);
	}
	/* Bogus data, just wait for next interrupt. */
	ESPLOG(("esp%d: bogus_data during end of data phase\n",
		esp->esp_id));
	return do_intr_end;
}

/* Either a command is completing or a target is dropping off the bus
 * to continue the command in the background so we can do other work.
 */
static inline int esp_do_freebus(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;
	int rval;

	rval = skipahead2(esp, eregs, SCptr, in_status, in_msgindone, in_freeing);
	if(rval)
		return rval;

	if(esp->ireg != ESP_INTR_DC) {
		ESPLOG(("esp%d: Target will not disconnect\n", esp->esp_id));
		return do_reset_bus; /* target will not drop BSY... */
	}
	esp->msgout_len = 0;
	esp->prevmsgout = NOP;
	if(esp->prevmsgin == COMMAND_COMPLETE) {
		/* Normal end of nexus. */
		if(esp->disconnected_SC)
			esp_cmd(esp, eregs, ESP_CMD_ESEL);

		if(SCptr->SCp.Status != GOOD &&
		   ((1<<SCptr->target) & esp->targets_present) &&
		   SCptr->device->sync && SCptr->device->sync_max_offset) {
			/* SCSI standard says that the synchronous capabilities
			 * should be renegotiated at this point.  Most likely
			 * we are about to request sense from this target
			 * in which case we want to avoid using sync
			 * transfers until we are sure of the current target
			 * state.
			 */
			ESPMISC(("esp: Status <%d> for target %d lun %d\n",
				 SCptr->SCp.Status, SCptr->target, SCptr->lun));

			/* But don't do this when spinning up a disk at
			 * boot time while we poll for completion as it
			 * fills up the console with messages.  Also, tapes
			 * can report not ready many times right after
			 * loading up a tape.
			 */
			if(SCptr->cmnd[0] != START_STOP &&
			   SCptr->data_cmnd[0] != START_STOP &&
			   SCptr->cmnd[0] != TEST_UNIT_READY &&
			   SCptr->data_cmnd[0] != TEST_UNIT_READY &&
			   !(SCptr->device->type == TYPE_TAPE &&
			     (SCptr->cmnd[0] == TEST_UNIT_READY ||
			      SCptr->data_cmnd[0] == TEST_UNIT_READY ||
			      SCptr->cmnd[0] == MODE_SENSE ||
			      SCptr->data_cmnd[0] == MODE_SENSE)))
				SCptr->device->sync = 0;
		}
		ESPDISC(("F<%02x,%02x>", SCptr->target, SCptr->lun));
		safe_esp_done(esp, ((SCptr->SCp.Status & 0xff) |
				    ((SCptr->SCp.Message & 0xff)<<8) |
				    (DID_OK << 16)));
	} else if(esp->prevmsgin == DISCONNECT) {
		/* Normal disconnect. */
		esp_cmd(esp, eregs, ESP_CMD_ESEL);
		ESPDISC(("D<%02x,%02x>", SCptr->target, SCptr->lun));
		append_SC(&esp->disconnected_SC, SCptr);
		esp->current_SC = NULL;
		if(esp->issue_SC)
			esp_exec_cmd(esp);
	} else {
		/* Driver bug, we do not expect a disconnect here
		 * and should not have advanced the state engine
		 * to in_freeing.
		 */
		ESPLOG(("esp%d: last msg not disc and not cmd cmplt.\n",
			esp->esp_id));
		return do_reset_bus;
	}
	return do_intr_end;
}

/* Do the needy when a target tries to reconnect to us. */
static inline int esp_do_reconnect(struct Sparc_ESP *esp, 
				   struct ESP_regs *eregs)
{
	int lun, target;
	Scsi_Cmnd *SCptr;

	/* Check for all bogus conditions first. */
	target = reconnect_target(esp, eregs);
	if(target < 0) {
		ESPDISC(("bad bus bits\n"));
		return do_reset_bus;
	}
	lun = reconnect_lun(esp, eregs);
	if(lun < 0) {
		ESPDISC(("target=%2x, bad identify msg\n", target));
		return do_reset_bus;
	}

	/* Things look ok... */
	ESPDISC(("R<%02x,%02x>", target, lun));
	esp_cmd(esp, eregs, ESP_CMD_FLUSH);
	if(esp100_reconnect_hwbug(esp, eregs))
		return do_reset_bus;
	esp_cmd(esp, eregs, ESP_CMD_NULL);
	SCptr = remove_SC(&esp->disconnected_SC, (unchar) target, (unchar) lun);
	if(!SCptr) {
		Scsi_Cmnd *sp;

		ESPLOG(("esp%d: Eieeee, reconnecting unknown command!\n",
			esp->esp_id));
		ESPLOG(("QUEUE DUMP\n"));
		sp = esp->issue_SC;
		ESPLOG(("esp%d: issue_SC[", esp->esp_id));
		while(sp) {
			ESPLOG(("<%02x,%02x>", sp->target, sp->lun));
			sp = (Scsi_Cmnd *) sp->host_scribble;
		}
		ESPLOG(("]\n"));
		sp = esp->current_SC;
		ESPLOG(("esp%d: current_SC[", esp->esp_id));
		while(sp) {
			ESPLOG(("<%02x,%02x>", sp->target, sp->lun));
			sp = (Scsi_Cmnd *) sp->host_scribble;
		}
		ESPLOG(("]\n"));
		sp = esp->disconnected_SC;
		ESPLOG(("esp%d: disconnected_SC[", esp->esp_id));
		while(sp) {
			ESPLOG(("<%02x,%02x>", sp->target, sp->lun));
			sp = (Scsi_Cmnd *) sp->host_scribble;
		}
		ESPLOG(("]\n"));
		return do_reset_bus;
	}
	esp_connect(esp, eregs, SCptr);
	esp_cmd(esp, eregs, ESP_CMD_MOK);

	/* No need for explicit restore pointers operation. */
	esp->snip = 0;
	esp_advance_phase(SCptr, in_the_dark);
	return do_intr_end;
}

/* End of NEXUS (hopefully), pick up status + message byte then leave if
 * all goes well.
 */
static int esp_do_status(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;
	int intr;
	int rval;

	rval = skipahead1(esp, eregs, SCptr, in_the_dark, in_status);
	if(rval)
		return rval;

	intr = esp->ireg;
	ESPSTAT(("esp_do_status: "));
	if(intr != ESP_INTR_DC) {
		int message_out = 0; /* for parity problems */

		/* Ack the message. */
		ESPSTAT(("ack msg, "));
		esp_cmd(esp, eregs, ESP_CMD_MOK);

		if(esp->dma_poll)
			esp->dma_poll(esp, (unsigned char *) esp->esp_command);

		ESPSTAT(("got something, "));
		/* ESP chimes in with one of
		 *
		 * 1) function done interrupt:
		 *	both status and message in bytes
		 *	are available
		 *
		 * 2) bus service interrupt:
		 *	only status byte was acquired
		 *
		 * 3) Anything else:
		 *	can't happen, but we test for it
		 *	anyways
		 *
		 * ALSO: If bad parity was detected on either
		 *       the status _or_ the message byte then
		 *       the ESP has asserted ATN on the bus
		 *       and we must therefore wait for the
		 *       next phase change.
		 */
		if(intr & ESP_INTR_FDONE) {
			/* We got it all, hallejulia. */
			ESPSTAT(("got both, "));
			SCptr->SCp.Status = esp->esp_command[0];
			SCptr->SCp.Message = esp->esp_command[1];
			esp->prevmsgin = SCptr->SCp.Message;
			esp->cur_msgin[0] = SCptr->SCp.Message;
			if(esp->sreg & ESP_STAT_PERR) {
				/* There was bad parity for the
				 * message byte, the status byte
				 * was ok.
				 */
				message_out = MSG_PARITY_ERROR;
			}
		} else if(intr == ESP_INTR_BSERV) {
			/* Only got status byte. */
			ESPLOG(("esp%d: got status only, ", esp->esp_id));
			if(!(esp->sreg & ESP_STAT_PERR)) {
				SCptr->SCp.Status = esp->esp_command[0];
				SCptr->SCp.Message = 0xff;
			} else {
				/* The status byte had bad parity.
				 * we leave the scsi_pointer Status
				 * field alone as we set it to a default
				 * of CHECK_CONDITION in esp_queue.
				 */
				message_out = INITIATOR_ERROR;
			}
		} else {
			/* This shouldn't happen ever. */
			ESPSTAT(("got bolixed\n"));
			esp_advance_phase(SCptr, in_the_dark);
			return esp_do_phase_determine(esp, eregs);
		}

		if(!message_out) {
			ESPSTAT(("status=%2x msg=%2x, ", SCptr->SCp.Status,
				SCptr->SCp.Message));
			if(SCptr->SCp.Message == COMMAND_COMPLETE) {
				ESPSTAT(("and was COMMAND_COMPLETE\n"));
				esp_advance_phase(SCptr, in_freeing);
				return esp_do_freebus(esp, eregs);
			} else {
				ESPLOG(("esp%d: and _not_ COMMAND_COMPLETE\n",
					esp->esp_id));
				esp->msgin_len = esp->msgin_ctr = 1;
				esp_advance_phase(SCptr, in_msgindone);
				return esp_do_msgindone(esp, eregs);
			}
		} else {
			/* With luck we'll be able to let the target
			 * know that bad parity happened, it will know
			 * which byte caused the problems and send it
			 * again.  For the case where the status byte
			 * receives bad parity, I do not believe most
			 * targets recover very well.  We'll see.
			 */
			ESPLOG(("esp%d: bad parity somewhere mout=%2x\n",
				esp->esp_id, message_out));
			esp->cur_msgout[0] = message_out;
			esp->msgout_len = esp->msgout_ctr = 1;
			esp_advance_phase(SCptr, in_the_dark);
			return esp_do_phase_determine(esp, eregs);
		}
	} else {
		/* If we disconnect now, all hell breaks loose. */
		ESPLOG(("esp%d: whoops, disconnect\n", esp->esp_id));
		esp_advance_phase(SCptr, in_the_dark);
		return esp_do_phase_determine(esp, eregs);
	}
}

/* The target has control of the bus and we have to see where it has
 * taken us.
 */
static int esp_do_phase_determine(struct Sparc_ESP *esp,
				  struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;

	ESPPHASE(("esp_do_phase_determine: "));
	if(!(esp->ireg & ESP_INTR_DC)) {
		switch(esp->sreg & ESP_STAT_PMASK) {
		case ESP_DOP:
		case ESP_DIP:
			ESPPHASE(("to data phase\n"));
			return esp_do_data(esp, eregs);

		case ESP_STATP:
			/* Whee, status phase, finish up the command. */
			ESPPHASE(("to status phase\n"));

			esp_cmd(esp, eregs, ESP_CMD_FLUSH);

			if(esp->do_pio_cmds){
				esp_advance_phase(SCptr, in_status);
				esp_cmd(esp, eregs, ESP_CMD_ICCSEQ);
				while(!(esp->eregs->esp_status & ESP_STAT_INTR));
				esp->esp_command[0] = eregs->esp_fdata;
				while(!(esp->eregs->esp_status & ESP_STAT_INTR));
				esp->esp_command[1] = eregs->esp_fdata;
			} else {				
				esp->esp_command[0] = 0xff;
				esp->esp_command[1] = 0xff;

				eregs->esp_tclow = 2;
				eregs->esp_tcmed = 0;
				esp->dma_init_read(esp, (char *) esp->esp_command, 2);
				esp_advance_phase(SCptr, in_status);
				esp_cmd(esp, eregs, ESP_CMD_DMA | ESP_CMD_ICCSEQ);
			}
			return esp_do_status(esp, eregs);

		case ESP_MOP:
			ESPPHASE(("to msgout phase\n"));
			esp_advance_phase(SCptr, in_msgout);
			return esp_do_msgout(esp, eregs);

		case ESP_MIP:
			ESPPHASE(("to msgin phase\n"));
			esp_advance_phase(SCptr, in_msgin);
			return esp_do_msgin(esp, eregs);

		case ESP_CMDP:
			/* Ugh, we're running a non-standard command the
			 * ESP doesn't understand, one byte at a time.
			 */
			ESPPHASE(("to cmd phase\n"));
			esp_advance_phase(SCptr, in_cmdbegin);
			return esp_do_cmdbegin(esp, eregs);
		};
	} else {
		Scsi_Device *dp = SCptr->device;

		/* This means real problems if we see this
		 * here.  Unless we were actually trying
		 * to force the device to abort/reset.
		 */
		ESPLOG(("esp%d Disconnect amidst phases, ", esp->esp_id));
		ESPLOG(("pphase<%s> cphase<%s>, ",
			phase_string(SCptr->SCp.phase),
			phase_string(SCptr->SCp.sent_command)));
		if(esp->disconnected_SC)
			esp_cmd(esp, eregs, ESP_CMD_ESEL);

		switch(esp->cur_msgout[0]) {
		default:
			/* We didn't expect this to happen at all. */
			ESPLOG(("device is bolixed\n"));
			esp_advance_phase(SCptr, in_tgterror);
			safe_esp_done(esp, (DID_ERROR << 16));
			break;

		case BUS_DEVICE_RESET:
			ESPLOG(("device reset successful\n"));
			dp->sync_max_offset = 0;
			dp->sync_min_period = 0;
			dp->sync = 0;
			esp_advance_phase(SCptr, in_resetdev);
			safe_esp_done(esp, (DID_RESET << 16));
			break;

		case ABORT:
			ESPLOG(("device abort successful\n"));
			esp_advance_phase(SCptr, in_abortone);
			safe_esp_done(esp, (DID_ABORT << 16));
			break;

		};
		return do_intr_end;
	}

	ESPLOG(("esp%d: to unknown phase\n", esp->esp_id));
	printk("esp%d: Bizarre bus phase %2x.\n", esp->esp_id,
	       esp->sreg & ESP_STAT_PMASK);
	return do_reset_bus;
}

/* First interrupt after exec'ing a cmd comes here. */
static int esp_select_complete(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;
	Scsi_Device *SDptr = SCptr->device;
	int cmd_bytes_sent, fcnt;

	esp->seqreg = (eregs->esp_sstep & ESP_STEP_VBITS);
	fcnt = (eregs->esp_fflags & ESP_FF_FBYTES);
	cmd_bytes_sent = esp->dma_bytes_sent(esp, fcnt);
	if(esp->dma_invalidate)
		esp->dma_invalidate(esp);

	/* Let's check to see if a reselect happened
	 * while we we're trying to select.  This must
	 * be checked first.
	 */
	if(esp->ireg == (ESP_INTR_RSEL | ESP_INTR_FDONE)) {
		esp_reconnect(esp, SCptr);
		return esp_do_reconnect(esp, eregs);
	}

	/* Looks like things worked, we should see a bus service &
	 * a function complete interrupt at this point.  Note we
	 * are doing a direct comparison because we don't want to
	 * be fooled into thinking selection was successful if
	 * ESP_INTR_DC is set, see below.
	 */
	if(esp->ireg == (ESP_INTR_FDONE | ESP_INTR_BSERV)) {
		/* target speaks... */
		esp->targets_present |= (1<<SCptr->target);

		/* What if the target ignores the sdtr? */
		if(esp->snip)
			SDptr->sync = 1;

		/* See how far, if at all, we got in getting
		 * the information out to the target.
		 */
		switch(esp->seqreg) {
		default:

		case ESP_STEP_ASEL:
			/* Arbitration won, target selected, but
			 * we are in some phase which is not command
			 * phase nor is it message out phase.
			 *
			 * XXX We've confused the target, obviously.
			 * XXX So clear it's state, but we also end
			 * XXX up clearing everyone elses.  That isn't
			 * XXX so nice.  I'd like to just reset this
			 * XXX target, but if I cannot even get it's
			 * XXX attention and finish selection to talk
			 * XXX to it, there is not much more I can do.
			 * XXX If we have a loaded bus we're going to
			 * XXX spend the next second or so renegotiating
			 * XXX for synchronous transfers.
			 */
			ESPLOG(("esp%d: STEP_ASEL for tgt %d\n",
				esp->esp_id, SCptr->target));

		case ESP_STEP_SID:
			/* Arbitration won, target selected, went
			 * to message out phase, sent one message
			 * byte, then we stopped.  ATN is asserted
			 * on the SCSI bus and the target is still
			 * there hanging on.  This is a legal
			 * sequence step if we gave the ESP a select
			 * and stop command.
			 *
			 * XXX See above, I could set the borken flag
			 * XXX in the device struct and retry the
			 * XXX command.  But would that help for
			 * XXX tagged capable targets?
			 */

		case ESP_STEP_NCMD:
			/* Arbitration won, target selected, maybe
			 * sent the one message byte in message out
			 * phase, but we did not go to command phase
			 * in the end.  Actually, we could have sent
			 * only some of the message bytes if we tried
			 * to send out the entire identify and tag
			 * message using ESP_CMD_SA3.
			 */
			cmd_bytes_sent = 0;
			break;

		case ESP_STEP_PPC:
			/* No, not the powerPC pinhead.  Arbitration
			 * won, all message bytes sent if we went to
			 * message out phase, went to command phase
			 * but only part of the command was sent.
			 *
			 * XXX I've seen this, but usually in conjunction
			 * XXX with a gross error which appears to have
			 * XXX occurred between the time I told the
			 * XXX ESP to arbitrate and when I got the
			 * XXX interrupt.  Could I have misloaded the
			 * XXX command bytes into the fifo?  Actually,
			 * XXX I most likely missed a phase, and therefore
			 * XXX went into never never land and didn't even
			 * XXX know it.  That was the old driver though.
			 * XXX What is even more peculiar is that the ESP
			 * XXX showed the proper function complete and
			 * XXX bus service bits in the interrupt register.
			 */

		case ESP_STEP_FINI4:
		case ESP_STEP_FINI5:
		case ESP_STEP_FINI6:
		case ESP_STEP_FINI7:
			/* Account for the identify message */
			if(SCptr->SCp.phase == in_slct_norm)
				cmd_bytes_sent -= 1;
		};
		esp_cmd(esp, eregs, ESP_CMD_NULL);

		/* Be careful, we could really get fucked during synchronous
		 * data transfers if we try to flush the fifo now.
		 */
		if(!fcnt && /* Fifo is empty and... */
		   /* either we are not doing synchronous transfers or... */
		   (!SDptr->sync_max_offset ||
		    /* We are not going into data in phase. */
		    ((esp->sreg & ESP_STAT_PMASK) != ESP_DIP)))
			esp_cmd(esp, eregs, ESP_CMD_FLUSH); /* flush is safe */

		/* See how far we got if this is not a slow command. */
		if(!esp->esp_slowcmd) {
			if(cmd_bytes_sent < 0)
				cmd_bytes_sent = 0;
			if(cmd_bytes_sent != SCptr->cmd_len) {
				/* Crapola, mark it as a slowcmd
				 * so that we have some chance of
				 * keeping the command alive with
				 * good luck.
				 *
				 * XXX Actually, if we didn't send it all
				 * XXX this means either we didn't set things
				 * XXX up properly (driver bug) or the target
				 * XXX or the ESP detected parity on one of
				 * XXX the command bytes.  This makes much
				 * XXX more sense, and therefore this code
				 * XXX should be changed to send out a
				 * XXX parity error message or if the status
				 * XXX register shows no parity error then
				 * XXX just expect the target to bring the
				 * XXX bus into message in phase so that it
				 * XXX can send us the parity error message.
				 * XXX SCSI sucks...
				 */
				esp->esp_slowcmd = 1;
				esp->esp_scmdp = &(SCptr->cmnd[cmd_bytes_sent]);
				esp->esp_scmdleft = (SCptr->cmd_len - cmd_bytes_sent);
			}
		}

		/* Now figure out where we went. */
		esp_advance_phase(SCptr, in_the_dark);
		return esp_do_phase_determine(esp, eregs);
	}

	/* Did the target even make it? */
	if(esp->ireg == ESP_INTR_DC) {
		/* wheee... nobody there or they didn't like
		 * what we told it to do, clean up.
		 */

		/* If anyone is off the bus, but working on
		 * a command in the background for us, tell
		 * the ESP to listen for them.
		 */
		if(esp->disconnected_SC)
			esp_cmd(esp, eregs, ESP_CMD_ESEL);

		if(((1<<SCptr->target) & esp->targets_present) &&
		   esp->seqreg && esp->cur_msgout[0] == EXTENDED_MESSAGE &&
		   (SCptr->SCp.phase == in_slct_msg ||
		    SCptr->SCp.phase == in_slct_stop)) {
			/* shit */
			esp->snip = 0;
			printk("esp%d: Failed synchronous negotiation for target %d "
			       "lun %d\n",
			       esp->esp_id, SCptr->target, SCptr->lun);
			SDptr->sync_max_offset = 0;
			SDptr->sync_min_period = 0;
			SDptr->sync = 1; /* so we don't negotiate again */

			/* Run the command again, this time though we
			 * won't try to negotiate for synchronous transfers.
			 *
			 * XXX I'd like to do something like send an
			 * XXX INITIATOR_ERROR or ABORT message to the
			 * XXX target to tell it, "Sorry I confused you,
			 * XXX please come back and I will be nicer next
			 * XXX time".  But that requires having the target
			 * XXX on the bus, and it has dropped BSY on us.
			 */
			esp->current_SC = NULL;
			esp_advance_phase(SCptr, not_issued);
			prepend_SC(&esp->issue_SC, SCptr);
			esp_exec_cmd(esp);
			return do_intr_end;
		}

		/* Ok, this is normal, this is what we see during boot
		 * or whenever when we are scanning the bus for targets.
		 * But first make sure that is really what is happening.
		 */
		if(((1<<SCptr->target) & esp->targets_present)) {
			printk("esp%d: Warning, live target %d not responding to "
			       "selection.\n", esp->esp_id, SCptr->target);

			/* This _CAN_ happen.  The SCSI standard states that
			 * the target is to _not_ respond to selection if
			 * _it_ detects bad parity on the bus for any reason.
			 * Therefore, we assume that if we've talked successfully
			 * to this target before, bad parity is the problem.
			 */
			safe_esp_done(esp, (DID_PARITY << 16));
		} else {
			/* Else, there really isn't anyone there. */
			ESPMISC(("esp: selection failure, maybe nobody there?\n"));
			ESPMISC(("esp: target %d lun %d\n",
				 SCptr->target, SCptr->lun));
			safe_esp_done(esp, (DID_BAD_TARGET << 16));
		}
		return do_intr_end;
	}


	ESPLOG(("esp%d: Selection failure.\n", esp->esp_id));
	printk("esp%d: Currently -- ", esp->esp_id);
	esp_print_ireg(esp->ireg);
	printk(" ");
	esp_print_statreg(esp->sreg);
	printk(" ");
	esp_print_seqreg(esp->seqreg);
	printk("\n");
	printk("esp%d: New -- ", esp->esp_id);
	esp->sreg = eregs->esp_status;
	esp->seqreg = eregs->esp_sstep;
	esp->ireg = eregs->esp_intrpt;

	esp_print_ireg(esp->ireg);
	printk(" ");
	esp_print_statreg(esp->sreg);
	printk(" ");
	esp_print_seqreg(esp->seqreg);
	printk("\n");
	ESPLOG(("esp%d: resetting bus\n", esp->esp_id));
	return do_reset_bus; /* ugh... */
}

/* Continue reading bytes for msgin phase. */
static int esp_do_msgincont(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	if(esp->ireg & ESP_INTR_BSERV) {
		/* in the right phase too? */
		if((esp->sreg & ESP_STAT_PMASK) == ESP_MIP) {
			/* phew... */
			esp_cmd(esp, eregs, ESP_CMD_TI);
			esp_advance_phase(esp->current_SC, in_msgindone);
			return do_intr_end;
		}

		/* We changed phase but ESP shows bus service,
		 * in this case it is most likely that we, the
		 * hacker who has been up for 20hrs straight
		 * staring at the screen, drowned in coffee
		 * smelling like retched cigarette ashes
		 * have miscoded something..... so, try to
		 * recover as best we can.
		 */
		printk("esp%d: message in mis-carriage.\n", esp->esp_id);
	}
	esp_advance_phase(esp->current_SC, in_the_dark);
	return do_phase_determine;
}

static inline int check_singlebyte_msg(struct Sparc_ESP *esp,
				       struct ESP_regs *eregs)
{
	esp->prevmsgin = esp->cur_msgin[0];
	if(esp->cur_msgin[0] & 0x80) {
		/* wheee... */
		ESPLOG(("esp%d: target sends identify amidst phases\n",
			esp->esp_id));
		esp_advance_phase(esp->current_SC, in_the_dark);
		return 0;
	} else if(((esp->cur_msgin[0] & 0xf0) == 0x20) ||
		  (esp->cur_msgin[0] == EXTENDED_MESSAGE)) {
		esp->msgin_len = 2;
		esp_advance_phase(esp->current_SC, in_msgincont);
		return 0;
	}
	esp_advance_phase(esp->current_SC, in_the_dark);
	switch(esp->cur_msgin[0]) {
	default:
		/* We don't want to hear about it. */
		ESPLOG(("esp%d: msg %02x which we dont know about\n", esp->esp_id,
			esp->cur_msgin[0]));
		return MESSAGE_REJECT;

	case NOP:
		ESPLOG(("esp%d: target %d sends a nop\n", esp->esp_id,
			esp->current_SC->target));
		return 0;

	case RESTORE_POINTERS:
	case SAVE_POINTERS:
		/* We handle this all automatically. */
		return 0;

	case COMMAND_COMPLETE:
	case DISCONNECT:
		/* Freeing the bus, let it go. */
		esp->current_SC->SCp.phase = in_freeing;
		return 0;

	case MESSAGE_REJECT:
		ESPMISC(("msg reject, "));
		if(esp->prevmsgout == EXTENDED_MESSAGE) {
			Scsi_Device *SDptr = esp->current_SC->device;

			/* Doesn't look like this target can
			 * do synchronous transfers.
			 */
			ESPSDTR(("got reject, was trying sync nego, clearing sync\n"));
			SDptr->sync = 1;
			SDptr->sync_min_period = 0;
			SDptr->sync_max_offset = 0;
			return 0;
		} else {
			ESPMISC(("not sync nego, sending ABORT\n"));
			return ABORT;
		}
	};
}

/* Target negotiates for synchronous transfers before we do, this
 * is legal although very strange.  What is even funnier is that
 * the SCSI2 standard specifically recommends against targets doing
 * this because so many initiators cannot cope with this occuring.
 */
static inline int target_with_ants_in_pants(struct Sparc_ESP *esp,
					    Scsi_Cmnd *SCptr,
					    Scsi_Device *SDptr)
{
	if(SDptr->sync || SDptr->borken) {
		/* sorry, no can do */
		ESPSDTR(("forcing to async, "));
		build_sync_nego_msg(esp, 0, 0);
		SDptr->sync = 1;
		esp->snip = 1;
		ESPLOG(("esp%d: hoping for msgout\n", esp->esp_id));
		esp_advance_phase(SCptr, in_the_dark);
		return EXTENDED_MESSAGE;
	}

	/* Ok, we'll check them out... */
	return 0;
}

static inline void sync_report(struct Sparc_ESP *esp)
{
	int msg3, msg4;

	msg3 = esp->cur_msgin[3];
	msg4 = esp->cur_msgin[4];
	if(msg4) {
		int hz = 1000000000 / (msg3 * 4);
		int integer = hz / 1000000;
		int fraction = (hz - (integer * 1000000)) / 10000;
		printk("esp%d: target %d [period %dns offset %d %d.%02dMHz %s SCSI%s]\n",
		       esp->esp_id, esp->current_SC->target,
		       (int) msg3 * 4,
		       (int) msg4,
		       integer, fraction,
		       (((msg3 * 4) < 200) ? "FAST" : "synchronous"),
		       (((msg3 * 4) < 200) ? "-II" : ""));
	} else {
		printk("esp%d: target %d asynchronous\n",
		       esp->esp_id, esp->current_SC->target);
	}
}

static inline int check_multibyte_msg(struct Sparc_ESP *esp,
				       struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;
	Scsi_Device *SDptr = SCptr->device;
	unchar regval = 0;
	int message_out = 0;

	ESPSDTR(("chk multibyte msg: "));
	if(esp->cur_msgin[2] == EXTENDED_SDTR) {
		int period = esp->cur_msgin[3];
		int offset = esp->cur_msgin[4];

		ESPSDTR(("is sync nego response, "));
		if(!esp->snip) {
			int rval;

			/* Target negotiates first! */
			ESPSDTR(("target jumps the gun, "));
			message_out = EXTENDED_MESSAGE; /* we must respond */
			rval = target_with_ants_in_pants(esp, SCptr, SDptr);
			if(rval)
				return rval;
		}

		ESPSDTR(("examining sdtr, "));

		/* Offset cannot be larger than ESP fifo size. */
		if(offset > 15) {
			ESPSDTR(("offset too big %2x, ", offset));
			offset = 15;
			ESPSDTR(("sending back new offset\n"));
			build_sync_nego_msg(esp, period, offset);
			return EXTENDED_MESSAGE;
		}

		if(offset && period > esp->max_period) {
			/* Yeee, async for this slow device. */
			ESPSDTR(("period too long %2x, ", period));
			build_sync_nego_msg(esp, 0, 0);
			ESPSDTR(("hoping for msgout\n"));
			esp_advance_phase(esp->current_SC, in_the_dark);
			return EXTENDED_MESSAGE;
		} else if (offset && period < esp->min_period) {
			ESPSDTR(("period too short %2x, ", period));
			period = esp->min_period;
			if(esp->erev > esp236)
				regval = 4;
			else
				regval = 5;
		} else if(offset) {
			int tmp;

			ESPSDTR(("period is ok, "));
			tmp = esp->ccycle / 1000;
			regval = (((period << 2) + tmp - 1) / tmp);
			if(regval && ((esp->erev == fas100a ||
				       esp->erev == fas236))) {
				if(period >= 50)
					regval--;
			}
		}

		if(offset) {

			unchar bit;

			SDptr->sync_min_period = (regval & 0x1f);
			SDptr->sync_max_offset = (offset | esp->radelay);
			if((esp->erev == fas100a || esp->erev == fas236)) {
				if(esp->erev == fas100a)
					bit = ESP_CONFIG3_FAST;
				else
					bit = ESP_CONFIG3_FSCSI;
				if(period < 50)
					esp->config3[SCptr->target] |= bit;
				else
					esp->config3[SCptr->target] &= ~bit;
				eregs->esp_cfg3 = esp->config3[SCptr->target];
			}
			eregs->esp_soff = SDptr->sync_min_period;
			eregs->esp_stp  = SDptr->sync_max_offset;

			ESPSDTR(("soff=%2x stp=%2x cfg3=%2x\n",
				SDptr->sync_max_offset,
				SDptr->sync_min_period,
				esp->config3[SCptr->target]));

			esp->snip = 0;
		} else if(SDptr->sync_max_offset) {
			unchar bit;

			/* back to async mode */
			ESPSDTR(("unaccaptable sync nego, forcing async\n"));
			SDptr->sync_max_offset = 0;
			SDptr->sync_min_period = 0;
			eregs->esp_soff = 0;
			eregs->esp_stp = 0;
			if((esp->erev == fas100a || esp->erev == fas236)) {
				if(esp->erev == fas100a)
					bit = ESP_CONFIG3_FAST;
				else
					bit = ESP_CONFIG3_FSCSI;
				esp->config3[SCptr->target] &= ~bit;
				eregs->esp_cfg3 = esp->config3[SCptr->target];
			}
		}

		sync_report(esp);

		ESPSDTR(("chk multibyte msg: sync is known, "));
		SDptr->sync = 1;

		if(message_out) {
			ESPLOG(("esp%d: sending sdtr back, hoping for msgout\n",
				esp->esp_id));
			build_sync_nego_msg(esp, period, offset);
			esp_advance_phase(SCptr, in_the_dark);
			return EXTENDED_MESSAGE;
		}

		ESPSDTR(("returning zero\n"));
		esp_advance_phase(SCptr, in_the_dark); /* ...or else! */
		return 0;
	} else if(esp->cur_msgin[2] == EXTENDED_MODIFY_DATA_POINTER) {
		ESPLOG(("esp%d: rejecting modify data ptr msg\n", esp->esp_id));
		message_out = MESSAGE_REJECT;
	}
	esp_advance_phase(SCptr, in_the_dark);
	return message_out;
}

static int esp_do_msgindone(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;
	int message_out = 0, it = 0;
	int rval;

	rval = skipahead1(esp, eregs, SCptr, in_msgin, in_msgindone);
	if(rval)
		return rval;

	if(SCptr->SCp.sent_command != in_status) {
		if(!(esp->ireg & ESP_INTR_DC)) {
			if(esp->msgin_len && (esp->sreg & ESP_STAT_PERR)) {
				message_out = MSG_PARITY_ERROR;
				esp_cmd(esp, eregs, ESP_CMD_FLUSH);
			} else if((it = (eregs->esp_fflags & ESP_FF_FBYTES))!=1) {
				/* We certainly dropped the ball somewhere. */
				message_out = INITIATOR_ERROR;
				esp_cmd(esp, eregs, ESP_CMD_FLUSH);
			} else if(!esp->msgin_len) {
				it = eregs->esp_fdata;
				esp_advance_phase(SCptr, in_msgincont);
			} else {
				/* it is ok and we want it */
				it = esp->cur_msgin[esp->msgin_ctr] = eregs->esp_fdata;
				esp->msgin_ctr++;
			}
		} else {
			esp_advance_phase(SCptr, in_the_dark);
			return do_work_bus;
		}
	} else {
		it = esp->cur_msgin[0];
	}
	if(!message_out && esp->msgin_len) {
		if(esp->msgin_ctr < esp->msgin_len) {
			esp_advance_phase(SCptr, in_msgincont);
		} else if(esp->msgin_len == 1) {
			message_out = check_singlebyte_msg(esp, eregs);
		} else if(esp->msgin_len == 2) {
			if(esp->cur_msgin[0] == EXTENDED_MESSAGE) {
				if((it+2) >= 15) {
					message_out = MESSAGE_REJECT;
				} else {
					esp->msgin_len = (it + 2);
					esp_advance_phase(SCptr, in_msgincont);
				}
			} else {
				message_out = MESSAGE_REJECT; /* foo on you */
			}
		} else {
			message_out = check_multibyte_msg(esp, eregs);
		}
	}
	if(message_out < 0) {
		return -message_out;
	} else if(message_out) {
		if(((message_out != 1) &&
		    ((message_out < 0x20) || (message_out & 0x80))))
			esp->msgout_len = 1;
		esp->cur_msgout[0] = message_out;
		esp_cmd(esp, eregs, ESP_CMD_SATN);
		esp_advance_phase(SCptr, in_msgincont);
		esp->msgin_len = 0;
	}
	esp->sreg = eregs->esp_status;
	esp->sreg &= ~(ESP_STAT_INTR);
	if((esp->sreg & (ESP_STAT_PMSG|ESP_STAT_PCD)) == (ESP_STAT_PMSG|ESP_STAT_PCD))
		esp_cmd(esp, eregs, ESP_CMD_MOK);
	if((SCptr->SCp.sent_command == in_msgindone) &&
	    (SCptr->SCp.phase == in_freeing))
		return esp_do_freebus(esp, eregs);
	return do_intr_end;
}

static int esp_do_cmdbegin(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;

	esp_advance_phase(SCptr, in_cmdend);
	esp_cmd(esp, eregs, ESP_CMD_FLUSH);
	eregs->esp_fdata = *esp->esp_scmdp++;
	esp->esp_scmdleft--;
	esp_cmd(esp, eregs, ESP_CMD_TI);
	return do_intr_end;
}

static inline int esp_do_cmddone(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	esp_cmd(esp, eregs, ESP_CMD_NULL);
	if(esp->ireg & ESP_INTR_BSERV) {
		esp_advance_phase(esp->current_SC, in_the_dark);
		return esp_do_phase_determine(esp, eregs);
	}
	ESPLOG(("esp%d: in do_cmddone() but didn't get BSERV interrupt.\n",
		esp->esp_id));
	return do_reset_bus;
}

static int esp_do_msgout(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	esp_cmd(esp, eregs, ESP_CMD_FLUSH);
	switch(esp->msgout_len) {
	case 1:
		eregs->esp_fdata = esp->cur_msgout[0];
		esp_cmd(esp, eregs, ESP_CMD_TI);
		break;

	case 2:
		if(esp->do_pio_cmds){
			eregs->esp_fdata = esp->cur_msgout[0];
			eregs->esp_fdata = esp->cur_msgout[1];
			esp_cmd(esp, eregs, ESP_CMD_TI);
		} else {
			esp->esp_command[0] = esp->cur_msgout[0];
			esp->esp_command[1] = esp->cur_msgout[1];
			esp->dma_setup(esp, (char *) esp->esp_command, 2, 0);
			esp_setcount(eregs, 2);
			esp_cmd(esp, eregs, ESP_CMD_DMA | ESP_CMD_TI);
		}
		break;

	case 5:
		if(esp->do_pio_cmds){
			esp->snip = 1;
			eregs->esp_fdata = esp->cur_msgout[0];
			eregs->esp_fdata = esp->cur_msgout[1];
			eregs->esp_fdata = esp->cur_msgout[2];
			eregs->esp_fdata = esp->cur_msgout[3];
			eregs->esp_fdata = esp->cur_msgout[4];
			esp_cmd(esp, eregs, ESP_CMD_TI);
		} else {
			esp->esp_command[0] = esp->cur_msgout[0];
			esp->esp_command[1] = esp->cur_msgout[1];
			esp->esp_command[2] = esp->cur_msgout[2];
			esp->esp_command[3] = esp->cur_msgout[3];
			esp->esp_command[4] = esp->cur_msgout[4];
			esp->snip = 1;
			esp->dma_setup(esp, (char *) esp->esp_command, 5, 0);
			esp_setcount(eregs, 5);
			esp_cmd(esp, eregs, ESP_CMD_DMA | ESP_CMD_TI);
		}
		break;

	default:
		/* whoops */
		ESPMISC(("bogus msgout sending NOP\n"));
		eregs->esp_fdata = NOP;
		esp->msgout_len = 1;
		esp_cmd(esp, eregs, ESP_CMD_TI);
		break;
	}
	esp_advance_phase(esp->current_SC, in_msgoutdone);
	return do_intr_end;
}

static inline int esp_do_msgoutdone(struct Sparc_ESP *esp, 
				    struct ESP_regs *eregs)
{
	if((esp->msgout_len > 1) && esp->dma_barrier)
		esp->dma_barrier(esp);

	if(!(esp->ireg & ESP_INTR_DC)) {
		esp_cmd(esp, eregs, ESP_CMD_NULL);
		switch(esp->sreg & ESP_STAT_PMASK) {
		case ESP_MOP:
			/* whoops, parity error */
			ESPLOG(("esp%d: still in msgout, parity error assumed\n",
				esp->esp_id));
			if(esp->msgout_len > 1)
				esp_cmd(esp, eregs, ESP_CMD_SATN);
			esp_advance_phase(esp->current_SC, in_msgout);
			return do_work_bus;

		case ESP_DIP:
			break;

		default:
			if(!fcount(eregs) &&
			   !(esp->current_SC->device->sync_max_offset))
				esp_cmd(esp, eregs, ESP_CMD_FLUSH);
			break;

		};
	} else {
		ESPLOG(("esp%d: disconnect, resetting bus\n", esp->esp_id));
		return do_reset_bus;
	}

	/* If we sent out a synchronous negotiation message, update
	 * our state.
	 */
	if(esp->cur_msgout[2] == EXTENDED_MESSAGE &&
	   esp->cur_msgout[4] == EXTENDED_SDTR) {
		esp->snip = 1; /* anal retentiveness... */
	}

	esp->prevmsgout = esp->cur_msgout[0];
	esp->msgout_len = 0;
	esp_advance_phase(esp->current_SC, in_the_dark);
	return esp_do_phase_determine(esp, eregs);
}

/* This is the second tier in our dual-level SCSI state machine. */
static inline int esp_work_bus(struct Sparc_ESP *esp, struct ESP_regs *eregs)
{
	Scsi_Cmnd *SCptr = esp->current_SC;

	ESPBUS(("esp_work_bus: "));
	if(!SCptr) {
		ESPBUS(("reconnect\n"));
		return esp_do_reconnect(esp, eregs);
	}

	switch(SCptr->SCp.phase) {
	case in_the_dark:
		ESPBUS(("in the dark\n"));
		return esp_do_phase_determine(esp, eregs);

	case in_slct_norm:
	case in_slct_stop:
	case in_slct_msg:
	case in_slct_tag:
	case in_slct_sneg:
		ESPBUS(("finish selection\n"));
		return esp_select_complete(esp, eregs);

	case in_datain:
	case in_dataout:
		ESPBUS(("finish data\n"));
		return esp_do_data_finale(esp, eregs);

	case in_msgout:
		ESPBUS(("message out "));
		return esp_do_msgout(esp, eregs);

	case in_msgoutdone:
		ESPBUS(("finish message out "));
		return esp_do_msgoutdone(esp, eregs);

	case in_msgin:
		ESPBUS(("message in "));
		return esp_do_msgin(esp, eregs);

	case in_msgincont:
		ESPBUS(("continue message in "));
		return esp_do_msgincont(esp, eregs);

	case in_msgindone:
		ESPBUS(("finish message in "));
		return esp_do_msgindone(esp, eregs);

	case in_status:
		ESPBUS(("status phase "));
		return esp_do_status(esp, eregs);

	case in_freeing:
		ESPBUS(("freeing the bus "));
		return esp_do_freebus(esp, eregs);

	case in_cmdbegin:
		ESPBUS(("begin slow cmd "));
		return esp_do_cmdbegin(esp, eregs);

	case in_cmdend:
		ESPBUS(("end slow cmd "));
		return esp_do_cmddone(esp, eregs);

	default:
		printk("esp%d: command in weird state %2x\n",
		       esp->esp_id, esp->current_SC->SCp.phase);
		return do_reset_bus;
	};
}

/* Main interrupt handler for an esp adapter. */
inline void esp_handle(struct Sparc_ESP *esp)
{
	struct ESP_regs *eregs;
	Scsi_Cmnd *SCptr;
	int what_next = do_intr_end;
#ifdef CONFIG_SCSI_SUNESP
	struct dma_registers *dregs = esp->dregs;
#endif
	eregs = esp->eregs;
	SCptr = esp->current_SC;

	if(esp->dma_irq_entry)
		esp->dma_irq_entry(esp);

	/* Check for errors. */
	esp->sreg = eregs->esp_status;
	esp->sreg &= (~ESP_STAT_INTR);

	if(esp->sreg & (ESP_STAT_SPAM)) {
		/* Gross error, could be due to one of:
		 *
		 * - top of fifo overwritten, could be because
		 *   we tried to do a synchronous transfer with
		 *   an offset greater than ESP fifo size
		 *
		 * - top of command register overwritten
		 *
v		 * - DMA setup to go in one direction, SCSI
		 *   bus points in the other, whoops
		 *
		 * - weird phase change during asynchronous
		 *   data phase while we are initiator
		 */
		ESPLOG(("esp%d: Gross error sreg=%2x\n", esp->esp_id, esp->sreg));

		/* If a command is live on the bus we cannot safely
		 * reset the bus, so we'll just let the pieces fall
		 * where they may.  Here we are hoping that the
		 * target will be able to cleanly go away soon
		 * so we can safely reset things.
		 */
		if(!SCptr) {
			ESPLOG(("esp%d: No current cmd during gross error, "
				"resetting bus\n", esp->esp_id));
			what_next = do_reset_bus;
			goto again;
		}
	}

#ifdef CONFIG_SCSI_SUNESP
	if(dregs->cond_reg & DMA_HNDL_ERROR) {
		/* A DMA gate array error.  Here we must
		 * be seeing one of two things.  Either the
		 * virtual to physical address translation
		 * on the SBUS could not occur, else the
		 * translation it did get pointed to a bogus
		 * page.  Ho hum...
		 */
		ESPLOG(("esp%d: DMA error %08lx\n", esp->esp_id,
			dregs->cond_reg));

		/* DMA gate array itself must be reset to clear the
		 * error condition.
		 */
		if(esp->dma_reset)
			esp->dma_reset(esp);

		what_next = do_reset_bus;
		goto again;
	}
#endif /* CONFIG_SCSI_SUNESP */

	esp->ireg = eregs->esp_intrpt;   /* Unlatch intr and stat regs */

	/* No current cmd is only valid this point when there are
	 * commands off the bus or we are trying a reset.
	 */
	if(!SCptr && !esp->disconnected_SC && !(esp->ireg & ESP_INTR_SR)) {
		/* Panic is safe, since current_SC is null. */
		ESPLOG(("esp%d: no command in esp_handle()\n", esp->esp_id));
		panic("esp_handle: current_SC == penguin within interrupt!");
	}

	if(esp->ireg & (ESP_INTR_IC)) {
		/* Illegal command fed to ESP.  Outside of obvious
		 * software bugs that could cause this, there is
		 * a condition with esp100 where we can confuse the
		 * ESP into an erroneous illegal command interrupt
		 * because it does not scrape the FIFO properly
		 * for reselection.  See esp100_reconnect_hwbug()
		 * to see how we try very hard to avoid this.
		 */
		ESPLOG(("esp%d: illegal command\n", esp->esp_id));

		esp_dump_state(esp, eregs);

		if(SCptr) {
			/* Devices with very buggy firmware can drop BSY
			 * during a scatter list interrupt when using sync
			 * mode transfers.  We continue the transfer as
			 * expected, the target drops the bus, the ESP
			 * gets confused, and we get a illegal command
			 * interrupt because the bus is in the disconnected
			 * state now and ESP_CMD_TI is only allowed when
			 * a nexus is alive on the bus.
			 */
			ESPLOG(("esp%d: Forcing async and disabling disconnect for "
				"target %d\n", esp->esp_id, SCptr->target));
			SCptr->device->borken = 1; /* foo on you */
		}

		what_next = do_reset_bus;
		goto again;
	}

	if(!(esp->ireg & ~(ESP_INTR_FDONE | ESP_INTR_BSERV | ESP_INTR_DC))) {
		int phase;
		if(SCptr) {
			phase = SCptr->SCp.phase;
			if(phase & in_phases_mask) {
				what_next = esp_work_bus(esp, eregs);
			} else if(phase & in_slct_mask) {
				what_next = esp_select_complete(esp, eregs);
			} else {
				ESPLOG(("esp%d: interrupt for no good reason...\n",
					esp->esp_id));
				goto esp_handle_done;
			}
		} else {
			ESPLOG(("esp%d: BSERV or FDONE or DC while SCptr==NULL\n",
				esp->esp_id));
			what_next = do_reset_bus;
			goto again;
		}
	} else if(esp->ireg & ESP_INTR_SR) {
		ESPLOG(("esp%d: SCSI bus reset interrupt\n", esp->esp_id));
		what_next = do_reset_complete;
	} else if(esp->ireg & (ESP_INTR_S | ESP_INTR_SATN)) {
		ESPLOG(("esp%d: AIEEE we have been selected by another initiator!\n",
			esp->esp_id));
		what_next = do_reset_bus;
		goto again;
	} else if(esp->ireg & ESP_INTR_RSEL) {
		if(!SCptr) {
			/* This is ok. */
			what_next = esp_do_reconnect(esp, eregs);
		} else if(SCptr->SCp.phase & in_slct_mask) {
			/* Only selection code knows how to clean
			 * up properly.
			 */
			ESPDISC(("Reselected during selection attempt\n"));
			what_next = esp_select_complete(esp, eregs);
		} else {
			ESPLOG(("esp%d: Reselected while bus is busy\n",
				esp->esp_id));
			what_next = do_reset_bus;
			goto again;
		}
	}

	/* We're trying to fight stack problems, and inline as much as
	 * possible without making this driver a mess. hate hate hate
	 * This is tier-one in our dual level SCSI state machine.
	 */
again:
	switch(what_next) {
	case do_intr_end:
		goto esp_handle_done;

	case do_work_bus:
		what_next = esp_work_bus(esp, eregs);
		break;

	case do_phase_determine:
		what_next = esp_do_phase_determine(esp, eregs);
		break;

	case do_reset_bus:
		ESPLOG(("esp%d: resetting bus...\n", esp->esp_id));
		esp->resetting_bus = 1;
		esp_cmd(esp, eregs, ESP_CMD_RS);
		goto esp_handle_done;

	case do_reset_complete:
		/* Tricky, we don't want to cause any more commands to
		 * go out until we clear all the live cmds by hand.
		 */
#ifdef CONFIG_SCSI_SUNESP
		if(!DMA_ISBROKEN(esp))
#endif
			esp->dma_ints_off(esp);
		if(esp->current_SC) {
			Scsi_Cmnd *SCptr = esp->current_SC;

#ifdef CONFIG_SCSI_SUNESP
			if(!SCptr->use_sg)
				mmu_release_scsi_one((char *)
						     SCptr->SCp.have_data_in,
						     SCptr->request_bufflen,
						     ((struct linux_sbus_device *) (esp->edev))->my_bus);
			else
				mmu_release_scsi_sgl((struct mmu_sglist *)
						     SCptr->buffer,
						     SCptr->use_sg - 1,
						     ((struct linux_sbus_device *) (esp->edev))->my_bus);
#endif
			SCptr->result = (DID_RESET << 16);
			SCptr->scsi_done(SCptr);
			cli();
		}
		esp->current_SC = NULL;
		if(esp->disconnected_SC) {
			Scsi_Cmnd *SCptr;
			while((SCptr = remove_first_SC(&esp->disconnected_SC))) {
#ifdef CONFIG_SCSI_SUNESP
				if(!SCptr->use_sg)
					mmu_release_scsi_one((char *)
							     SCptr->SCp.have_data_in,
							     SCptr->request_bufflen,
							     ((struct linux_sbus_device *) (esp->edev))->my_bus);
				else
					mmu_release_scsi_sgl((struct mmu_sglist *)
							     SCptr->buffer,
							     SCptr->use_sg - 1,
							     ((struct linux_sbus_device *) (esp->edev))->my_bus);
#endif
				SCptr->result = (DID_RESET << 16);
				SCptr->scsi_done(SCptr);
				cli();
			}
		}
		esp->resetting_bus = 0;
#ifdef CONFIG_SCSI_SUNESP
		if(!DMA_ISBROKEN(esp))
#endif
			esp->dma_ints_on(esp);

		if(esp->current_SC) {
			printk("esp%d: weird weird weird, current_SC not NULL after "
			       "SCSI bus reset.\n", esp->esp_id);
			goto esp_handle_done;
		}

		/* Now it is safe to execute more things. */
		if(esp->issue_SC){
			esp_exec_cmd(esp);
		}
		goto esp_handle_done;

	default:
		/* state is completely lost ;-( */
		ESPLOG(("esp%d: interrupt engine loses state, resetting bus\n",
			esp->esp_id));
		what_next = do_reset_bus;
		break;

	};
	goto again;

esp_handle_done:
	if(esp->dma_irq_exit)
		esp->dma_irq_exit(esp);
	return;
}

void esp_intr(int irq, void *dev_id, struct pt_regs *pregs)
{
	struct Sparc_ESP *esp;

	static int running = 0;
	int again;

	/* It is ok to take irq's on one esp while the other
	 * is amidst the processing of a reset.
	 */
	running++;
	if(running > esps_running)
		ESPLOG(("esp_intr: yieee, recursive interrupt!\n"));

	/* Handle all ESP interrupts showing */
repeat:
	again = 0;
	for_each_esp(esp) {
		if(esp->dma_irq_p(esp)) {
			again = 1;
			ESPIRQ(("I%d(", esp->esp_id));
			esp_handle(esp);
			ESPIRQ((")"));
		}
	}
	if(again)
		goto repeat;

	running--;
}

#ifdef MODULE
int init_module(void) { return 0; }
void cleanup_module(void) {}
void esp_release(void)
{
	MOD_DEC_USE_COUNT;
}
#endif
