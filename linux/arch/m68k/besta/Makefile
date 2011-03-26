# 
# Makefile for Linux arch/m68k/besta source directory.
#
# Copyright 1996, 1997	    Dmitry K. Butskoy
#			    <buc@citadel.stu.neva.ru>
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2, or (at your option) any
# later version.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile...
#
# After `vmlinux' is done, you may invoke `make zlinux' in top directory
# to make compressed kernel loading. If you want to accompany kernel
# with initial ramdisk image (initrd), specify a (gzipped) image,
# for example:  make zlinux ZDISK=arch/m68k/besta/image.gz
#

ifdef CONFIG_HCPU30

BASE_OBJS =  hcpu30.o xdus.o xclk.o xdsk.o xlanc.o xfd.o xcen.o
BASE_HEADERS = hcpu30.h

else

BASE_OBJS =  cp31.o cpsio.o cpcen.o
BASE_HEADERS = cp31.h

endif

VME_OBJS = hcww.o ios.o cww.o md10.o lance.o cwn.o
BESTA_OBJS = VME.o VME_probe.o scsi.o scsi_disk.o scsi_tape.o scsi_sense.o
DRIVERS_OBJS = sockm.o clone.o stream.o sockmod.o
SVR3_PERS_OBJS =  svr3.o svr3sys.o


O_TARGET = besta.o
O_OBJS = $(BASE_OBJS) $(BESTA_OBJS) $(SVR3_PERS_OBJS)
L_TARGET = VME.a
L_OBJS = $(VME_OBJS) $(DRIVERS_OBJS)


include $(TOPDIR)/Rules.make


$(O_TARGET) VME.o:	$(TOPDIR)/include/linux/autoconf.h


ifdef kaka
cwn.o:  cwn_before.o cwn_after.o cwn_image.o
	$(LD) -r -o cwn.o cwn_before.o cwn_after.o cwn_image.o

cwn_before.o:   besta.h
cwn_after.o:    cwn_soft.h besta.h scsi.h

cwn_soft.o:     cwn_soft.c cwn_soft.h
	$(CC) -Wall -Wstrict-prototypes -O2 -m68010 -pipe -c cwn_soft.c

cwn_image.o:    cwn_soft.o
	do_cwn_image cwn_soft.o cwn_image.o
endif

#
# Stuff for making compressed kernel loading.
# May be accompanied with compressed initial root disk.
#

SRC_KERN = $(TOPDIR)/vmlinux
ZDISK = /dev/null
TMP = ./zimageDATA

# Note: `-R $(SRC_KERN)' must be *last*!
#
zlinux: z_head.o unzip.o $(TOPDIR)/arch/m68k/lib/lib.a\
				      zimage.o zdisk.o zspecs.ld
	$(LD) -T zspecs.ld -s -o $(TOPDIR)/zlinux z_head.o unzip.o \
	     $(TOPDIR)/arch/m68k/lib/lib.a zimage.o zdisk.o -R $(SRC_KERN)

z_head.o: z_head.S

unzip.o: unzip.c

zimage.o: $(SRC_KERN)
	$(LD) -o $(TMP) -r -oformat binary $(SRC_KERN)
	gzip -9 $(TMP)
	$(LD) -o zimage.o -r -iformat binary $(TMP).gz
	rm -f $(TMP).gz

zdisk.o: dummy
	$(LD) -o zdisk.o -r -iformat binary $(ZDISK)

