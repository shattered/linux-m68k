/*
 * Defines for the GVP IO-Extender (duart, midi, parallel board), for
 * the Amiga range of computers.
 *
 * This code is (C) 1995 Jes Sorensen (jds@kom.auc.dk).
 */

#ifndef _IO_EXTENDER_H_
#define _IO_EXTENDER_H_

#include "16c552.h"

#define MAX_IOEXT 5 /*
		     * The maximum number of io-extenders is 5, as you
		     * can't have more than 5 ZII boards in any Amiga.
		     */

#define UART_CLK 7372800

#define IOEXT_BAUD_BASE (UART_CLK / 16)

#define curruart(info) ((struct uart_16c550 *)(info->port))

#define ser_DTRon(info)  curruart(info)->MCR |=  DTR
#define ser_RTSon(info)  curruart(info)->MCR |=  RTS
#define ser_DTRoff(info) curruart(info)->MCR &= ~DTR
#define ser_RTSoff(info) curruart(info)->MCR &= ~RTS


/*
 * This is the struct describing the registers on the IO-Extender.
 * NOTE: The board uses a dual uart (16c552), which should be equal to
 * two 16c550 uarts.
 */
typedef struct {
  u_char gap0[0x41];
  volatile u_char CNTR;        /* GVP DMAC CNTR (status register)     */
  u_char gap1[0x11e];
  struct uart_16c550 uart0;    /* The first uart                      */
  u_char gap2[0xf0];
  struct uart_16c550 uart1;    /* The second uart                     */
  u_char gap3[0xf0];
  struct IOEXT_par par;        /* The parallel port                   */
  u_char gap4[0xfb];
  volatile u_char CTRL;       /* The control-register on the board   */
} IOEXT_struct;

/*
 * CNTR defines (copied from the GVP SCSI-driver file gvp11.h
 */
#define GVP_BUSY	(1<<0)
#define GVP_IRQ_PEND	(1<<1)
#define GVP_IRQ_ENA 	(1<<3)
#define GVP_DIR_WRITE   (1<<4)

/*
 * CTRL defines 
 */
#define PORT0_MIDI   (1<<0)  /* CLR = DRIVERS         SET = MIDI      */
#define PORT1_MIDI   (1<<1)  /* CLR = DRIVERS         SET = MIDI      */
#define PORT0_DRIVER (1<<2)  /* CLR = RS232,          SET = MIDI      */
#define PORT1_DRIVER (1<<3)  /* CLR = RS232,          SET = MIDI      */
#define IRQ_SEL      (1<<4)  /* CLR = INT2,           SET = INT6      */
#define ROM_BANK_SEL (1<<5)  /* CLR = LOW 32K,        SET = HIGH 32K  */
#define PORT0_CTRL   (1<<6)  /* CLR = RTSx or RXRDYx, SET = RTSx ONLY */
#define PORT1_CTRL   (1<<7)  /* CLR = RTSx or RXRDYx, SET = RTSx ONLY */

#endif
