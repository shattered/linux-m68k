#
# Makefile for the kernel character device drivers.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now inherited from the
# parent makes..
#

SUB_DIRS     := 
MOD_SUB_DIRS := $(SUB_DIRS)
ALL_SUB_DIRS := $(SUB_DIRS) ftape

#
# This file contains the font map for the default (hardware) font
#
FONTMAPFILE = cp437.uni

L_TARGET := char.a
M_OBJS   :=
L_OBJS   := tty_io.o n_tty.o tty_ioctl.o pty.o vt.o mem.o vc_screen.o \
            consolemap.o selection.o random.o

ifeq "$(ARCH)" "i386"
L_OBJS += console.o keyboard.o defkeymap.o
endif

ifeq "$(ARCH)" "alpha"
L_OBJS += console.o keyboard.o defkeymap.o
endif

ifeq "$(ARCH)" "m68k"
L_OBJS += fbmem.o keyboard.o defkeymap.o
endif

ifeq ($(CONFIG_AMIGA_GSP),y)
M = y
L_OBJS += tiga.o
else
  ifeq ($(CONFIG_AMIGA_GSP),m)
    M_OBJS += tiga.o
    MM = m
  endif
endif

ifeq ($(CONFIG_AMIGAMOUSE),y)
M = y
L_OBJS += amigamouse.o
else
  ifeq ($(CONFIG_AMIGAMOUSE),m)
    M_OBJS += amigamouse.o
    MM = m
  endif
endif

ifeq ($(CONFIG_ATARIMOUSE),y)
M = y
L_OBJS += atarimouse.o
else
  ifeq ($(CONFIG_ATARIMOUSE),m)
    M_OBJS += atarimouse.o
    MM = m
  endif
endif

ifeq ($(CONFIG_ATARI_MFPSER),y)
L_OBJS += atari_MFPser.o
S = y
else
  ifeq ($(CONFIG_ATARI_MFPSER),m)
  M_OBJS += atari_MFPser.o
  SM = y
  endif
endif

ifeq ($(CONFIG_ATARI_SCC),y)
L_OBJS += atari_SCC.o
S = y
else
  ifeq ($(CONFIG_ATARI_SCC),m)
  M_OBJS += atari_SCC.o
  SM = y
  endif
endif

ifeq ($(CONFIG_ATARI_MIDI),y)
L_OBJS += atari_MIDI.o
S = y
else
  ifeq ($(CONFIG_ATARI_MIDI),m)
  M_OBJS += atari_MIDI.o
  SM = y
  endif
endif

ifeq ($(CONFIG_AMIGA_BUILTIN_SERIAL),y)
L_OBJS += amiga_ser.o
S = y
else
  ifeq ($(CONFIG_AMIGA_BUILTIN_SERIAL),m)
  M_OBJS += amiga_ser.o
  SM = y
  endif
endif

ifeq ($(CONFIG_GVPIOEXT),y)
L_OBJS += ser_ioext.o
S = y
#  else
#  ifeq ($(CONFIG_GVPIOEXT),m)
#  M_OBJS += ser_ioext.o
#  SM = y
#  endif
endif

ifeq ($(CONFIG_MULTIFACE_III_TTY),y)
L_OBJS += ser_mfc.o
S = y
else
  ifeq ($(CONFIG_MULTIFACE_III_TTY),m)
  M_OBJS += ser_mfc.o
  SM = y
  endif
endif

ifeq ($(CONFIG_WHIPPET),y)
L_OBJS += ser_whippet.o
S = y
endif

ifeq ($(CONFIG_ASDG_DSB),y)
L_OBJS += asdg_ser.o
S=y
else
  ifeq ($(CONFIG_ASDG_DSB),m)
  M_OBJS += asdg_ser.o 
  SM = y
  endif
endif

ifeq ($(ARCH),m68k)

ifdef CONFIG_USERIAL
S = y
endif

ifdef S
LX_OBJS += m68kserial.o
else
  ifdef SM
  MX_OBJS += m68kserial.o
  endif
endif

else

ifeq ($(CONFIG_SERIAL),y)
  ifndef CONFIG_SUN_SERIAL
  LX_OBJS += serial.o
  endif
else
  ifeq ($(CONFIG_SERIAL),m)
  MX_OBJS += serial.o
  endif
endif

endif

ifeq ($(CONFIG_DIGI),y)
L_OBJS += pcxx.o
else
  ifeq ($(CONFIG_DIGI),m)
  M_OBJS += pcxx.o
  endif
endif

ifeq ($(CONFIG_CYCLADES),y)
L_OBJS += cyclades.o
else
  ifeq ($(CONFIG_CYCLADES),m)
  M_OBJS += cyclades.o
  endif
endif

ifeq ($(CONFIG_STALLION),y)
L_OBJS += stallion.o
else
  ifeq ($(CONFIG_STALLION),m)
  M_OBJS += stallion.o
  endif
endif

ifeq ($(CONFIG_ISTALLION),y)
L_OBJS += istallion.o
else
  ifeq ($(CONFIG_ISTALLION),m)
  M_OBJS += istallion.o
  endif
endif

