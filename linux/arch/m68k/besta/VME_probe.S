/*
 * besta/VME_probe.S -- Common assemble-coded routines for Besta configuration.
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

#ifndef __ASSEMBLY__
#define __ASSEMBLY__
#endif

#include <asm/setup.h>

/*  int VME_probe (void *addr, int *value, int rw, int size);    */

text
        global VME_probe
VME_probe:
        mov.l   %d2,-(%sp)
        mov.l   %d3,-(%sp)

        clr.l   %d1
        mov.l   16(%sp),%a0      |  value
        tst.l   %a0
        beq     1f
        mov.l   (%a0),%d1       |  value to write
1:
        mov.l   24(%sp),%d0     |  port size
        sub.l   &1,%d0
        cmp.l   &4,%d0
        bcs     1f
        mov.l   &0,%d0
1:
        tst.l   20(%sp)         |  rw
        beq     1f
        add.l   &4,%d0
1:

        mov.w   %sr,%d2         |  save_flags (flags);
        or.w    &0x0700,%sr     |  cli();

        mov.l   %vbr,%a1
        mov.l   8(%a1),%d3
        lea.l   L%buserr.w(%pc),%a0
        mov.l   %a0,8(%a1)      |  set new buserr handler
        mov.l   %sp,%a1         |  save stack pointer

        mov.l   12(%sp),%a0     |  tested addr

        |  d0  test type index
        |  a0  tested addr
        |  d1  value to write, register for read into
        |  a1  saved stack pointer
        |  d2  saved status register
        |  d3  saved order buserr handler

        jmp     L%base.b(%pc,%d0.l*4)

L%base:
        mov.b   (%a0),%d1       |  1
        bra.b   L%OK
        mov.w   (%a0),%d1       |  2
        bra.b   L%OK
        mov.b   (%a0),%d1       |  let 3 be the same as 1
        bra.b   L%OK
        mov.l   (%a0),%d1       |  4
        bra.b   L%OK
        mov.b   %d1,(%a0)       |  1
        bra.b   L%OK
        mov.w   %d1,(%a0)       |  2
        bra.b   L%OK
        mov.b   %d1,(%a0)       |  let 3 be the same as 1
        bra.b   L%OK
        mov.l   %d1,(%a0)       |  4
        bra.b   L%OK

L%buserr:
        mov.l   %a1,%sp         |  restore stack pointer
        mov.l   &-1,%d0
        bra     1f

L%OK:   clr.l   %d0
1:

        |  restore order buserr handler
        mov.l   %vbr,%a1
        mov.l   %d3,8(%a1)

        mov.w   %d2,%sr         |  restore_flags (flags);

        tst.l   %d0
        bne     1f
        tst.l   20(%sp)         |  rw
        bne     1f
        mov.l   16(%sp),%a0     |  value
        tst.l   %a0
        beq     1f
        mov.l   %d1,(%a0)       |  successful readed value...
1:
        mov.l   (%sp)+,%d3
        mov.l   (%sp)+,%d2
        rts



/*  unsigned __rw_while_OK (unsigned start_area,
                            unsigned end_area,
                            int rw,
                            int value_to_write,
                            int op_size,
                            int direction);

  Assume  %vbr is valid,
  interrupts are disabled.
  (invalidating cache is currently supported for mc680[23]0 only)
*/

text
        align   4
        global __rw_while_OK
__rw_while_OK:
        mov.l   %d2,-(%sp)
        mov.l   %a2,-(%sp)

        mov.l   28(%sp),%d0     |  op_size  (1 | 2 | 4)

        /*  convert  1|2|4  into  0|1|2   (for shifts...)   */
        cmp.b   &4,%d0
        bne     1f
        mov.l   &2,%d0
        bra     3f
1:      cmp.b   &2,%d0
        bne     2f
        mov.l   &1,%d0
        bra     3f
2:      mov.l   &0,%d0
3:
        mov.l   %d0,%d2         |  for insn index in a table (see below)

        mov.l   %vbr,%a0
        mov.l   8(%a0),%a2      |  save old buserr handler
        lea.l   L%bus.w(%pc),%a1
        mov.l   %a1,8(%a0)      |  set new buserr handler

        tst.l   32(%sp)         |  direction    (0 - forward, 1 - backward)
        bne     1f
        mov.l   12(%sp),%a0     |  start_area   (ptr init value)
        mov.l   16(%sp),%d1     |  end_area
        sub.l   %a0,%d1         |  size in bytes
        bra     2f
