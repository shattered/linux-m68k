/*
 * besta/zspecs.ld -- GNU linker specifications to build boot loadable image
 *		      with compressed kernel and (optional) ramdisk.
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

SECTIONS {
        .text 0x0000 : {
             *(.text)
             . = ALIGN(4);
	     etext = .;
	     _etext = .;
        }
        .data ALIGN(4) : {
             zimage_start = .;
             zimage.o(.data)
             zimage_end = .;
             . = ALIGN(4);
             zdisk_start = .;
             zdisk.o(.data)
             zdisk_end = .;
             . = ALIGN(4);
             *(.data)
	     edata = .;
	     _edata = .;
        } 
	.bss SIZEOF(.data) + ADDR(.data) : {
	     *(.bss)
	     *(COMMON)
	     end = ALIGN(8);
	     _end = ALIGN(8);
	}
}

/*  Rvalues come from ordinary `vmlinux' by `-R vmlinux' linker option.  */
image_dest = kernel_image_start;
image_len = kernel_image_end - kernel_image_start;
bss_len = kernel_bss_len;
start_in_image = kernel_start;
rdsk_start_in_image = initrd_start;
rdsk_end_in_image = initrd_end;
ramdisk_len = 0x00400000;	/*  4M   */
