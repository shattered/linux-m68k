#
# Makefile for i386-specific library files..
#

L_TARGET = lib.o
L_OBJS  = checksum.o cksum_support.o

${L_TARGET}: $(L_OBJS)
	$(LD) -r -o ${L_TARGET} $(L_OBJS)

fastdep:
