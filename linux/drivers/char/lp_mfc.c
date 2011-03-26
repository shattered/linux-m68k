/*
 * lp driver for the parallel port of a Multiface Card III
 * 6.11.95 Joerg Dorchain (dorchain@mpi-sb.mpg.de)
 *
 * ported to 2.0.18 and modularised
 * 25.9.96 Joerg Dorchain
 *
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/lp_m68k.h>
#include <linux/lp_mfc.h>
#include <asm/amigahw.h>
#include <asm/zorro.h>
#include <asm/irq.h>
#include <asm/amigaints.h>
#include "multiface.h"
#include "mc6821.h"

static void lp_mfc_out(int,int);
static int lp_mfc_busy(int);
static int lp_mfc_pout(int);
static int lp_mfc_online(int);
static int lp_mfc_interrupt(int);

static inline struct pia *pia(int);

int lp_mfc_init(void);

static volatile int dummy; /* for trigger reads */
static int minor[MAX_LP] = { -1, -1, -1, -1, -1 };
#ifdef MODULE
static int board_key[MAX_LP];
#endif

static void lp_mfc_out(int c, int dev)
{
int wait = 0;

while (wait != lp_table[dev]->wait) wait++;
dummy = pia(dev)->prb; /* trigger read clears irq bit*/
pia(dev)->prb = c;  /* strobe goes down by hardware */
}

static int lp_mfc_busy(int dev)
{
return pia(dev)->pra&1;
}

static int lp_mfc_pout(int dev)
{
return pia(dev)->pra&2;
}

static int lp_mfc_online(int dev)
{
return pia(dev)->pra&4;
}

static int lp_mfc_interrupt(int dev)
{
int inter = pia(dev)->crb&128;

dummy = pia(dev)->prb; /* clears irq bit */
return inter;
}

static inline struct pia *pia(int dev)
{
return lp_table[dev]->base;
}

static void lp_mfc_open(void)
{
MOD_INC_USE_COUNT;
}

static void lp_mfc_release(void)
{
MOD_DEC_USE_COUNT;
}

static struct lp_struct tab[MAX_LP] = {{0,},};

int lp_mfc_init()
{
int pias;
struct pia *pp;
int key = 0;
struct ConfigDev *cd;

pias = 0;
while((key = zorro_find( MANUF_BSC2, PROD_MULTIFACE_III, 0 , key))) {
  cd = zorro_get_board( key );
  pp = (struct pia *)ZTWO_VADDR((((u_char *)cd->cd_BoardAddr)+PIABASE));
  if (pias < MAX_LP) {
	pp->crb = 0;
	pp->ddrb = 255;    /* all pins output */
	pp->crb = PIA_DDR|32|8;
	dummy = pp->prb;
	pp->crb |=(lp_irq!=0)?PIA_C1_ENABLE_IRQ:0; 
	pp->cra = 0;
	pp->ddra = 0xe0;   /* /RESET,  /DIR ,/AUTO-FEED output */
	pp->cra = PIA_DDR;
	pp->pra = 0;    /* reset printer */
	udelay(5);
	pp->pra |= 128;
	tab[pias].name="Multiface III LP";
	tab[pias].lp_out=lp_mfc_out;
	tab[pias].lp_is_busy=lp_mfc_busy;
	tab[pias].lp_has_pout=lp_mfc_pout;
	tab[pias].lp_is_online=lp_mfc_online;
	tab[pias].lp_my_interrupt=lp_mfc_interrupt;
	tab[pias].lp_ioctl=NULL;
	tab[pias].lp_open=lp_mfc_open;
	tab[pias].lp_release=lp_mfc_release;
	tab[pias].flags=LP_EXIST;
	tab[pias].chars=LP_INIT_CHAR;
	tab[pias].time=LP_INIT_TIME;
	tab[pias].wait=LP_INIT_WAIT;
	tab[pias].lp_wait_q=NULL;
	tab[pias].base=pp;
	tab[pias].type=LP_MFC;
	if ((minor[pias] = register_parallel(tab + pias, minor[pias] )) >= 0) {
	  zorro_config_board( key, 0 );
#ifdef MODULE
	  board_key[minor[pias]] = key;
#endif
	  pias++;
	}
	else
	  printk("mfc_init: cant get a minor for pia at 0x%08lx\n",(long)pp);
  }
}
if ((pias != 0) && (lp_irq != 0))
  request_irq(IRQ_AMIGA_PORTS, lp_interrupt, 0,
    "Multiface III printer", lp_mfc_init);

return (pias==0)?-ENODEV:0;
}

#ifdef MODULE
int init_module(void)
{
return lp_mfc_init();
}

void cleanup_module(void)
{
int i;

if (lp_irq)
  free_irq(IRQ_AMIGA_PORTS, lp_mfc_init);
for(i = 0; i < MAX_LP; i++)
  if ((lp_table[i] != NULL) && (lp_table[i]->type == LP_MFC)) {
    unregister_parallel(i);
    zorro_unconfig_board(board_key[i], 0);
  }
}
#endif
