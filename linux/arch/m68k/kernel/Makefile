#
# Makefile for the linux kernel.
#
# Note! Dependencies are done automagically by 'make dep', which also
# removes any old dependencies. DON'T put your own dependencies here
# unless it's something special (ie not a .c file).
#
# Note 2! The CFLAGS definitions are now in the main makefile...

.S.o:
	$(CC) -D__ASSEMBLY__ -traditional -c $< -o $*.o

all: kernel.o head.o
O_TARGET := kernel.o

ifndef CONFIG_BESTA
O_OBJS := entry.o process.o traps.o ints.o signal.o ptrace.o \
	  setup.o bios32.o sys_m68k.o console.o time.o ksyms.o
else
O_OBJS := entry.o process.o traps.o ints.o signal.o ptrace.o \
	  setup.o bios32.o sys_m68k.o time.o ksyms.o
endif

head.o: head-besta.S
	$(CC) -D__ASSEMBLY__ -traditional -c head-besta.S -o head.o

include $(TOPDIR)/Rules.make