1:
        add.l   &4,%d2          |  index offset
        mov.l   16(%sp),%a0     |  end_area     (ptr init value)
        mov.l   %a0,%d1         |  end_area
        sub.l   12(%sp),%d1     |  size in bytes
2:
        lsr.l   %d0,%d1         |  size in operands


        tst.l   20(%sp)         |  rw:  0 - read, 1 - write
        beq     1f
        add.l   &8,%d2          |  index offset
1:

        lea.l   L%insn_place.w(%pc),%a1
        mov.w   L%insn_table.b(%pc,%d2.l*2),(%a1)
        |  invalidate insn cache should be hear.
        |  but for mc680[46]0 ...
        mov.l   %cacr,%d2
        or.w    &0x0808,%d2
        mov.l   %d2,%cacr

        mov.l   %sp,%a1         |  save stack pointer
        mov.l   24(%sp),%d2     |  value to write

        |  d0:  shift count
        |  d1:  count in operands
        |  d2:  value to write
        |  a0:  start_area ptr
        |  a1:  saved stack pointer
        |  a2:  saved buserr handler

        bra     L%start

L%insn_table:
        tst.b   (%a0)+
        tst.w   (%a0)+
        tst.l   (%a0)+
        nop
        tst.b   -(%a0)
        tst.w   -(%a0)
        tst.l   -(%a0)
        nop
        mov.b   %d2,(%a0)+
        mov.w   %d2,(%a0)+
        mov.l   %d2,(%a0)+
        nop
        mov.b   %d2,-(%a0)
        mov.w   %d2,-(%a0)
        mov.l   %d2,-(%a0)
        nop

3:      swap    %d1
L%insn_place:
1:      nop
        nop             | Why this `nop'?.. To avoid insn overloading
                        |  to correctly decrement %d1 ...
L%start:
        dbra    %d1,1b
        swap    %d1
        dbra    %d1,3b

L%bus:
        mov.l   %a1,%sp         |  restore stack pointer (if any)
        add.l   &1,%d1          |  left in operands
        lsl.l   %d0,%d1         |  left in bytes

        tst.l   32(%sp)         |  direction
        bne     1f
        mov.l   16(%sp),%d0     |  end_area
        sub.l   %d1,%d0         |  highest addr still OK    (return value)
        bra     2f
1:
        mov.l   12(%sp),%d0     |  start_area
        add.l   %d1,%d0         |  lowest addr still OK     (return value)
2:
        mov.l   %vbr,%a0
        mov.l   %a2,8(%a0)      |  restore old buserr handler

        mov.l   (%sp)+,%a2
        mov.l   (%sp)+,%d2
        rts

/*  int  __set_cpu_fpu_type (void);
  Currently implemented only for mc68020/mc68030/mc68881/mc68882 .

   Assume interrupts are disabled.
*/
text
        align   4
        global __set_cpu_fpu_type
__set_cpu_fpu_type:
        mov.l   %cacr,%d0       |  old %cacr value
        mov.l   &0x0909,%d1     |  data clear, data on, insn clear, insn on
        mov.l   %d1,%cacr
        mov.l   %cacr,%d1       |  should be 0x0101 on mc68030,
                                |  and 0x0001 on mc68020
        mov.l   %d0,%cacr       |  restore old %cacr value

        clr.l   %d0
        btst    &8,%d1
        bne     1f
        bset    &CPUB_68020,%d0
        bra     2f
1:      bset    &CPUB_68030,%d0
2:
        mov.l   %vbr,%a0
        mov.l   44(%a0),%d1     |  1111 - line
        lea.l   L%fail.w(%pc),%a1
        mov.l   %a1,44(%a0)

        mov.l   %d1,%a0         |  save old handler
        mov.l   %sp,%a1         |  save stack pointer
        fmov.l  &0,%fp0         |  set fpu into non-null state,
                                |  if no fpu, jump to L%fail
        fsave   -(%sp)
        mov.w   (%sp),%d1       |  version | length
        cmp.b   &0x18,%d1       |  mc68881 idle format
        beq     1f
        cmp.b   &0xb4,%d1       |  mc68881 busy format
        beq     1f
        bset    &FPUB_68882,%d0 |  assume else mc68882 (should be 0x38/0xd4)
        bra     2f
1:      bset    &FPUB_68881,%d0
2:
        clr.l   (%sp)           |  make null-state window
        frestore (%sp)+         |  reset fpu

L%fail:
        mov.l   %a1,%sp         |  restore stack pointer
        mov.l   %vbr,%a1
        mov.l   %a0,44(%a1)     |  restore old handler

        rts     |   now  %d0  has all the bits true
                |  to full cputype field of boot_info structure...




/*  int __mem_accs_per_clock (void *addr, int value, int rw, int size);

   The value to return is not useful immediately, it is a number of some
  memory access cycles per one clock tick. User should do two (or more) tests:
  i.e.: per one-byte and per two-byte for 8bit memory,
        per aligned and misaligned long word for 32bit memory,
        etc.
    Returned value is something about of `X + N * acc_time' , so, when you
  do additional test for `X + 2*N * acc_time' you might let the `X' go away...
   Br-r-r-r-r....
