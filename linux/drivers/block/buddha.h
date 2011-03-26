/*  This file as well as the changes in ide.c are based on 
 *  linux/drivers/block/buddha.c from linux-2.1.131
 *
 *  - tested on A2000, currently only one controller is working
 *    19-01-1999 CTS
 *
 *
 *  linux/drivers/block/buddha.c -- Amiga Buddha and Catweasel IDE Driver
 *
 *      Copyright (C) 1997 by Geert Uytterhoeven
 *
 *  This driver was written by based on the specifications in README.buddha.
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of this archive for
 *  more details.
 *
 *  TODO:
 *    - test it :-)
 *    - tune the timings using the speed-register
 */
              
#ifndef _BUDDHA_H
#define _BUDDHA_H


    /*
     *  The Buddha has 2 IDE interfaces, the Catweasel has 3
     */

#define BUDDHA_NUM_HWIFS	2
#define CATWEASEL_NUM_HWIFS	3


    /*
     *  Bases of the IDE interfaces (relative to the board address)
     */

#define BUDDHA_BASE1	0x800
#define BUDDHA_BASE2	0xa00
#define BUDDHA_BASE3	0xc00


    /*
     *  Offsets from one of the above bases
     */

#define BUDDHA_DATA	0x00
#define BUDDHA_ERROR	0x06		/* see err-bits */
#define BUDDHA_NSECTOR	0x0a		/* nr of sectors to read/write */
#define BUDDHA_SECTOR	0x0e		/* starting sector */
#define BUDDHA_LCYL	0x12		/* starting cylinder */
#define BUDDHA_HCYL	0x16		/* high byte of starting cyl */
#define BUDDHA_SELECT	0x1a		/* 101dhhhh , d=drive, hhhh=head */
#define BUDDHA_STATUS	0x1e		/* see status-bits */
#define BUDDHA_CONTROL	0x11a


    /*
     *  Other registers
     */

#define BUDDHA_IRQ1	0xf00		/* MSB = 1, Harddisk is source of */
#define BUDDHA_IRQ2	0xf40		/* interrupt */
#define BUDDHA_IRQ3	0xf80


#define BUDDHA_IRQ_MR	0xfc0		/* master interrupt enable */


#endif /* _BUDDHA_H */
