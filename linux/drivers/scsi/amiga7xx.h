#ifndef AMIGA7XX_H

#include <linux/types.h>

int amiga7xx_detect(Scsi_Host_Template *);
const char *NCR53c7x0_info(void);
int NCR53c7xx_queue_command(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *));
int NCR53c7xx_abort(Scsi_Cmnd *);
int NCR53c7x0_release (struct Scsi_Host *);
int NCR53c7xx_reset(Scsi_Cmnd *, unsigned int);
void NCR53c7x0_intr(int irq, void *dev_id, struct pt_regs * regs);

#ifndef NULL
#define NULL 0
#endif

#ifndef CMD_PER_LUN
#define CMD_PER_LUN 3
#endif

#ifndef CAN_QUEUE
#define CAN_QUEUE 24
#endif

#if defined(HOSTS_C) || defined(MODULE)
#include <scsi/scsicam.h>

extern struct proc_dir_entry proc_scsi_amiga7xx;

#define AMIGA7XX_SCSI {/* next */               NULL,            \
		      /* usage_count */         NULL,	         \
		      /* proc_dir_entry */      NULL, \
		      /* proc_info */           NULL,            \
		      /* name */                "Amiga NCR53c710 SCSI", \
		      /* detect */              amiga7xx_detect,    \
		      /* release */             NCR53c7x0_release,   \
		      /* info */                NULL,	         \
		      /* command */             NULL,            \
		      /* queuecommand */        NCR53c7xx_queue_command, \
		      /* abort */               NCR53c7xx_abort,   \
		      /* reset */               NCR53c7xx_reset,   \
		      /* slave_attach */        NULL,            \
		      /* bios_param */          scsicam_bios_param,   \
		      /* can_queue */           24,       \
		      /* this_id */             7,               \
		      /* sg_tablesize */        127,          \
		      /* cmd_per_lun */	        3,     \
		      /* present */             0,               \
		      /* unchecked_isa_dma */   0,               \
		      /* use_clustering */      DISABLE_CLUSTERING }
#endif
#endif /* AMIGA7XX_H */
