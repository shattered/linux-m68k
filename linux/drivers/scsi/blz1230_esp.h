/* blz1230_esp.h: Defines and structures for the Blizzard 1230 SCSI IV driver.
 *
 * Copyright (C) 1996 Jesper Skov (jskov@cs.auc.dk)
 *
 * This file is based on cyber_esp.h (hence the occasional reference to
 * CyberStorm).
 */

#include "esp.h"

#ifndef BLZ1230_ESP_H
#define BLZ1230_ESP_H

/* The controller registers can be found in the Z2 config area at these
 * offsets:
 */
#define BLZ1230_ESP_ADDR 0x8000
#define BLZ1230_DMA_ADDR 0x10000
#define BLZ1230II_ESP_ADDR 0x10000
#define BLZ1230II_DMA_ADDR 0x10021


/* The Blizzard 1230 DMA interface
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * Only two things can be programmed in the Blizzard DMA:
 *  1) The data direction is controlled by the status of bit 31 (1 = write)
 *  2) The source/dest address (word aligned, shifted one right) in bits 30-0
 *
 * Program DMA by first latching the highest byte of the address/direction
 * (i.e. bits 31-24 of the long word constructed as described in steps 1+2
 * above). Then write each byte of the address/direction (starting with the
 * top byte, working down) to the DMA address register.
 *
 * Figure out interrupt status by reading the ESP status byte.
 */
struct blz1230_dma_registers {
	volatile unsigned char dma_addr; 	/* DMA address      [0x0000] */
	unsigned char dmapad2[0x7fff];
	volatile unsigned char dma_latch; 	/* DMA latch        [0x8000] */
};

struct blz1230II_dma_registers {
	volatile unsigned char dma_addr; 	/* DMA address      [0x0000] */
	unsigned char dmapad2[0xf];
	volatile unsigned char dma_latch; 	/* DMA latch        [0x0010] */
};

#define BLZ1230_DMA_WRITE 0x80000000

extern int blz1230_esp_detect(struct SHT *);
extern int blz1230_esp_release(struct Scsi_Host *);
extern const char *esp_info(struct Scsi_Host *);
extern int esp_queue(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *));
extern int esp_command(Scsi_Cmnd *);
extern int esp_abort(Scsi_Cmnd *);
extern int esp_reset(Scsi_Cmnd *, unsigned int);

#define SCSI_BLZ1230_ESP {                                                               \
/* struct SHT *next */                                         NULL,                   \
/* long *usage_count */                                        NULL,                   \
/* struct proc_dir_entry *proc_dir */                          &proc_scsi_esp,         \
/* int (*proc_info)(char *, char **, off_t, int, int, int) */  NULL,                   \
/* const char *name */                                         "Blizzard1230 SCSI IV", \
/* int detect(struct SHT *) */                                 blz1230_esp_detect,             \
/* int release(struct Scsi_Host *) */                          blz1230_esp_release,    \
/* const char *info(struct Scsi_Host *) */                     esp_info,               \
/* int command(Scsi_Cmnd *) */                                 esp_command,            \
/* int queuecommand(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *)) */ esp_queue,              \
/* int abort(Scsi_Cmnd *) */                                   esp_abort,              \
/* int reset(Scsi_Cmnd *) */                                   esp_reset,              \
/* int slave_attach(int, int) */                               NULL,                   \
/* int bios_param(Disk *, kdev_t, int[]) */                    NULL,                   \
/* int can_queue */                                            7,                      \
/* int this_id */                                              7,                      \
/* short unsigned int sg_tablesize */                          SG_ALL,                 \
/* short cmd_per_lun */                                        1,                      \
/* unsigned char present */                                    0,                      \
/* unsigned unchecked_isa_dma:1 */                             0,                      \
/* unsigned use_clustering:1 */                                DISABLE_CLUSTERING, }

#endif /* BLZ1230_ESP_H */