ifeq ($(CONFIG_RISCOM8),y)
L_OBJS += riscom8.o
else
  ifeq ($(CONFIG_RISCOM8),m)
  M_OBJS += riscom8.o
  endif
endif

ifeq ($(CONFIG_SPECIALIX),y)
L_OBJS += specialix.o
else
  ifeq ($(CONFIG_SPECIALIX),m)
  M_OBJS += specialix.o
  endif
endif

ifeq ($(CONFIG_ISI),y)
L_OBJS += isicom.o
else
  ifeq ($(CONFIG_ISI),m)
  M_OBJS += isicom.o
  endif
endif

ifeq ($(CONFIG_ATIXL_BUSMOUSE),y)
M = y
L_OBJS += atixlmouse.o
else
  ifeq ($(CONFIG_ATIXL_BUSMOUSE),m)
  M_OBJS += atixlmouse.o
  MM = m
  endif
endif

ifeq ($(CONFIG_BUSMOUSE),y)
M = y
L_OBJS += busmouse.o
else
  ifeq ($(CONFIG_BUSMOUSE),m)
  M_OBJS += busmouse.o
  MM = m
  endif
endif

ifeq ($(ARCH),m68k)
  ifeq ($(CONFIG_PRINTER),y)
  L = y
  L_OBJS += lp_intern.o
  else
    ifeq ($(CONFIG_PRINTER),m)
    M_OBJS += lp_intern.o
    LM = m
    endif
  endif
  ifeq ($(CONFIG_MULTIFACE_III_LP),y)
  L = y
  L_OBJS += lp_mfc.o
  else 
    ifeq ($(CONFIG_MULTIFACE_III_LP),m)
    M_OBJS += lp_mfc.o
    LM = m
    endif
  endif
  ifdef L
    LX_OBJS += lp_m68k.o
  else
    ifdef LM
      MX_OBJS += lp_m68k.o
    endif
  endif
else
  ifeq ($(CONFIG_PRINTER),y)
  L_OBJS += lp.o
  else
    ifeq ($(CONFIG_PRINTER),m)
    M_OBJS += lp.o
    endif
  endif
endif

ifeq ($(CONFIG_MS_BUSMOUSE),y)
M = y
L_OBJS += msbusmouse.o
else
  ifeq ($(CONFIG_MS_BUSMOUSE),m)
  M_OBJS += msbusmouse.o
  MM = m
  endif
endif

ifeq ($(CONFIG_PSMOUSE),y)
M = y
L_OBJS += psaux.o
else
  ifeq ($(CONFIG_PSMOUSE),m)
  M_OBJS += psaux.o
  MM = m
  endif
endif

ifeq ($(CONFIG_UMISC),y)
# To support third-party modules, misc.c must reside in the kernel
M = y
endif

ifeq ($(CONFIG_SOFT_WATCHDOG),y)
M = y
L_OBJS += softdog.o
else
  ifeq ($(CONFIG_SOFT_WATCHDOG),m)
  M_OBJS += softdog.o
  MM = m
  endif   
endif

ifeq ($(CONFIG_PCWATCHDOG),y)
M = y
L_OBJS += pcwd.o
else
  ifeq ($(CONFIG_PCWATCHDOG),m)
  M_OBJS += pcwd.o
  MM = m
  endif
endif

ifdef CONFIG_SUN_MOUSE
M = y
endif

ifeq ($(CONFIG_WDT),y)
M = y
L_OBJS += wdt.o
else
  ifeq ($(CONFIG_WDT),m)
    M_OBJS += wdt.o
    MM = m
  endif
endif

ifeq ($(CONFIG_RTC),y)
M = y
L_OBJS += rtc.o
endif

ifdef CONFIG_QIC02_TAPE
L_OBJS += tpqic02.o 
endif

ifeq ($(CONFIG_FTAPE),y)
SUB_DIRS += ftape
L_OBJS += ftape/ftape.o
else
  ifeq ($(CONFIG_FTAPE),m)
  MOD_SUB_DIRS += ftape
  endif
endif

ifdef CONFIG_APM
LX_OBJS += apm_bios.o
M = y
endif

ifdef CONFIG_H8
LX_OBJS += h8.o
M = y
endif

ifdef M
LX_OBJS += misc.o
else
  ifdef MM
  MX_OBJS += misc.o
  endif
endif
  
ifeq ($(CONFIG_SCC),y)
L_OBJS += scc.o
else
  ifeq ($(CONFIG_SCC),m)
  M_OBJS += scc.o
  endif
endif  

ifneq "$(ARCH)" "m68k" 
ifdef CONFIG_TGA_CONSOLE
L_OBJS += tga.o
  ifdef CONFIG_VGA_CONSOLE
    L_OBJS += vga.o vesa_blank.o
  endif
else
 ifndef CONFIG_SUN_CONSOLE
 L_OBJS += vga.o vesa_blank.o
 endif
endif
endif

include $(TOPDIR)/Rules.make

fastdep: uni_hash.tbl

consolemap.o:

conmakehash: conmakehash.c
	$(HOSTCC) -o conmakehash conmakehash.c

uni_hash.tbl: $(FONTMAPFILE) conmakehash
	./conmakehash $(FONTMAPFILE) > uni_hash.tbl
