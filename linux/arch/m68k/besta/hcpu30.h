/*
 * besta/hcpu30.h -- Common header for HCPU30 configuration sources.
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

#define XDUS_ADDR       0xffff8120
#define XDUS_VEC        0x110
#define XDUS_LEV        4

#define XDUS1_ADDR      0xffff8140
#define XDUS1_VEC       0x114
#define XDUS1_LEV       4

#define XDSK_ADDR       0xffff8160
#define XDSK_VEC        0x118
#define XDSK_LEV        4

#define X_ADDR          0xffff8100
#define XCLK_ADDR       X_ADDR
#define XCLK_VEC        0x124
#define XCLK_LEV        6

#define XFD_ADDR        0xffff8260
#define XFD_VEC         0x11c
#define XFD_LEV         4

#define XLAN_ADDR       0xffff82a0
#define XLAN_VEC        0x21c
#define XLAN_LEV        4

#define XCEN_ADDR       0xffff8280
#define XCEN_VEC        0x128
#define XCEN_LEV        3


extern int hcpu30_debug_mode;

/*  Layout of specific hcpu30 bootinfo data
  (16 bytes reserved for this in the ordinary `boot_info' Linux struct).
*/
struct hcpu30_info {
	unsigned long dip_switch;
	unsigned long flags;    /*  i.e., xlan_present   */
	unsigned long stram_size;
	unsigned long reserved;
};
#define XLAN_PRESENT    1

