#include <linux/module.h>
#include <asm/zorro.h>
#include <asm/amigatypes.h>
#include <asm/amigahw.h>
#include <asm/amigatypes.h>
#include <asm/amipcmcia.h>

extern volatile u_short amiga_audio_min_period;
extern u_short amiga_audio_period;

static struct symbol_table mach_amiga_symbol_table = {
#include <linux/symtab_begin.h>

  /*
   * Add things here when you find the need for it.
   */
  X(amiga_colorclock),
  X(amiga_chip_alloc),
  X(amiga_chip_free),
  X(amiga_chip_avail),
  X(amiga_audio_period),
  X(amiga_audio_min_period),

  X(zorro_find),
  X(zorro_get_board),
  X(zorro_config_board),
  X(zorro_unconfig_board),
  X(zorro_unused_z2ram),

#ifdef CONFIG_AMIGA_PCMCIA
  X(pcmcia_reset),
  X(pcmcia_copy_tuple),
  X(pcmcia_program_voltage),
  X(pcmcia_access_speed),
  X(pcmcia_write_enable),
  X(pcmcia_write_disable),
#endif

  /* example
  X(something_you_need),
  */


#include <linux/symtab_end.h>
};

void mach_amiga_syms_export(void)
{
	register_symtab(&mach_amiga_symbol_table);
}
