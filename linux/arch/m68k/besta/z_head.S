/*
 * besta/z_head.S -- A variant of `../kernel/head.S' for compressed kernel
 *		     loading.
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

/*  at this point  %d3 contains botstr pointer...   */
text
        space   1024            /*  boot touch this area ???   */
        global _start
_start:
        mov.w   &0x2700,%sr

        /*  To make all addresses natural
           (as it wanted by cp[23]x boards...)
        */
        mov.l   &0x00000000,0x00000000

        /*  save boot string, addressed by %d0, in `saved_bootstr' area   */
        mov.l   &255,%d0
        mov.l   %d3,%a0
        lea.l   saved_bootstr,%a1
1:      mov.b   (%a0)+,(%a1)+
        dbeq    %d0,1b


        mov.l   &end,%d6
        add.l   &3,%d6
        and.w   &0xfffc,%d6
        mov.l   %d6,%a0         |  long word aligned `end'

        /*  clear/init  area for a temporary stack (1024 bytes)   */
        mov.l   &255,%d0
1:      clr.l   (%a0)+
        dbra    %d0,1b

        mov.l   %a0,%sp         |  let it be the temporary stack

        clr.l   %d0
        mov.l   %d0,%vbr

        lea.l   L%bus.w(%pc),%a1
        mov.l   %a1,0x8         |  set buserr handler

        /*  clear/init all the memory, while it is OK...   */
        mov.l   %a0,%a1
        mov.l   &0x01000000,%d0

        mov.l   %sp,%a2         |  save stack pointer
        bra     1f

3:      swap    %d0
2:      clr.l   (%a1)+
1:      dbra    %d0,2b
        swap    %d0
        dbra    %d0,3b

L%bus:
        mov.l   %a2,%sp         |  restore stack pointer

        /*  at this point %a1 consists an adress *near* the end of
           accessable memory area  (*near*, because autoincrement, etc).
            For a paranoidal reason, go back by 1k ...
        */
        mov.l   %a1,%d7
        and.w   &0xfc00,%d7
        sub.l   &0x400,%d7

        /*  at this point %d7  consists `end of accessable area' address...  */


/*
       MEMORY  LOCATION  FOR  OUR  WORK

0x00000000:             \
                        | - insn codes (.text) to movies, decompress, etc
                        |               (loaded by boot)
*end_of_code*           /
zimage_start            -
                         |
                         | - gzipped kernel image  (loaded by boot too)
                         |
                         |
zimage_end              -
zdisk_start             -
                         |
                         | - gzipped ramdisk image  (if any)
                         |                  (loaded by boot too)
                         |
zdisk_end               -
                        \
                        | - ini (.data) and unini (.bss) data
                        |         for decompress code (loaded by boot too)
%d6 (aligned `end')     /

*start_of_heap*         -
                         |
                         |
                         |
                         | - heap for malloc/free when decompress...
                         |
                         |
                         |
*end_of_heap*           -
*stack_bottom*          \
                        | - stack for decompress routine
                        |
*stack_top*             /
image_start             -
                         |
                         | - area for uncompressed kernel image
                         |   (data will be movied into `image_dest'
			 |    location)
                         |
*image_end*             -
rdsk_start              -
                         |
                         | - area for uncompressed ramdisk image
                         |                   (data will be stored hear...)
                         |
rdsk_end                -
                        \
                        | - parranoidal 1k
%d7                     /
*end_of_accessable_area*

*/

        /*  it is better to turn on insn and data caches hear   */
        mov.l   &(0x0101 + 0x0808),%d0
        mov.l   %d0,%cacr

        add.l   &0x400,%d6      |  start_of_heap

        /*  now:  d6:   start_of_free_area
                  d7:   end_of_free_area
        */

/*  First, decompress ram disk image, if it is present.   */
        mov.l   &zdisk_end,%d0
        sub.l   &zdisk_start,%d0
        cmp.l   &0x1000,%d0
        bcs     L%decompress_kernel

        /*  uncompressed ramdisk area   */
        sub.l   &ramdisk_len,%d7
        and.w   &0xfffc,%d7     | long word alignment
        mov.l   %d7,%a2         |

        sub.l   &0x400,%d7      | of cause, paranoia

        /*  stack for decompress routine   */
        mov.l   %d7,%sp

        sub.l   &0x2000,%d7     | it is enough even 4k too

        /*  now:  d6:   start_of_heap
                  d7:   end_of_heap
                    stack is OK
                  a2:   start of area to
        */

/*      CALL  THE  DECOMPRESS  ROUTINE  FOR  RAMDISK     */

        mov.l   %d7,%d0
        sub.l   %d6,%d0
        mov.l   %d0,-(%sp)              |  heap_len
        mov.l   %d6,-(%sp)              |  heap_start
        mov.l   &ramdisk_len,-(%sp)     |  to_len
        mov.l   %a2,-(%sp)              |  to
        mov.l   &zdisk_end,%d0
        sub.l   &zdisk_start,%d0
        mov.l   %d0,-(%sp)              |  from_len
        mov.l   &zdisk_start,-(%sp)     |  from

        jsr     unzip

        add.l   &24,%sp         |  for elegance

        /*  because `ramdisk_len' may be not true value, store the value
           depended by `unzip()' return...
        */
        mov.l   %a0,%d2

        /*  now we have:
            %a2:     uncompressed ramdisk image start
            %d2:     uncompressed ramdisk image end
        */

        add.l   &0x2000,%d7             |  to eat old stack area...

        bra     1f

L%decompress_kernel:
        sub.l   %a2,%a2         |  no ramdisk image
        clr.l   %d2