*/

text
        align   4
L%test_insns:
        tst.b   (%a0)
        tst.w   (%a0)
        tst.b   (%a0)
        tst.l   (%a0)
        mov.b   %d1,(%a0)
        mov.w   %d1,(%a0)
        mov.b   %d1,(%a0)
        mov.l   %d1,(%a0)

        global __mem_accs_per_clock
__mem_accs_per_clock:

        mov.l   16(%sp),%d0     |  port size
        sub.l   &1,%d0
        cmp.l   &4,%d0
        bcs     1f
        mov.l   &0,%d0

1:      tst.l   12(%sp)         |  0 - read, 1 - write
        beq     1f
        add.l   &4,%d0

1:      lea.l   L%test_insns.w(%pc),%a0
        mov.w   (%a0,%d0.l*2),%d0     |  testing insn

        lea.l   L%insns_start.w(%pc),%a0

        L%insns_count = (L%insns_end - L%insns_start) / 2
        mov.l   &L%insns_count-1,%d1
1:      mov.w   %d0,(%a0)+
        dbra    %d1,1b

        |  flush insn cache, because above...
        mov.l   %cacr,%d0
        or.w    &0x0008,%d0
        mov.l   %d0,%cacr

        |  OK,  testing body is filled

        mov.l   4(%sp),%a0      |  tested addr...
        mov.l   8(%sp),%d1      |  value to write, if it is write...


        |  go...
        mov.l   jiffies,%a1
        mov.l   &-1,%d0

L%insns_start:
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
L%insns_end:
        cmp.l   jiffies,%a1
        dbne    %d0,L%insns_start

        neg.l   %d0     |  this is the value to return

        rts

/*  int __mem_accs_per_cycle (void);   */
text
        align 4
        global __mem_accs_per_cycle
__mem_accs_per_cycle:
        mov.l   &L%insns_count,%d0

        rts


/*  int __try_burst_ok (unsigned long addr);   */

text
        align   4
        global __try_burst_ok
__try_burst_ok:
        mov.l   %d2,-(%sp)
        mov.l   %d3,-(%sp)

        mov.l   %cacr,%d1
        mov.l   %d1,%d0
        and.w   &0x00ff,%d0
        or.w    &0x1900,%d0     |  data on, data burst on, data flush

        mov.w   %sr,%d2         |  save_flags (flags);
        or.w    &0x0700,%sr     |  cli();

        mov.l   %vbr,%a0
        mov.l   8(%a0),%d3              |  save buserr handler
        lea.l   L%burst_fail.w(%pc),%a1
        mov.l   %a1,8(%a0)              |  new buserr handler
        mov.l   %sp,%a1         |  save stack pointer

        mov.l   12(%sp),%a0     |  addr to check

        mov.l   %d0,%cacr       |  set data burst in %cacr ...

        tst.l   (%a0)           |  test...

        mov.l   &1,%d0
        bra     1f

L%burst_fail:
        mov.l   %a1,%sp         |  restore stack pointer
        clr.l   %d0
1:
        mov.l   %d1,%cacr       |  restore %cacr

        mov.l   %vbr,%a0
        mov.l   %d3,8(%a0)      |  restore buserr handler

        mov.w   %d2,%sr         |  restore_flags (flags);

        mov.l   (%sp)+,%d3
        mov.l   (%sp)+,%d2
        rts

text
        align   4
data
        align   4

