#ifndef _TIGA_H
#define _TIGA_H

/*
 * Host control register bits
 */
#define HLT	 0x8000      /* Halt GSP */
#define CF	 0x4000      /* Cache Flush */
#define LBL	 0x2000      /* Lower Byte Last */
#define INCR	 0x1000      /* Increment on read cycles */
#define INCW	 0x0800      /* Increment on write cycles */
#define NMIMODE  0x0200      /* NMI mode (no stack usage) */
#define NMI	 0x0100      /* Non Maskable Int */
#define INTOUT	 0x0080      /* MSG Interrupt out */
#define MSGOUT	 0x0070      /* Message out */
#define INTIN	 0x0008      /* MSG Interrupt in */
#define MSGIN	 0x0007      /* Message in */

/*
 * TIGA I/O registers.
 */
#define HESYNC    0xC0000000
#define HEBLNK    0xC0000010
#define HSBLNK    0xC0000020
#define HTOTAL    0xC0000030
#define VESYNC    0xC0000040
#define VEBLNK    0xC0000050
#define VSBLNK    0xC0000060
#define VTOTAL    0xC0000070
#define DPYCTL    0xC0000080
#define DPYSTRT   0xC0000090
#define DPYINT    0xC00000A0
#define CONTROL   0xC00000B0
#define HSTDATA   0xC00000C0
#define HSTADRL   0xC00000D0
#define HSTADRH   0xC00000E0
#define HSTCTLL   0xC00000F0
#define HSTCTLH   0xC0000100
#define INTENB    0xC0000110
#define INTPEND   0xC0000120
#define CONVSP    0xC0000130
#define CONVDP    0xC0000140
#define PSIZE     0xC0000150
#define PMASK     0xC0000160
#define DPYTAP    0xC00001B0
#define HCOUNT    0xC00001C0
#define VCOUNT    0xC00001D0
#define DPYADR    0xC00001E0
#define REFCNT    0xC00001F0

struct ioregs {
	__volatile__ short hadrl;
	__volatile__ short hadrh;
	__volatile__ short data;
	__volatile__ short ctrl;
};

/*
 * Macros for access the host I/O-registers.
 */
#define gspsethadr(gsp, adr)			\
     do{					\
		gsp->hadrh = ((adr) >> 16);	\
		gsp->hadrl = ((adr) & 0xffff);	\
     }while(0)

#define gspsethadrl(gsp, adr)			\
     do{					\
		gsp->hadrl = ((adr) & 0xffff);	\
     } while (0)

#define gspgethadr(gsp)	((gsp->hadrh) << 16 | ((gsp)->hadrl))

#endif /* _TIGA_H */
