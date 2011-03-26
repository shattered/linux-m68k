/*
 * besta/cp31.h -- Common header for CP20/CP30/CP31 configuration sources.
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

#define PIT_ADDR        0xff800c00
#define BIM_ADDR        0xff800800
#define RTC_ADDR        0xff800a00
#define SECRET_ADDR     0xff800e00
#define CLK_VEC         0x49
#define CLK_LEV         6

#define SIO_ADDR        0xff800000
#define SIO_VEC         0x44
#define SIO_LEV         3

#define SIO1_ADDR       0xff800200
#define SIO1_VEC        0x45
#define SIO1_LEV        3

#define SIO2_ADDR       0xff800600
#define SIO2_VEC        0x46
#define SIO2_LEV        3

#define CEN_ADDR        0xff800200
#define CEN_VEC         0x45
#define CEN_LEV         2

/*  Layout of specific cp3x/cp2x bootinfo data
  (16 bytes reserved for this in the ordinary `boot_info' Linux struct).
*/
struct cp31_info {
	unsigned long dip_switch;
	unsigned char num_sio;
	unsigned char cen_present;
	unsigned char rtc_present;
	unsigned char board_type;
	unsigned long stram_size;
	unsigned long slave_stram_size;
};

/*  values for `board_type' field   */
#define CP31_BOARD_UNKNOWN      0
#define CP31_BOARD_CP20         1
#define CP31_BOARD_CP30         2
#define CP31_BOARD_CP31         3


/*  structure for correct access to odd ports...   */
struct odd {
	char          r0;
	unsigned char reg;
};

/*  Sigh, this is mc68153  */
struct bim {
	struct odd cntrl[4];
	struct odd vect[4];
};

/*  Sigh, this is mc68230  */
struct pit {
	unsigned char gen_cntl;
	unsigned char serv_req;
	unsigned char a_dir;
	unsigned char b_dir;
	unsigned char c_dir;
	unsigned char vect;
	unsigned char a_cntl;
	unsigned char b_cntl;
	unsigned char a_data;
	unsigned char b_data;
	unsigned char a_alt;
	unsigned char b_alt;
	unsigned char c_data;
	unsigned char status;
	char          re;
	char          rf;
	unsigned char timer_cntl;
	unsigned char timer_vect;
	unsigned char timer_start0;
	unsigned char timer_start1;
	unsigned char timer_start2;
	unsigned char timer_start3;
	unsigned char timer_count0;
	unsigned char timer_count1;
	unsigned char timer_count2;
	unsigned char timer_count3;
	unsigned char timer_status;
};