1:

        /*  uncompressed image area   */
        sub.l   &image_len,%d7
        and.w   &0xfffc,%d7     | long word alignment
        mov.l   %d7,%a3         | image_start

        sub.l   &0x400,%d7      | of cause, paranoia

        /*  stack for decompress routine   */
        mov.l   %d7,%sp

        sub.l   &0x2000,%d7     | it is enough even 4k too

        /*  now:  d6:   start_of_heap
                  d7:   end_of_heap
                    stack is OK
                  a3:   image_start
        */


/*      CALL  THE  DECOMPRESS  ROUTINE       */

        mov.l   %d7,%d0
        sub.l   %d6,%d0
        mov.l   %d0,-(%sp)              |  heap_len
        mov.l   %d6,-(%sp)              |  heap_start
        mov.l   &image_len,-(%sp)       |  to_len
        mov.l   %a3,-(%sp)              |  to
        mov.l   &zimage_end,%d0
        sub.l   &zimage_start,%d0
        mov.l   %d0,-(%sp)              |  from_len
        mov.l   &zimage_start,-(%sp)    |  from

        jsr     unzip

        add.l   &24,%sp         |  for elegance

/*  Now we have good image at `image_start' location.
   We should move this image into from 0x00000000 address,
   initialize .bss area, resave boot string and jump to `_start' label.
    But because our code located at the same 0x00000000 address,
   we should move ourself at other place...
*/

        mov.l   &image_dest,%d3     |  usually 0x00000000
        add.l   &image_len,%d3
        add.l   &bss_len,%d3
        add.l   &3,%d3
        and.w   &0xfffc,%d3     |   `_end' of good image & new addr for bootstr

        /*  store boot string   */
        lea     saved_bootstr,%a0       |  addr of saved boot string
        mov.l   %d3,%a1                 |  new addr for boot string

        mov.l   &255,%d1
1:      mov.b   (%a0)+,(%a1)+
        dbeq    %d1,1b

        /*  move a trampoline code to good place...  */
        lea.l   L%trampoline.w(%pc),%a0
        lea.l   L%trampoline_end.w(%pc),%a1
        sub.l   %a0,%a1         |  trampoline length in %a1

        sub.l   %a1,%d7         |  good trampoline area is ended by end_of_heap

        mov.l   %d7,%a4         |  start of good trampoline area

        mov.l   %a1,%d0
        lsr.l   &2,%d0          |  size in long words...
        sub.l   &1,%d0
        mov.l   %a4,%a1

1:      mov.l   (%a0)+,(%a1)+
        dbra    %d0,1b          |  assume the trampoline is less then 4 * 64k

        clr.l   %d0
        mov.l   %d0,%cacr       |  turn off both the cache to do no problem

        jmp     (%a4)           |  and jump for it


/*      Trampoline stuff.
     Move image to its place and initialize .bss area.

        %a3 consists image_start in memory.
*/

        align   4
L%trampoline:
        /*  it is better to turn on both the caches   */
        mov.l   &(0x0101 + 0x0808),%d0
        mov.l   %d0,%cacr

        mov.l   &image_len,%d0
        add.l   &31,%d0
        lsr.l   &5,%d0          |  per 32byte`s blocks (for speedup)

        lea.l   image_dest,%a0 |  0x00000000 as usual

        /*  and move   */
/*
        It`s nice to be important, but more impotant to be nice !!!

*/
        bra     1f
3:      swap    %d0
2:      mov.l   (%a3)+,(%a0)+
        mov.l   (%a3)+,(%a0)+
        mov.l   (%a3)+,(%a0)+
        mov.l   (%a3)+,(%a0)+
        mov.l   (%a3)+,(%a0)+
        mov.l   (%a3)+,(%a0)+
        mov.l   (%a3)+,(%a0)+
        mov.l   (%a3)+,(%a0)+
1:      dbra    %d0,2b
        swap    %d0
        dbra    %d0,3b

        /*  clear the .bss area   */
        mov.l   &image_dest,%d0
        add.l   &image_len,%d0  |  .bss start

        add.l   &3,%d0
        and.w   &0xfffc,%d0
        mov.l   %d0,%a0         |  aligned start of .bss

        /*  %d3 already consists  aligned `_end'  (start of boot string)  */
        mov.l   %d3,%d1
        sub.l   %d0,%d1
        lsr.l   &2,%d1          |  per long words

        mov.l   &0x7,%d0
        and.l   %d1,%d0
        lsr.l   &3,%d1          |  per 32byte`s blocks
        neg.l   %d0
        lea.l   1f(%pc),%a1
        jmp     (%a1,%d0.l*2)
3:      swap    %d1
2:      clr.l   (%a0)+
        clr.l   (%a0)+
        clr.l   (%a0)+
        clr.l   (%a0)+
        clr.l   (%a0)+
        clr.l   (%a0)+
        clr.l   (%a0)+
        clr.l   (%a0)+
1:      dbra    %d1,2b
        swap    %d1
        dbra    %d1,3b

        /*  send the kernel information about ramdisk representation...  */
        mov.l   %a2,rdsk_start_in_image
        mov.l   %d2,rdsk_end_in_image

/*      ...and jump to real good nice beautiful image.  Ye-a-a !!!

        %d3  already consists boot string address. Also, it is
        in Linux style  (after `_end' label immediately)
*/
        /*  turn off both the cache   */
        clr.l   %d0
        mov.l   %d0,%cacr

        jmp     start_in_image      |  may be differ with order _start ...

        nop
        nop
        nop
        nop
L%trampoline_end:

data
        /*  area to save the boot string in good place...   */
saved_bootstr:
        space   256

        /*  that is all   */
