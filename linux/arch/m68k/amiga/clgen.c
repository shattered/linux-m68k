/*
 * arch/m68k/amiga/clgen.c - kernel-based graphic board driver for
 *                            Cirrus-Logic based VGA boards
 *
 * This driver currently supports: Piccolo, Picasso II, GVP Spectrum, 
 * Piccolo SD64, Picasso IV
 *
 *    Copyright (c) 1997,1998 Frank Neumann
 *
 *
 *  History:
 *  - 16-Mar-1997: v1.0 First public release
 *
 *  - 22-Oct-1997: v1.1 Added basic console support, P4 support (not working)
 *                      This was never released to the public
 *  - 19-Dec-1997: v1.2 Public release
 *			working console support
 *                      working P4 support
 *			hardware-accelerated RectFill/BitBLT on consoles
 *			(only in 8 bits right now)
 *			correct standard VGA colour table
 *			1 bit support working again
 *			uses the FB_ACTIVATE_* flags correctly now
 *			correct behaviour when used with fbset
 *
 *  - 12-Jan-1998: v1.3 Public release
 *			correct CLUT handling for Picasso IV (was BGR, not RGB)
 *			removed left-over printk for Picasso II
 *			now accepts monitorcap: and mode: on command line
 *			correct "scrolling" when height or width = 0
 * 			(though that's been done in console/fbcon.c)
 * - 29-Jan-1998: v1.3b Added support for P4 in Zorro II (A2000)
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of this archive
 * for more details.
 *
 */


/* Please ignore the following lines of german/english notices. :-) */

/*
	alle moeglichen Checks fuer die anderen Karten aktivieren
	Beschleunigung  fuer 1 Bit
	hi/true color modes wieder aktivieren (zumindest 24/32 bpp)
	
	- need to make a copy of the screenmode database for each new board
	- get rid of the ugly DELAY stuff (use jiffies instead)
	"weiches" Blanken?

	### Noch zu ueberpruefen: Aenderungen vom 4. Okt:
		VSSM2 nicht mehr in der +$fff-Translation bei RGen/WGen
		WClut/RClut: Auch +$fff beruecksichtigen

	eventuell bei SRF (SEQ_ID_DRAM_CONTROL) bei 1 Bit Tiefe d0 statt b0

	Bugfix fuer 24bpp-Mode (clock * / % 3etc.)
	Bugfix fuer 320er-Modus: letzte Spalte..
*/

/*** INCLUDES ***/
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/config.h>
#include <linux/types.h>
#include <asm/segment.h>
#include <asm/pgtable.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/malloc.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/console.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <asm/amigaints.h>
#include <asm/amigahw.h>

#include "clgen.h"

/*** DEFINES ***/
#define CLGEN_VERSION "1.3b (29-Jan-98)"
/* #define DEBUG if(1) */
#define DEBUG if(0)

/* this is not nice, but I just need a delay of a few cycles */
#define DELAY { int del; for (del = 0; del < 100; del++) ; }
#define DELAY2(n) {int k; for(k = 0; k < n; k++) {long x; x = 1234567/1234;}}
#define arraysize(x)    (sizeof(x)/sizeof(*(x)))

/* copied from drivers/char/fbmem.c :-\ */
#define FB_MODES_SHIFT 5
#define GET_INODE(i) MKDEV(FB_MAJOR, (i) << FB_MODES_SHIFT)
#define GET_FB_IDX(node) (MINOR(node) >> FB_MODES_SHIFT)
#define GET_FB_VAR_IDX(node) (MINOR(node) & ((1 << FB_MODES_SHIFT)-1))

/* board types */
#define BT_NONE     0
#define BT_SD64     1
#define BT_PICCOLO  2
#define BT_PICASSO  3
#define BT_SPECTRUM 4
#define BT_PICASSO4 5

#define MAX_NUM_BOARDS 7

#define TRUE  1
#define FALSE 0 

/*** STRUCTS ***/
/* I need one of these per board */
struct clgenstruct {
	int fbnum;  /* fb index associated with this board */
	int node; /* return value from register_framebuffer() */
	int boardtype; /* type of this board, for simple lookups */
	unsigned char *RegBase;   /* physical start address of register space */
	unsigned char *VirtRegBase; /* virt. address of register space */
	/* although each board has 2 autoconfig nodes (I/O registers and DRAM), */
	/* I only need one kernel_map for the DRAM, as the I/O stuff is already */
	/* mapped in (it's in Zorro-II space) */
	unsigned long PhysAddr;   /* physical start address of board (DRAM) */
	unsigned char *VirtAddr;  /* virtual address of board after kernel_map */
	unsigned char *VirtRAMAddr; /* virtual start address of DRAM area */
	unsigned long size;       /* length of board address space in bytes */
	int smallboard;           /* board has small/large mem, shortcut */
	int p4flag;		  /* auxiliary flag for P4 in Z2 mode */
	unsigned char SFR;        /* shadow of "special function register" */
	/* This maybe not really belongs here, but for now... */
/* ###	int xres, yres; */
	int xoffset, yoffset;
/* ###	int vxres, vyres; */
	int line_length;
	int visual;
	int bpp;
	int type;
	long mon_hfmin, mon_hfmax, mon_vfmin, mon_vfmax;
	struct fb_var_screeninfo currentinfo;
};

typedef struct _clgen
{
    long HorizTotal, HorizDispEnd, HorizBlankStart;
	long HorizBlankEnd, HorizSyncStart, HorizSyncEnd;
	long VerticalTotal, VerticalSyncStart, VerticalSyncEnd;
	long VerticalDispEnd, VerticalBlankStart, VerticalBlankEnd;
} CLGenData;

/* ### need a copy of the mode database for each board! */
static struct fb_var_screeninfo clgen_fb_predefined[] = {
	/* autodetect mode */
	{ 0 },

	/* 640x480, 31.25 kHz, 60 Hz, 25 MHz PixClock */
	/* 
		Modeline from XF86Config:
		ModeLine "640x480"    25  640  672  768  800  480  490  492  525
	*/
	{ 
		640, 480, 640, 480, 0, 0, 8, 0, 
		{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0}, 
		0, 0, -1, -1, FB_ACCEL_CLGEN, 40000, 32, 32, 33, 10, 96, 2,
		FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	},

	/* 800x600, 48 kHz, 72 Hz, 50 MHz PixClock */
	/* 
		Modeline from XF86Config:
		ModeLine "800x600"    50  800  856  976 1040  600  637  643  666
	*/
	{ 
		800, 600, 800, 600, 0, 0, 8, 0, 
		{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0}, 
		0, 0, -1, -1, FB_ACCEL_CLGEN, 20000, 64, 56, 23, 37, 120, 6,
		FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	},

	/* 1024x768, 55.8 kHz, 70 Hz, 80 MHz PixClock */
	/* 
		Modeline from XF86Config:
		Mode "1024x768"
		DotClock 80
		HTimings 1024 1136 1340 1432 VTimings 768 770 774 805
	*/
	{ 
		1024, 768, 1024, 768, 0, 0, 8, 0, 
		{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0}, 
		0, 0, -1, -1, FB_ACCEL_CLGEN, 12500, 92, 112, 31, 2, 204, 4,
		FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	},

	/* 320x200, 31.4 kHz, 70 Hz, 25 MHz PixClock */
	/*
		self-invented mode, uses DoubleScan and autoscroll
	*/
	{
		320, 200, 320, 200, 0, 0, 8, 0, 
		{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0}, 
		0, 0, -1, -1, FB_ACCEL_CLGEN, 39726, 40, 16, 16, 7, 24, 1,
		FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, 
		FB_VMODE_DOUBLE | FB_VMODE_CLOCK_HALVE
	},

	/* 1024x768i, 31.5 kHz, 77 Hz i, 44.9 MHz PixClock */
	/*
		more-or-less self-invented mode
	*/
	{
		1024, 768, 1024, 768, 0, 0, 8, 0, 
		{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
		0, 0, -1, -1, FB_ACCEL_CLGEN, 22268, 92, 112, 32, 9, 204, 8, 
		FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, 
		FB_VMODE_INTERLACED
	},

	/* 1024x768i, 46.5 kHz, 113 Hz i, 66.2 MHz PixClock */
	/*
		almost same as above, but higher vertical refresh rate
	*/
	{
		1024, 768, 1024, 768, 0, 0, 8, 0, 
		{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
		0, 0, -1, -1, FB_ACCEL_CLGEN, 15102, 92, 112, 32, 9, 204, 8, 
		FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, 
		FB_VMODE_INTERLACED
	},

	/* 1280x1024i, 51 kHz, 77 Hz i, 80 MHz PixClock */
	/*
		Mode "1280x1024"
		DotClock 80
		HTimings 1280 1296 1512 1568 VTimings 1024 1025 1037 1165
		Flags "Interlace"
	*/
	{
		1280, 1024, 1280, 1024, 0, 0, 8, 0, 
		{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
		0, 0, -1, -1, FB_ACCEL_CLGEN, 12500, 56, 16, 128, 1, 216, 12,
		FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT, 
		FB_VMODE_INTERLACED
	},

	/*
	 *    Dummy Video Modes
	 */

	{ 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, },
	{ 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, },

	/*
	 *    User Defined Video Modes
	 */

	{ 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }
};

#define NUM_TOTAL_MODES		arraysize(clgen_fb_predefined)

   /*
    *    Default Colormaps
    */

/* the default colour table, for VGA+ colour systems */
static u_short red16[] = 
      {	0x0000, 0x0000, 0x0000, 0x0000, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa,
	0x5555, 0x5555, 0x5555, 0x5555, 0xffff, 0xffff, 0xffff, 0xffff};
static u_short green16[] = 
      {	0x0000, 0x0000, 0xaaaa, 0xaaaa, 0x0000, 0x0000, 0xaaaa, 0xaaaa,
	0x5555, 0x5555, 0xffff, 0xffff, 0x5555, 0x5555, 0xffff, 0xffff};
static u_short blue16[] = 
      {	0x0000, 0xaaaa, 0x0000, 0xaaaa, 0x0000, 0xaaaa, 0x0000, 0xaaaa,
	0x5555, 0xffff, 0x5555, 0xffff, 0x5555, 0xffff, 0x5555, 0xffff};

static u_short red2[]=
        { 0x0000,0xaaaa};
static u_short green2[]=
        { 0x0000,0xaaaa};
static u_short blue2[]=
        { 0x0000,0xaaaa};

static struct fb_cmap default_16_colors =
	{ 0, 16, red16, green16, blue16, NULL };
static struct fb_cmap default_2_colors = 
	{ 0, 2, red2, green2, blue2, NULL };


/*** GLOBAL VARIABLES ***/
struct clgenstruct clboards[MAX_NUM_BOARDS];
static int currcon = 0;
static int num_inited = 0; /* number of boards currently init'ed */
static int g_slotnum = -1;
static int g_startmode = 1;  /* initial video mode */
static struct display disp[MAX_NR_CONSOLES];
static struct fb_info fb_info;

static struct fb_hwswitch {
   /* Display Control */
   int (*getcolreg)(u_int regno, u_int *red, u_int *green, u_int *blue,
                    u_int *transp);
   int (*setcolreg)(u_int regno, u_int red, u_int green, u_int blue,
                    u_int transp);
} *fbhw;

/*** EXTERNAL STUFF ***/
extern unsigned long kernel_map(unsigned long paddr, unsigned long size,
	int nocacheflag, unsigned long *memavailp);


/*** PROTOTYPES ***/
void 			init_vgachip( int fbidx );
void			WGen( int regnum, unsigned char val );
unsigned char 		RGen( int regnum );
void 			WSeq( unsigned char regnum, unsigned char val );
unsigned char 		RSeq( unsigned char regnum );
void 			WCrt( unsigned char regnum, unsigned char val );
unsigned char		RCrt( unsigned char regnum );
void			WGfx( unsigned char regnum, unsigned char val );
unsigned char		RGfx( unsigned char regnum );
void			WAttr( unsigned char regnum, unsigned char val );
void			AttrOn( void );
unsigned char		RAttr( unsigned char regnum );
void			WHDR( unsigned char val );
unsigned char		RHDR( void );
void			WSFR( unsigned char val );
void			WSFR2( unsigned char val );
void			WClut( unsigned char regnum, unsigned char red, unsigned char green, unsigned char blue );
void			RClut( unsigned char regnum, unsigned char *red, unsigned char *green, unsigned char *blue );
void			SelectMap( unsigned char mapnum );
void	bestclock( long freq, long *best, long *nom, long *den, long *div, long maxfreq );
static int clgen_fb_get_fix(struct fb_fix_screeninfo *fix, int con, int fbidx);
static int clgen_fb_get_var(struct fb_var_screeninfo *var, int con, int fbidx);
static int clgen_fb_set_var(struct fb_var_screeninfo *var, int con, int fbidx);
static void clgen_set_var( struct fb_var_screeninfo *var, int clearflag );

static struct fb_cmap *get_default_colormap( int len );
static int do_fb_get_cmap( struct fb_cmap *cmap, struct fb_var_screeninfo *var,
                          int kspc );
static int do_fb_set_cmap( struct fb_cmap *cmap, struct fb_var_screeninfo *var,
                          int kspc );
static void do_install_cmap( int con );
static void memcpy_fs( int fsfromto, void *to, void *from, int len );
static void copy_cmap( struct fb_cmap *from, struct fb_cmap *to, int fsfromto );
static int alloc_cmap( struct fb_cmap *cmap, int len, int transp );
static int clgen_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                         u_int transp);
static int clgen_getcolreg(u_int regno, u_int *red, u_int *green, u_int *blue,
                         u_int *transp);

static void clgen_fb_set_disp( int con, int fbidx );

static int clgen_fb_get_cmap(struct fb_cmap *cmap, int kspc, int con, int fbidx);
static int clgen_fb_set_cmap(struct fb_cmap *cmap, int kspc, int con, int fbidx);

void clgen_WaitBLT( void );
void clgen_BitBLT (u_short curx, u_short cury, u_short destx, u_short desty,
		u_short width, u_short height, u_short line_length);
void clgen_RectFill (u_short x, u_short y, u_short width, u_short height,
                     u_char color, u_short line_length);

static int clgen_fb_ioctl(struct inode *inode, struct file *file,
    unsigned int cmd, unsigned long arg, int con, int fbidx);
static int clgen_fb_pan_display(struct fb_var_screeninfo *var, int val, int fbidx);
static int clgen_get_monitorspec(struct fb_monitorspec *spec, int val, int fbidx);
static int clgen_put_monitorspec(struct fb_monitorspec *spec, int val, int fbidx);
int					find_clboard( int fbidx );
struct fb_info 				*clgen_fb_init( long *mem_start );
static int clgen_switch( int );
static int clgen_updatevar( int );
static void clgen_blank( int );

int clgen_probe( void );
void clgen_video_setup( char *options, int *ints );
static char *strtoke( char *s, const char *ct );

static struct fb_hwswitch clgen_hwswitch = {
   clgen_getcolreg, clgen_setcolreg
};

/********************************************/
/* init_vgachip() - initialize the VGA chip */
/********************************************/

void init_vgachip(int fbidx)
{
	int slotnum;

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		printk(KERN_ERR "clgen: Warning: Board not found in init_vgachip)!!\n");
		return;
	}

	g_slotnum = slotnum;

	/* reset board globally */
	switch(clboards[slotnum].boardtype)
	{
		case BT_SD64:
			WSFR(0x1f);
			DELAY
			WSFR(0x4f); /* was: 6f (but no "video on" anymore) */
			clboards[slotnum].SFR = 0x4f;
			DELAY
			break;

		case BT_PICCOLO:
			WSFR(0x01);
			DELAY
			WSFR(0x51);
			clboards[slotnum].SFR = 0x51;
			DELAY
			break;

		case BT_PICASSO:
			WSFR2(0xff);
			clboards[slotnum].SFR = 0xff;
			DELAY
			break;

		case BT_SPECTRUM:
			WSFR(0x1f);
			DELAY
			WSFR(0x4f);
			clboards[slotnum].SFR = 0x4f;
			DELAY
			break;

		case BT_PICASSO4:
			WCrt(CRT51, 0x00); /* disable flickerfixer */
			DELAY2(2000000)
			WGfx(GR2F, 0x00); /* from Klaus' NetBSD driver: */
			WGfx(GR33, 0x00); /* put blitter into 542x compat */
			WGfx(GR31, 0x00); /* mode */
			break;

		default:
			printk(KERN_ERR "clgen: Warning: Unknown board type\n");
			break;
	}

    /* the P4 is not fully initialized here; I rely on it having been inited under      */
    /* AmigaOS already, which seems to work just fine (Klaus advised to do it this way) */

    if (clboards[slotnum].boardtype != BT_PICASSO4)
    {
	WGen(VSSM, 0x10);  /* EGS: 0x16 */
	WGen(POS102, 0x01);
	WGen(VSSM, 0x08);  /* EGS: 0x0e */

	if(clboards[slotnum].boardtype != BT_SD64)
		WGen(VSSM2, 0x01);

	WSeq(SR0, 0x03);  /* reset sequencer logic */

	WSeq(SR1, 0x21);  /* FullBandwidth (video off) and 8/9 dot clock */
	WGen(MISC_W, 0xc1);  /* polarity (-/-), disable access to display memory, CRTC base address: color */

/*	WGfx(GRA, 0xce);    "magic cookie" - doesn't make any sense to me.. */
	WSeq(SR6, 0x12);   /* unlock all extension registers */

	WGfx(GR31, 0x04);  /* reset blitter */

	switch(clboards[slotnum].boardtype)
	{
		case BT_SD64:
			WSeq(SRF, 0xb8);  /* 4 MB Ram SD64, disable CRT fifo(!), 64 bit bus */
			break;

		case BT_PICCOLO:
			WSeq(SR16, 0x0f); /* Perf. Tuning: Fix value..(?) */
			WSeq(SRF, 0xb0); /* 2 MB DRAM, 8level write buffer, 32bit bus */
			break;

		case BT_PICASSO:
			WSeq(SR16, 0x0f); /* Perf. Tuning: Fix value..(?) */
			WSeq(SRF, 0xb0); /* 2 MB DRAM, 8level write buffer, 32bit bus */
			break;

		case BT_SPECTRUM:
			WSeq(SR16, 0x0f); /* Perf. Tuning: Fix value..(?) */
			WSeq(SRF, 0xb0); /* 2 MB DRAM, 8level write buffer, 32bit bus */
			break;

		default:
			printk(KERN_ERR "clgen: unknown boardtype\n");
			break;
	}
    }

	WSeq(SR2, 0xff);  /* plane mask: nothing */
	WSeq(SR3, 0x00);  /* character map select: doesn't even matter in gx mode */
	WSeq(SR4, 0x0e);  /* memory mode: chain-4, no odd/even, ext. memory */

	/* controller-internal base address of video memory */
	switch(clboards[slotnum].boardtype)
	{
		case BT_SD64:
			WSeq(SR7, 0xf0);
			break;

		case BT_PICCOLO:
			WSeq(SR7, 0x80);
			break;

		case BT_PICASSO:
			WSeq(SR7, 0x20);
			break;

		case BT_SPECTRUM:
			WSeq(SR7, 0x80);
			break;

		case BT_PICASSO4:
			WSeq(SR7, 0x20);
			break;

		default:
			printk(KERN_ERR "clgen: unknown board type\n");
			break;
	}

/*	WSeq(SR8, 0x00);*/  /* EEPROM control: shouldn't be necessary to write to this at all.. */

	WSeq(SR10, 0x00); /* graphics cursor X position (incomplete; position gives rem. 3 bits */
	WSeq(SR11, 0x00); /* graphics cursor Y position (..."... ) */
	WSeq(SR12, 0x00); /* graphics cursor attributes */
	WSeq(SR13, 0x00); /* graphics cursor pattern address */

	/* writing these on a P4 might give problems..  */
	if (clboards[slotnum].boardtype != BT_PICASSO4)
	{
		WSeq(SR17, 0x00); /* configuration readback and ext. color */
		WSeq(SR18, 0x02); /* signature generator */
	}

	/* MCLK select etc. */
	switch(clboards[slotnum].boardtype)
	{
		case BT_SD64:
			WSeq(SR1F, 0x20);
			break;

		case BT_PICCOLO:
			WSeq(SR1F, 0x22);
			break;

		case BT_PICASSO:
			WSeq(SR1F, 0x22);
			break;

		case BT_SPECTRUM:
			WSeq(SR1F, 0x22);
			break;

		case BT_PICASSO4:
/*			WSeq(SR1F, 0x1c); */
			break;

		default:
			printk(KERN_ERR "clgen: unknown board type!\n");
			break;
	}

	WCrt(CRT8, 0x00);  /* Screen A preset row scan: none */
	WCrt(CRTA, 0x20);  /* Text cursor start: disable text cursor */
	WCrt(CRTB, 0x00);  /* Text cursor end: - */
	WCrt(CRTC, 0x00);  /* Screen start address high: 0 */
	WCrt(CRTD, 0x00);  /* Screen start address low: 0 */
	WCrt(CRTE, 0x00);  /* text cursor location high: 0 */
	WCrt(CRTF, 0x00);  /* text cursor location low: 0 */

	WCrt(CRT14, 0x00); /* Underline Row scanline: - */
	WCrt(CRT17, 0xc3); /* mode control: timing enable, byte mode, no compat modes */
	WCrt(CRT18, 0x00); /* Line Compare: not needed */
	/* ### add 0x40 for text modes with > 30 MHz pixclock */
	WCrt(CRT1B, 0x02); /* ext. display controls: ext.adr. wrap */

	WGfx(GR0, 0x00); /* Set/Reset registes: - */
	WGfx(GR1, 0x00); /* Set/Reset enable: - */
	WGfx(GR2, 0x00); /* Color Compare: - */
	WGfx(GR3, 0x00); /* Data Rotate: - */
	WGfx(GR4, 0x00); /* Read Map Select: - */
	WGfx(GR5, 0x00); /* Mode: conf. for 16/4/2 color mode, no odd/even, read/write mode 0 */
	WGfx(GR6, 0x01); /* Miscellaneous: memory map base address, graphics mode */
	WGfx(GR7, 0x0f); /* Color Don't care: involve all planes */
	WGfx(GR8, 0xff); /* Bit Mask: no mask at all */
	WGfx(GRB, 0x28); /* Graphics controller mode extensions: finer granularity, 8byte data latches */

	WGfx(GRC, 0xff); /* Color Key compare: - */
	WGfx(GRD, 0x00); /* Color Key compare mask: - */
	WGfx(GRE, 0x00); /* Miscellaneous control: - */
/*	WGfx(GR10, 0x00);*/ /* Background color byte 1: - */
/*	WGfx(GR11, 0x00); */

	WAttr(AR0, 0x00); /* Attribute Controller palette registers: "identity mapping" */
	WAttr(AR1, 0x01);
	WAttr(AR2, 0x02);
	WAttr(AR3, 0x03);
	WAttr(AR4, 0x04);
	WAttr(AR5, 0x05);
	WAttr(AR6, 0x06);
	WAttr(AR7, 0x07);
	WAttr(AR8, 0x08);
	WAttr(AR9, 0x09);
	WAttr(ARA, 0x0a);
	WAttr(ARB, 0x0b);
	WAttr(ARC, 0x0c);
	WAttr(ARD, 0x0d);
	WAttr(ARE, 0x0e);
	WAttr(ARF, 0x0f);

	WAttr(AR10, 0x01); /* Attribute Controller mode: graphics mode */
	WAttr(AR11, 0x00); /* Overscan color reg.: reg. 0 */
	WAttr(AR12, 0x0f); /* Color Plane enable: Enable all 4 planes */
/* ### 	WAttr(AR33, 0x00); * Pixel Panning: - */
	WAttr(AR14, 0x00); /* Color Select: - */

	WGen(M_3C6, 0xff); /* Pixel mask: no mask */

	WGen(MISC_W, 0xc3); /* polarity (-/-), enable display mem, CRTC i/o base = color */

	WGfx(GR31, 0x04); /* BLT Start/status: Blitter reset */
	WGfx(GR31, 0x00); /* - " -           : "end-of-reset" */

	/* CLUT setup */
/* first, a grey ramp */
#if 1
	{
		int i;

		for (i = 0; i < 256; i++)
			WClut(i, i, i, i);
    }
#endif


#if 1
	WClut( 0, 0x00, 0x00, 0x00);  /* background: black */
	WClut( 1, 0xff, 0xff, 0xff);  /* foreground: white */
	WClut( 2, 0x00, 0x80, 0x00);
	WClut( 3, 0x00, 0x80, 0x80);
	WClut( 4, 0x80, 0x00, 0x00);
	WClut( 5, 0x80, 0x00, 0x80);
	WClut( 6, 0x80, 0x40, 0x00);
	WClut( 7, 0x80, 0x80, 0x80);
	WClut( 8, 0x40, 0x40, 0x40);
	WClut( 9, 0x40, 0x40, 0xc0);
	WClut(10, 0x40, 0xc0, 0x40);
	WClut(11, 0x40, 0xc0, 0xc0);
	WClut(12, 0xc0, 0x40, 0x40);
	WClut(13, 0xc0, 0x40, 0xc0);
	WClut(14, 0xc0, 0xc0, 0x40);
	WClut(15, 0xc0, 0xc0, 0xc0);
#endif


	/* misc... */
	WHDR(0); /* Hidden DAC register: - */

	/* if p4flag is set, board RAM size has already been determined, so skip test */
	if (!clboards[slotnum].p4flag)
	{
		/* "pre-set" a RAMsize; if the test succeeds, double it */
		if (clboards[slotnum].boardtype == BT_SD64 ||
			clboards[slotnum].boardtype == BT_PICASSO4)
			clboards[slotnum].size = 0x400000;
		else
			clboards[slotnum].size = 0x200000;

		/* assume it's a "large memory" board (2/4 MB) */
		clboards[slotnum].smallboard = FALSE;

		/* check for 1/2 MB Piccolo/Picasso/Spectrum resp. 2/4 MB SD64 */
		/* DRAM register has already been pre-set for "large", so it is*/
		/* only modified if we find that this is a "small" version */
		{
			unsigned volatile char *ram = clboards[slotnum].VirtRAMAddr;
			int i, flag = 0;

			ram += (clboards[slotnum].size >> 1);

			for (i = 0; i < 256; i++)
				*(ram+i) = (unsigned char)i;

			for (i = 0; i < 256; i++)
			{
				if (*(ram + i) != i)
					flag = 1;
			}

			/* if the DRAM test failed, halve RAM value */
			if (flag)
			{
				clboards[slotnum].size /= 2;
				clboards[slotnum].smallboard = TRUE;
				switch(clboards[slotnum].boardtype)
				{
					case BT_SD64:
						WSeq(SRF, 0x38);  /* 2 MB Ram SD64 */
						break;

					case BT_PICCOLO:
					case BT_PICASSO:
					case BT_SPECTRUM:
						WSeq(SRF, 0x30); /* 1 MB DRAM */
						break;

					case BT_PICASSO4:
						WSeq(SRF, 0x38);   /* ### like SD64? */
						break;
					default:
						printk(KERN_WARNING "clgen: Uuhh..?\n");
				}
			}
		}
	}
	else
	{
		/* In case of P4 in Z2 mode, set reg here */
		if (clboards[slotnum].smallboard)
			WSeq(SRF, 0x38);
	}

	printk(KERN_INFO "clgen: This board has %ld bytes of DRAM memory\n", clboards[slotnum].size);
}


/**********************************************************************/
/* about the following functions - I have used the same names for the */
/* functions as Markus Wild did in his Retina driver for NetBSD as    */
/* they just made sense for this purpose. Apart from that, I wrote    */
/* these functions myself.                                            */
/**********************************************************************/

/*** WGen() - write into one of the external/general registers ***/
void WGen(int regnum, unsigned char val)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + regnum;

	if(clboards[g_slotnum].boardtype == BT_PICASSO)
	{
		/* Picasso II specific hack */
/*		if (regnum == M_3C7_W || regnum == M_3C9 || regnum == VSSM2) */
		if (regnum == M_3C7_W || regnum == M_3C9)
			reg += 0xfff;
	}

	*reg = val;
}

/*** RGen() - read out one of the external/general registers ***/
unsigned char RGen(int regnum)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + regnum;

	if(clboards[g_slotnum].boardtype == BT_PICASSO)
	{
		/* Picasso II specific hack */
/*		if (regnum == M_3C7_W || regnum == M_3C9 || regnum == VSSM2) */
		if (regnum == M_3C7_W || regnum == M_3C9)
			reg += 0xfff;
	}

	return *reg;
}

/*** WSeq() - write into a register of the sequencer ***/
void WSeq(unsigned char regnum, unsigned char val)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + SRX;

	*reg     = regnum;
	*(reg+1) = val;
}

/*** RSeq() - read out one of the Sequencer registers ***/
unsigned char RSeq(unsigned char regnum)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + SRX;

	*reg = regnum;
	return *(reg+1);
}

/*** WCrt() - write into a register of the CRT controller ***/
void WCrt(unsigned char regnum, unsigned char val)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + CRTX;

	*reg = regnum;
	*(reg+1) = val;
}

/*** RCrt() - read out one of the CRT controller registers ***/
unsigned char RCrt(unsigned char regnum)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + CRTX;

	*reg = regnum;
	return *(reg+1);
}

/*** WGfx() - write into a register of the Gfx controller ***/
void WGfx(unsigned char regnum, unsigned char val)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + GRX;

	*reg = regnum;
	*(reg+1) = val;
}

/*** RGfx() - read out one of the Gfx controller registers ***/
unsigned char RGfx(unsigned char regnum)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + GRX;

	*reg = regnum;
	return *(reg+1);
}

/*** WAttr() - write into a register of the Attribute controller ***/
void WAttr(unsigned char regnum, unsigned char val)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + ARX;

	/* if the next access to the attribute controller is a data write access, */
	/* simply write back the information that was already there before, so that */
	/* the next write access after that will be an index write. */
	if (RCrt(CRT24) & 0x80)
	{
		/* can't use WAttr() here - we would go into a recursive loop otherwise */
		unsigned volatile char *reg2 = clboards[g_slotnum].VirtRegBase + ARX;
		*reg2 = *(reg2+1);
	}

	if (!(RCrt(CRT24) & 0x80))
	{
/*		printk("attr ok "); */
	}
	else
		printk(KERN_WARNING "clgen: *** AttrIdx BAD!***\n");

	/* now, first set index and after that the value - both to the same address (!) */
	*reg = regnum;
	*reg = val;
}

/*** AttrOn() - turn on VideoEnable for Attribute controller ***/
void AttrOn()
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + ARX;

	if (RCrt(CRT24) & 0x80)
	{
		/* if we're just in "write value" mode, write back the */
		/* same value as before to not modify anything */
		unsigned volatile char *reg2 = clboards[g_slotnum].VirtRegBase + ARX;
		*reg2 = *(reg2+1);
	}

	/* turn on video bit */
/*	*reg = 0x20; */
	*reg = 0x33;

	/* dummy write on Reg0 to be on "write index" mode next time */
	*reg = 0x00;
}

/*** RAttr() - read out a register of the Attribute controller ***/
unsigned char RAttr(unsigned char regnum)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + ARX;

	/* (explanation see above in WAttr() ) */

	if (RCrt(CRT24) & 0x80)
	{
		unsigned volatile char *reg2 = clboards[g_slotnum].VirtRegBase + ARX;
		*reg2 = *(reg2+1);
	}

	*reg = regnum;
	return *(reg+1);
}


/*** WHDR() - write into the Hidden DAC register ***/
/* as the HDR is the only extension register that requires special treatment 
 * (the other extension registers are accessible just like the "ordinary"
 * registers of their functional group) here is a specialized routine for 
 * accessing the HDR
 */
void WHDR(unsigned char val)
{
	unsigned char dummy;

	if(clboards[g_slotnum].boardtype == BT_PICASSO)
	{
		/* Klaus' hint for correct access to HDR on some boards */
		/* first write 0 to pixel mask (3c6) */
		WGen(M_3C6, 0x00);
		DELAY
		/* next read dummy from pixel address (3c8) */
		dummy = RGen(M_3C8);
		DELAY
	}

	/* now do the usual stuff to access the HDR */

	dummy = RGen(M_3C6);
	DELAY
	dummy = RGen(M_3C6);
	DELAY
	dummy = RGen(M_3C6);
	DELAY
	dummy = RGen(M_3C6);
	DELAY

	WGen(M_3C6, val);
	DELAY

	if(clboards[g_slotnum].boardtype == BT_PICASSO)
	{
		/* now first reset HDR access counter */
		dummy = RGen(M_3C8);
		DELAY

		/* and at the end, restore the mask value */
		/* ## is this mask always 0xff? */
		WGen(M_3C6, 0xff);
		DELAY
	}
}

/*** RHDR() - read out the Hidden DAC register ***/
/* I hope this does not break on the GD5428 - cannot test it. */
/* (Is there any board for the Amiga that uses the 5428 ?) */
unsigned char RHDR()
{
	unsigned char dummy;

	dummy = RGen(M_3C6);
	dummy = RGen(M_3C6);
	dummy = RGen(M_3C6);
	dummy = RGen(M_3C6);

	return RGen(M_3C6);
}


/*** WSFR() - write to the "special function register" (SFR) ***/

void WSFR(unsigned char val)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + 0x8000;
	*reg = val;
}

/* The Picasso has a second register for switching the monitor bit */
void WSFR2(unsigned char val)
{
	/* writing an arbitrary value to this one causes the monitor switcher */
	/* to flip to Amiga display */

	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + 0x9000;
	*reg = val;
}

/*** WClut - set CLUT entry (range: 0..255 is automat. shifted to 0..63) ***/
void WClut(unsigned char regnum, unsigned char red, unsigned char green, unsigned char blue)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + 0x3c8;

	if(clboards[g_slotnum].boardtype == BT_PICASSO ||
		clboards[g_slotnum].boardtype == BT_PICASSO4)
	{
		/* address write mode register is not translated.. */
		*reg = regnum;
		/* but DAC data register IS, at least for Picasso II */
		if(clboards[g_slotnum].boardtype == BT_PICASSO)
			reg += 0xfff;
		*(reg+1) = (red   >> 2);
		*(reg+1) = (green >> 2);
		*(reg+1) = (blue  >> 2);
	}
	else
	{
		*reg = regnum;
		*(reg+1) = (blue  >> 2);
		*(reg+1) = (green >> 2);
		*(reg+1) = (red   >> 2);
	}
}

/*** RClut - read CLUT entry and convert to 0..255 range ***/
void RClut(unsigned char regnum, unsigned char *red, unsigned char *green, unsigned char *blue)
{
	unsigned volatile char *reg = clboards[g_slotnum].VirtRegBase + 0x3c7;

	if(clboards[g_slotnum].boardtype == BT_PICASSO ||
		clboards[g_slotnum].boardtype == BT_PICASSO4)
	{
		*reg = regnum;
		if(clboards[g_slotnum].boardtype == BT_PICASSO)
			reg += 0xfff;
		*red   = *(reg+2) << 2;
		*green = *(reg+2) << 2;
		*blue  = *(reg+2) << 2;
	}
	else
	{
		*reg = regnum;
		*blue  = *(reg+2) << 2;
		*green = *(reg+2) << 2;
		*red   = *(reg+2) << 2;
	}
}

/*** SelectMap() - choose one of the 4 memory planes for reading (and its mask for writing) ***/
void SelectMap(unsigned char mapnum)
{
	unsigned char mask = 1 << mapnum;

	WGfx(GR4, mapnum);
	WSeq(SR2, mask);
}

/**************************************************************************
 * bestclock() - determine closest possible clock lower(?) than the
 * desired pixel clock
 **************************************************************************/

void bestclock(long freq, long *best, long *nom, long *den, long *div, long maxfreq)
{
	long n, h, d, f;

	*nom = 0;
	*den = 0;
	*div = 0;

	if (freq < 8000)
		freq = 8000;

	if (freq > maxfreq)
		freq = maxfreq;

	*best = 0;
	f = freq * 10;

	for(n = 32; n < 128; n++)
	{
        d = (143181 * n) / f;
		if ( (d >= 7) && (d <= 63) )
        {
            if (d > 31)
            	d = (d / 2) * 2;
            h = (14318 * n) / d;
            if ( abs(h - freq) < abs(*best - freq) )
            {
                *best = h;
				*nom = n;
				if (d < 32)
				{
                    *den = d;
                    *div = 0;
				}
				else
				{
                    *den = d / 2;
					*div = 1;
				}
            }
        }
        d = ( (143181 * n)+f-1) / f;
        if ( (d >= 7) && (d <= 63) )
		{
        	if (d > 31)
        		d = (d / 2) * 2;
			h = (14318 * n) / d;
			if ( abs(h - freq) < abs(*best - freq) )
			{
    	        *best = h;
        	    *nom = n;
            	if (d < 32)
	            {
    	            *den = d;
        	        *div = 0;

	            }
    	        else
        	    {
    	            *den = d / 2;
	                *div = 1;
	            }
			}
		}
	}
}


/*********************************/
/* Framebuffer support functions */
/*********************************/

/**************************************************************
	clgen_fb_get_fix()

	returns structure containing unchangeable values
***************************************************************/

static int clgen_fb_get_fix(struct fb_fix_screeninfo *fix, int con, int fbidx)
{
	int i, slotnum;

	if (con == -1)
	{
		printk(KERN_WARNING "CLGEN: What? con == -1? (1)\n");
		/* ### Fix this..? */
		con = 0;
	}

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		printk(KERN_WARNING "CLGEN: Warning: Board not found in get_fix!!\n");
		return 0;
	}

	/* ID: maximum of 16 characters */
	strcpy(fix->id, "CLGen:");
	switch(clboards[slotnum].boardtype)
	{
		case BT_SD64:
			strcat(fix->id, "SD64");
			break;
		case BT_PICCOLO:
			strcat(fix->id, "Piccolo");
			break;
		case BT_SPECTRUM:
			strcat(fix->id, "Spectrum");
			break;
		case BT_PICASSO:
			strcat(fix->id, "Picasso");
			break;
		case BT_PICASSO4:
			strcat(fix->id, "Picasso IV");
			break;
	}

	fix->smem_start = clboards[slotnum].VirtRAMAddr;

	if (disp[con].var.bits_per_pixel == 1)
	{
		/* monochrome: only 1 memory plane */
		fix->smem_len = clboards[slotnum].size / 4;
	}
	else
	{
		/* 8 bit and above: Use whole memory area */
		fix->smem_len = clboards[slotnum].size;
	}

	fix->type = disp[con].type;
	fix->type_aux = 0;
	fix->visual = disp[con].visual;

	fix->xpanstep = 1;
	fix->ypanstep = 1;
	fix->ywrapstep = 0;
	/* Is this ever used (in the X server?) ? */
	fix->line_length = disp[con].line_length;
	fix->accel = FB_ACCEL_CLGEN;

	for (i = 0; i < arraysize(fix->reserved); i++)
		fix->reserved[i] = 0;

	return(0);
}

/**************************************************************
	clgen_fb_get_var()

	returns changeable information for the "current" mode
***************************************************************/

static int clgen_fb_get_var(struct fb_var_screeninfo *var, int con, int fbidx)
{
	int slotnum;

	if (con == -1)
	{
		printk(KERN_WARNING "CLGEN: What? con == -1? (2)\n");
		/* ### Fix this..? */
		con = 0;
	}

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		printk(KERN_WARNING "CLGEN: Warning: Board not found in get_var)!!\n");
		return 0;
	}

	g_slotnum = slotnum;

	/* this is really all there is to it. */
	*var = disp[con].var;

	return(0);
}

static void clgen_fb_set_disp(int con, int fbidx)
{
	struct fb_fix_screeninfo fix;

	clgen_fb_get_fix(&fix, con, fbidx);
	if (con == -1)
		con = 0;
	disp[con].screen_base = (u_char *)fix.smem_start;
	disp[con].type_aux = fix.type_aux;
	disp[con].ypanstep = fix.ypanstep;
	disp[con].ywrapstep = fix.ywrapstep;
	disp[con].can_soft_blank = 1;
	/* ### add inverse support later? */
	disp[con].inverse = 0;

}

/*************************************************************************
	clgen_fb_set_var()

	activates a new mode, using the supplied fb_var_screeninfo struct
**************************************************************************/

static int clgen_fb_set_var(struct fb_var_screeninfo *var, int con, int fbidx)
{
	int xres, hfront, hsync, hback, yres, vfront, vsync, vback;
	long freq, best, nom, den, div;
	long hfreq, vfreq, hf2, vf2; 
	float freq_f;
	int slotnum, err;
	int oldxres, oldyres, oldvxres, oldvyres, oldbpp;
	int activate = var->activate;
	static int firsttime = 1;

	/* if it's not the current console we change, quit here. */
	if (con != currcon)
		return 1;

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		printk(KERN_WARNING "CLGEN: Warning: Board not found in set_var!!\n");
		return 0;
	}

	g_slotnum = slotnum;

	DEBUG printk("clgen_fb_set_var: entry\n");

	/* convert from ps to kHz */
	freq_f = (1.0/(float)var->pixclock) * 1000000000;
	freq = (long)freq_f;

	DEBUG printk("desired pixclock: %ld kHz\n", freq);	

	/* the SD64/P4 have a higher max. videoclock */
	if (clboards[slotnum].boardtype == BT_SD64 ||
		clboards[slotnum].boardtype == BT_PICASSO4)
		bestclock(freq, &best, &nom, &den, &div, 140000);
	else
		bestclock(freq, &best, &nom, &den, &div, 90000);

	freq = best;

	DEBUG printk("Best possible values for given frequency: best: %ld kHz  nom: %ld  den: %ld  div: %ld\n", best, nom, den, div);

	xres   = var->xres;
	hfront = var->right_margin;
	hsync  = var->hsync_len;
	hback  = var->left_margin;

	if (var->vmode & FB_VMODE_DOUBLE)
	{
		yres = var->yres * 2;
		vfront = var->lower_margin * 2;
		vsync  = var->vsync_len * 2;
		vback  = var->upper_margin * 2;
	}
	else if (var->vmode & FB_VMODE_INTERLACED)
	{
		yres   = (var->yres + 1) / 2;
		vfront = (var->lower_margin + 1) / 2;
		vsync  = (var->vsync_len + 1) / 2;
		vback  = (var->upper_margin + 1) / 2;
	}
	else
	{
		yres   = var->yres;
		vfront = var->lower_margin;
		vsync  = var->vsync_len;
		vback  = var->upper_margin;
	}

	hfreq = (best * 1000) / (xres + hfront + hsync + hback);
	vfreq = hfreq / (yres + vfront + vsync  + vback);

	DEBUG printk("Min/max values: hfmin: %ld  hfmax: %ld  vfmin: %ld  vfmax: %ld\n",
		clboards[slotnum].mon_hfmin, clboards[slotnum].mon_hfmax, 
		clboards[slotnum].mon_vfmin, clboards[slotnum].mon_vfmax);
	DEBUG printk("HFreq: %ld Hz   VFreq: %ld Hz\n", hfreq, vfreq);

	/* in DoubleScan mode, frequencies are actually 1/2 of the */
	/* calculated values */
	if (var->vmode & FB_VMODE_DOUBLE)
	{
		hf2 = hfreq / 2;
		vf2 = vfreq / 2;
	}
	else
	{
		hf2 = hfreq;
		vf2 = vfreq;
	}
	if (hf2 < clboards[slotnum].mon_hfmin || 
	    hf2 > clboards[slotnum].mon_hfmax ||
	    vf2 < clboards[slotnum].mon_vfmin || 
	    vf2 > clboards[slotnum].mon_vfmax)
	{
		printk(KERN_WARNING "clgen: desired mode exceeds monitor limits (rejected)!\n");
		/* reject setting this mode */
		return -EINVAL;
	}
	else
		DEBUG printk(KERN_WARNING "(within monitor limits)\n");

	if (var->bits_per_pixel != 1 && var->bits_per_pixel != 8) 
	{
		printk(KERN_WARNING "clgen: driver only supports 1 and 8 bits depth at the moment\n");
		return -EINVAL;
	}

    /* fill out the rest of the fb_var_screeninfo (this was the reason */
    /* for this driver coming too late for Jes' christmas 2.0.33 rel)  */
    var->activate = 0;
    var->red.offset = 0;
    var->red.length = 8;
    var->red.msb_right = 0;

    var->green.offset = 0;
    var->green.length = 8;
    var->green.msb_right = 0;

    var->blue.offset = 0;
    var->blue.length = 8;
    var->blue.msb_right = 0;

    /* right now, hardware acceleration is only enabled for 8bpp */
    if (var->bits_per_pixel == 8)
	var->accel = FB_ACCEL_CLGEN;

    if ((activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_NOW )
    {
	/* now fill up the disp[] structure according to clboards[..] */
	/* There are many duplicate entries in those two, but for now */
	/* I'll carry around both of them. */
	oldxres = disp[con].var.xres;
	oldyres = disp[con].var.yres;
	oldvxres = disp[con].var.xres_virtual;
	oldvyres = disp[con].var.yres_virtual;
	oldbpp = disp[con].var.bits_per_pixel;
	disp[con].var = *var;

	if (oldxres != var->xres || oldyres != var->yres ||
	  oldvxres != var->xres_virtual || oldvyres != var->yres_virtual ||
	  oldbpp != var->bits_per_pixel || firsttime)
	{
		if (var->bits_per_pixel == 1)
		{
			clboards[slotnum].line_length = var->xres_virtual / 8;
			clboards[slotnum].bpp = 1;
			clboards[slotnum].visual = FB_VISUAL_MONO10;
			clboards[slotnum].type = FB_TYPE_PACKED_PIXELS;
			disp[con].var.xoffset = 0;
			disp[con].var.yoffset = 0;
			disp[con].line_length = clboards[slotnum].line_length;
			disp[con].var.bits_per_pixel = clboards[slotnum].bpp;
			disp[con].visual =  clboards[slotnum].visual;
			disp[con].type =  clboards[slotnum].type;
		}
		else if (var->bits_per_pixel == 8)
		{
			clboards[slotnum].line_length = var->xres_virtual;
			clboards[slotnum].bpp = 8;
			clboards[slotnum].visual = FB_VISUAL_PSEUDOCOLOR;
			clboards[slotnum].type = FB_TYPE_PACKED_PIXELS;
			disp[con].var.xoffset = 0; /* ## should be set synchronous to clboards[..] */
			disp[con].var.yoffset = 0;
			disp[con].line_length = clboards[slotnum].line_length;
			disp[con].var.bits_per_pixel = clboards[slotnum].bpp;
			disp[con].visual =  clboards[slotnum].visual;
			disp[con].type =  clboards[slotnum].type;
		}

		clgen_fb_set_disp(con, fbidx);
		if (!firsttime && fb_info.changevar)
		{
			(*fb_info.changevar)(con);
		}

	}
	else
	{
		/* set mode in hardware; with NOCLEAR! */
		if (con == currcon)
			clgen_set_var(&disp[con].var, 0);
	}

	if (oldbpp != var->bits_per_pixel)
	{
 		if ((err = alloc_cmap(&disp[con].cmap, 0, 0)))
			return err;
		do_install_cmap(con);
	}

	/* ### still necessary? */
	firsttime = 0;
    }

    DEBUG printk(KERN_INFO "clgen_fb_set_var: exit\n");

    return(0);
}


/*************************************************************************
	clgen_set_var()

	actually writes the values for a new video mode into the hardware,
	uses the information supplied through a fb_var_screeninfo structure.

	It should be safe to call this function repeatedly (what happens
	when you do e.g. "fbset -depth 1")
**************************************************************************/

static void clgen_set_var(struct fb_var_screeninfo *var, int clearflag )
{
	unsigned char tmp;
	int slotnum = g_slotnum;
	int xres, hfront, hsync, hback, yres, vfront, vsync, vback;
	long freq, best, nom, den, div;
	float freq_f;
	int offset = 0;
	CLGenData data;

	/* switching to a (new) mode always means resetting panning values */
	clboards[slotnum].xoffset = 0;
	clboards[slotnum].yoffset = 0;

	/* convert from ps to kHz */
	freq_f = (1.0/(float)var->pixclock) * 1000000000;
	freq = (long)freq_f;

	DEBUG printk("desired pixclock: %ld kHz\n", freq);	

	/* the SD64/P4 have a higher max. videoclock */
	if (clboards[slotnum].boardtype == BT_SD64 ||
		clboards[slotnum].boardtype == BT_PICASSO4)
		bestclock(freq, &best, &nom, &den, &div, 140000);
	else
		bestclock(freq, &best, &nom, &den, &div, 90000);

	freq = best;

	DEBUG printk("Best possible values for given frequency: best: %ld kHz  nom: %ld  den: %ld  div: %ld\n", best, nom, den, div);

	xres   = var->xres;
	hfront = var->right_margin;
	hsync  = var->hsync_len;
	hback  = var->left_margin;

	if (var->vmode & FB_VMODE_DOUBLE)
	{
		yres = var->yres * 2;
		vfront = var->lower_margin * 2;
		vsync  = var->vsync_len * 2;
		vback  = var->upper_margin * 2;
	}
	else if (var->vmode & FB_VMODE_INTERLACED)
	{
		yres   = (var->yres + 1) / 2;
		vfront = (var->lower_margin + 1) / 2;
		vsync  = (var->vsync_len + 1) / 2;
		vback  = (var->upper_margin + 1) / 2;
	}
	else
	{
		yres   = var->yres;
		vfront = var->lower_margin;
		vsync  = var->vsync_len;
		vback  = var->upper_margin;
	}


	/* this is our "current" mode storage place */
/*	memcpy(&clboards[slotnum].currentinfo, var, sizeof(struct fb_var_screeninfo)); */

	data.HorizTotal      = (xres / 8) + (hfront / 8) + (hsync / 8) + (hback / 8) - 5;
	data.HorizDispEnd    = (xres / 8) - 1;
	data.HorizBlankStart = xres / 8;
/* alt: 	data.HorizBlankStart = data.HorizDispEnd; */
	data.HorizBlankEnd   = data.HorizTotal+5; /* does not count with "-5" */
	data.HorizSyncStart  = (xres / 8) + (hfront/8) + 1;
	data.HorizSyncEnd    = (xres / 8) + (hfront/8) + (hsync/8) + 1;

	data.VerticalTotal      = yres + vfront + vsync + vback -2;
	data.VerticalDispEnd    = yres - 1;
	data.VerticalBlankStart = yres;
/* alt	data.VerticalBlankStart = data.VerticalDispEnd; */
	data.VerticalBlankEnd   = data.VerticalTotal;
	data.VerticalSyncStart  = yres + vfront - 1;
	data.VerticalSyncEnd    = yres + vfront + vsync - 1;

	if (data.VerticalTotal >= 1024)
		printk(KERN_WARNING "clgen: WARNING: VerticalTotal >= 1024; special treatment required! (TODO)\n");

	/* unlock register CRT0..CRT7 */
	WCrt(CRT11, 0x20); /* previously: 0x00) */

	/* if DEBUG is set, all parameters get output before writing */
	DEBUG printk("CRT0: %ld\n", data.HorizTotal); 
	WCrt(CRT0, data.HorizTotal);

	DEBUG printk("CRT1: %ld\n", data.HorizDispEnd); 
	WCrt(CRT1, data.HorizDispEnd);

	DEBUG printk("CRT2: %ld\n", data.HorizBlankStart); 
	WCrt(CRT2, data.HorizBlankStart);

	DEBUG printk("CRT3: 128+%ld\n", data.HorizBlankEnd % 32); /*  + 128: Compatible read */
	WCrt(CRT3, 128 + (data.HorizBlankEnd % 32));

	DEBUG printk("CRT4: %ld\n", data.HorizSyncStart);
	WCrt(CRT4, data.HorizSyncStart);

	if (data.HorizBlankEnd & 32)	
		tmp = 128 + (data.HorizSyncEnd % 32);
	else
		tmp = data.HorizSyncEnd % 32;
	DEBUG printk("CRT5: %d\n", tmp); 
	WCrt(CRT5, tmp);

	DEBUG printk("CRT6: %ld\n", data.VerticalTotal & 0xff); 
	WCrt(CRT6, (data.VerticalTotal & 0xff));

	tmp = 16;  /* LineCompare bit #9 */
	if (data.VerticalTotal & 256)
		tmp |= 1;
	if (data.VerticalDispEnd & 256)
		tmp |= 2;
	if (data.VerticalSyncStart & 256)
		tmp |= 4;
	if (data.VerticalBlankStart & 256)
		tmp |= 8;
	if (data.VerticalTotal & 512)
		tmp |= 32;
	if (data.VerticalDispEnd & 512)
		tmp |= 64;
	if (data.VerticalSyncStart & 512)
		tmp |= 128;
	DEBUG printk("CRT7: %d\n", tmp);
	WCrt(CRT7, tmp);

	tmp = 0x40; /* LineCompare bit #8 */
	if (data.VerticalBlankStart & 512)
		tmp |= 0x20;
	if (var->vmode & FB_VMODE_DOUBLE)
		tmp |= 0x80;
 	DEBUG printk("CRT9: %d\n", tmp);
	WCrt(CRT9, tmp);

	/* screen start address high/low */
	WCrt(CRTC, 0x00);
	WCrt(CRTD, 0x00);

 	DEBUG printk("CRT10: %ld\n", data.VerticalSyncStart & 0xff);
	WCrt(CRT10, (data.VerticalSyncStart & 0xff));

	DEBUG printk("CRT11: 64+32+%ld\n", data.VerticalSyncEnd % 16);
	WCrt(CRT11, (data.VerticalSyncEnd % 16 + 64 + 32));

	DEBUG printk("CRT12: %ld\n", data.VerticalDispEnd & 0xff);
	WCrt(CRT12, (data.VerticalDispEnd & 0xff));

	DEBUG printk("CRT15: %ld\n", data.VerticalBlankStart & 0xff);
	WCrt(CRT15, (data.VerticalBlankStart & 0xff));

	DEBUG printk("CRT16: %ld\n", data.VerticalBlankEnd & 0xff);
	WCrt(CRT16, (data.VerticalBlankEnd & 0xff));

	DEBUG printk("CRT18: 0xff\n");
	WCrt(CRT18, 0xff);

	tmp = 0;
	if (data.HorizBlankEnd & 64)
		tmp |= 16;
	if (data.HorizBlankEnd & 128)
		tmp |= 32;
	if (data.VerticalBlankEnd & 256)
		tmp |= 64;
	if (data.VerticalBlankEnd & 512)
		tmp |= 128;
	if (var->vmode & FB_VMODE_INTERLACED)
		tmp |= 0x01;
 	DEBUG printk("CRT1a: %d\n", tmp);
	WCrt(CRT1A, tmp);

	/* set VCLK0 */
	/* hardware RefClock: 14.31818 MHz */
	/* formula: VClk = (OSC * N) / (D * (1+P)) */
	/* Example: VClk = (14.31818 * 91) / (23 * (1+1)) = 28.325 MHz */

	WSeq(SRB, nom);
	if (div == 0)
		tmp = den << 1;
	else
		tmp = (den << 1) | 0x01;

	if (clboards[slotnum].boardtype == BT_SD64)
		tmp |= 0x80; /* 6 bit denom; ONLY 5434!!! (bugged me 10 days) */
	WSeq(SR1B, tmp);

	WCrt(CRT17, 0xc3); /* mode control: CRTC enable, ROTATE(?), 16bit address wrap, no compat. */

/* HAEH?	WCrt(CRT11, 0x20);  * previously: 0x00  unlock CRT0..CRT7 */

	/* don't know if it would hurt to also program this if no interlaced */
	/* mode is used, but I feel better this way.. :-) */
	if (var->vmode & FB_VMODE_INTERLACED)
		WCrt(CRT19, (data.HorizTotal / 2));
	else
		WCrt(CRT19, 0x00);	/* interlace control */

	WSeq(SR3, 0);

	/* adjust horizontal/vertical sync type (low/high) */
	tmp = 0x03; /* enable display memory & CRTC I/O address for color mode */
	if (var->sync & FB_SYNC_HOR_HIGH_ACT)
		tmp |= 0x40;
	if (var->sync & FB_SYNC_VERT_HIGH_ACT)
		tmp |= 0x80;
	WGen(MISC_W, tmp);

	WCrt(CRT8,    0);      /* Screen A Preset Row-Scan register */
	WCrt(CRTA,    0);      /* text cursor on and start line */
	WCrt(CRTB,   31);      /* text cursor end line */

	/* programming for different color depths */
	if (var->bits_per_pixel == 1)
	{
	DEBUG printk(KERN_INFO "clgen: preparing for 1 bit deep display\n");
#if 0
		/* restore first 2 color registers for mono mode */
		WClut( 0, 0x00, 0x00, 0x00);  /* background: black */
		WClut( 1, 0xff, 0xff, 0xff);  /* foreground: white */
#endif
		
		WGfx(GR5,     0);      /* mode register */
		/* Extended Sequencer Mode */
		switch(clboards[slotnum].boardtype)
		{
			case BT_SD64:
			/* setting the SRF on SD64 is not necessary (only during init) */
	DEBUG printk(KERN_INFO "(for SD64)\n");
				WSeq(SR7,  0xf0);
				WSeq(SR1F, 0x1a);     /*  MCLK select */
				break;

			case BT_PICCOLO:
	DEBUG printk(KERN_INFO "(for Piccolo)\n");
				WSeq(SR7, 0x80);
/* ### ueberall 0x22? */
				WSeq(SR1F, 0x22);     /* ##vorher 1c MCLK select */
				WSeq(SRF, 0xb0);    /* evtl d0 bei 1 bit? avoid FIFO underruns..? */
				break;

			case BT_PICASSO:
	DEBUG printk(KERN_INFO "(for Picasso)\n");
				WSeq(SR7, 0x20);
				WSeq(SR1F, 0x22);     /* ##vorher 22 MCLK select */
				WSeq(SRF, 0xd0);    /* ## vorher d0 avoid FIFO underruns..? */
				break;

			case BT_SPECTRUM:
	DEBUG printk(KERN_INFO "(for Spectrum)\n");
				WSeq(SR7, 0x80);
/* ### ueberall 0x22? */
				WSeq(SR1F, 0x22);     /* ##vorher 1c MCLK select */
				WSeq(SRF, 0xb0);    /* evtl d0? avoid FIFO underruns..? */
				break;

			case BT_PICASSO4:
	DEBUG printk(KERN_INFO "(for Picasso IV)\n");
				WSeq(SR7, 0x20);
/*				WSeq(SR1F, 0x1c); */
/* SRF not being set here...	WSeq(SRF, 0xd0); */
				break;

			default:
				printk(KERN_WARNING "clgen: unknown Board\n");
				break;
		}

		WGen(M_3C6,0x01);     /* pixel mask: pass-through for first plane */
		WHDR(0);              /* hidden dac reg: nothing special */
		WSeq(SR4,  0x06);     /* memory mode: odd/even, ext. memory */
		WSeq(SR2, 0x01);      /* plane mask: only write to first plane */
		offset = var->xres_virtual / (8 * 2);
		clboards[slotnum].line_length = var->xres_virtual / 8;
		clboards[slotnum].bpp = 1;
		clboards[slotnum].visual = FB_VISUAL_MONO10;
		clboards[slotnum].type = FB_TYPE_PACKED_PIXELS;
	}
	else if (var->bits_per_pixel == 8)
	{
	DEBUG printk(KERN_INFO "clgen: preparing for 8 bit deep display\n");
		switch(clboards[slotnum].boardtype)
		{
			case BT_SD64:
				WSeq(SR7,  0xf1); /* Extended Sequencer Mode: 256c col. mode */
				WSeq(SR1F, 0x1d);     /* MCLK select */
				break;

			case BT_PICCOLO:
				WSeq(SR7, 0x81);
				WSeq(SR1F, 0x22);     /* ### vorher 1c MCLK select */
				WSeq(SRF, 0xb0);    /* Fast Page-Mode writes */
				break;

			case BT_PICASSO:
				WSeq(SR7, 0x21);
				WSeq(SR1F, 0x22);     /* ### vorher 1c MCLK select */
				WSeq(SRF, 0xb0);    /* Fast Page-Mode writes */
				break;

			case BT_SPECTRUM:
				WSeq(SR7, 0x81);
				WSeq(SR1F, 0x22);     /* ### vorher 1c MCLK select */
				WSeq(SRF, 0xb0);    /* Fast Page-Mode writes */
				break;

			case BT_PICASSO4:
				WSeq(SR7, 0x21);
				WSeq(SRF, 0xb8); /* ### INCOMPLETE!! */
/*				WSeq(SR1F, 0x1c); */
				break;

			default:
				printk(KERN_WARNING "clgen: unknown Board\n");
				break;
		}

		WGfx(GR5,    64);     /* mode register: 256 color mode */
		WGen(M_3C6,0xff);     /* pixel mask: pass-through all planes */
		WHDR(0);              /* hidden dac reg: nothing special */
		WSeq(SR4,  0x0a);     /* memory mode: chain4, ext. memory */
		WSeq(SR2,  0xff);     /* plane mask: enable writing to all 4 planes */
		offset = var->xres_virtual / 8;
		clboards[slotnum].line_length = var->xres_virtual;
		clboards[slotnum].bpp = 8;
		clboards[slotnum].visual = FB_VISUAL_PSEUDOCOLOR;
		clboards[slotnum].type = FB_TYPE_PACKED_PIXELS;
	}
	else if (var->bits_per_pixel == 16)
	{
	DEBUG printk(KERN_INFO "clgen: preparing for 16 bit deep display\n");
		switch(clboards[slotnum].boardtype)
		{
			case BT_SD64:
				WSeq(SR7,  0xf7); /* Extended Sequencer Mode: 256c col. mode */
				WSeq(SR1F, 0x1e);     /* MCLK select */
				break;

			case BT_PICCOLO:
				WSeq(SR7, 0x87);
				WSeq(SRF, 0xb0);    /* Fast Page-Mode writes */
				WSeq(SR1F, 0x22);     /* MCLK select */
				break;

			case BT_PICASSO:
				WSeq(SR7, 0x27);
				WSeq(SRF, 0xb0);    /* Fast Page-Mode writes */
				WSeq(SR1F, 0x22);     /* MCLK select */
				break;

			case BT_SPECTRUM:
				WSeq(SR7, 0x87);
				WSeq(SRF, 0xb0);    /* Fast Page-Mode writes */
				WSeq(SR1F, 0x22);     /* MCLK select */
				break;

			case BT_PICASSO4:
				WSeq(SR7, 0x27);
/*				WSeq(SR1F, 0x1c);  */
				break;

			default:
				printk(KERN_WARNING "CLGEN: unknown Board\n");
				break;
		}

		WGfx(GR5,    64);     /* mode register: 256 color mode */
		WGen(M_3C6,0xff);     /* pixel mask: pass-through all planes */
		WHDR(0xa0);           /* hidden dac reg: nothing special */
		WSeq(SR4,  0x0a);     /* memory mode: chain4, ext. memory */
		WSeq(SR2,  0xff);     /* plane mask: enable writing to all 4 planes */
		offset = (var->xres_virtual / 8) * 2;
		clboards[slotnum].line_length = var->xres_virtual * 2;
		clboards[slotnum].bpp = 16;
		clboards[slotnum].visual = FB_VISUAL_DIRECTCOLOR;
		clboards[slotnum].type = FB_TYPE_PACKED_PIXELS;
	}
	else if (var->bits_per_pixel == 32)
	{
	DEBUG printk(KERN_INFO "clgen: preparing for 24/32 bit deep display\n");
		switch(clboards[slotnum].boardtype)
		{
			case BT_SD64:
				WSeq(SR7,  0xf9); /* Extended Sequencer Mode: 256c col. mode */
				WSeq(SR1F, 0x1e);     /* MCLK select */
				break;

			case BT_PICCOLO:
				WSeq(SR7, 0x85);
				WSeq(SRF, 0xb0);    /* Fast Page-Mode writes */
				WSeq(SR1F, 0x22);     /* MCLK select */
				break;

			case BT_PICASSO:
				WSeq(SR7, 0x25);
				WSeq(SRF, 0xb0);    /* Fast Page-Mode writes */
				WSeq(SR1F, 0x22);     /* MCLK select */
				break;

			case BT_SPECTRUM:
				WSeq(SR7, 0x85);
				WSeq(SRF, 0xb0);    /* Fast Page-Mode writes */
				WSeq(SR1F, 0x22);     /* MCLK select */
				break;

			case BT_PICASSO4:
				WSeq(SR7, 0x25);
/*				WSeq(SR1F, 0x1c);  */
				break;

			default:
				printk(KERN_WARNING "clgen: unknown Board\n");
				break;
		}

		WGfx(GR5,    64);     /* mode register: 256 color mode */
		WGen(M_3C6,0xff);     /* pixel mask: pass-through all planes */
		WHDR(0xc5);           /* hidden dac reg: 8-8-8 mode (24 or 32) */
		WSeq(SR4,  0x0a);     /* memory mode: chain4, ext. memory */
		WSeq(SR2,  0xff);     /* plane mask: enable writing to all 4 planes */
		offset = (var->xres_virtual / 8) * 2;
		clboards[slotnum].line_length = var->xres_virtual * 4;
		clboards[slotnum].bpp = 32;
		clboards[slotnum].visual = FB_VISUAL_DIRECTCOLOR;
		clboards[slotnum].type = FB_TYPE_PACKED_PIXELS;
	}
	else
		printk(KERN_ERR "clgen: Uh-oh. You are not supposed to read this - now I'll have to kill you.\n");

	WCrt(CRT13, offset & 0xff);
	tmp = 0x22;
	if (offset & 0x100)  /* offset overflow bit */
		tmp |= 0x10;
	WCrt(CRT1B,tmp);    /* screen start addr #16-18, fastpagemode cycles */
	if (clboards[slotnum].boardtype == BT_SD64 ||
		clboards[slotnum].boardtype == BT_PICASSO4)
		WCrt(CRT1D, 0x00);   /* screen start address bit 19 */

	WCrt(CRTE,    0);      /* text cursor location high */
	WCrt(CRTF,    0);      /* text cursor location low */
	WCrt(CRT14,   0);      /* underline row scanline = at very bottom */

	WAttr(AR10,  1);      /* controller mode */
	WAttr(AR11,  0);      /* overscan (border) color */
	WAttr(AR12, 15);      /* color plane enable */
	WAttr(AR33,  0);      /* pixel panning */
	WAttr(AR14,  0);      /* color select */

	/* [ EGS: SetOffset(); ] */
	/* From SetOffset(): Turn on VideoEnable bit in Attribute controller */
	AttrOn();

	WGfx(GR0,    0);      /* set/reset register */
	WGfx(GR1,    0);      /* set/reset enable */
	WGfx(GR2,    0);      /* color compare */
	WGfx(GR3,    0);      /* data rotate */
	WGfx(GR4,    0);      /* read map select */
	WGfx(GR6,    1);      /* miscellaneous register */
	WGfx(GR7,   15);      /* color don't care */
	WGfx(GR8,  255);      /* bit mask */

	WSeq(SR12,  0x0);     /* graphics cursor attributes: nothing special */

	/* finally, turn on everything - turn off "FullBandwidth" bit */
	/* also, set "DotClock%2" bit where requested */
	tmp = 0x01;
	if (var->vmode & FB_VMODE_CLOCK_HALVE)
		tmp |= 0x08;
	WSeq(SR1, tmp);
	DEBUG printk("SR1: %d\n", tmp);

	/* turn on board video encode and switch monitor bit.. */
	switch(clboards[slotnum].boardtype)
	{
		case BT_SD64:
			clboards[slotnum].SFR |= 0x21;
			WSFR(clboards[slotnum].SFR);
			break;

		case BT_PICCOLO:
			clboards[slotnum].SFR |= 0x28;
			WSFR(clboards[slotnum].SFR);
			break;

		case BT_PICASSO:
			clboards[slotnum].SFR = 0xff;
			WSFR(clboards[slotnum].SFR);
			break;

		case BT_SPECTRUM:
			clboards[slotnum].SFR = 0x6f;
			WSFR(clboards[slotnum].SFR);
			break;

		case BT_PICASSO4:
			/* no monitor bit to be switched here */
			break;

		default:
			DEBUG printk(KERN_ERR "clgen: board not really known..\n");
			break;

	}

	if (clearflag)
	{
		DEBUG printk(KERN_INFO "clgen: clearing display...");
		clgen_RectFill(0, 0, xres, yres, 0, xres);
		clgen_WaitBLT();
		DEBUG printk("clgen: done.\n");
	}
}


/**************
 New versions of fb_get_cmap / fb_set_cmap, copied from amifb / cyberfb
**************/


   /*
    *    Get the Colormap
    */

static int clgen_fb_get_cmap(struct fb_cmap *cmap, int kspc, int con, int fbidx)
{
	int slotnum;

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		DEBUG printk(KERN_WARNING "clgen: Warning: Board not found in get_cmap)!!\n");
		return 0;
	}

	g_slotnum = slotnum;

	if (con == currcon) /* current console? */
		return(do_fb_get_cmap(cmap, &disp[con].var, kspc));
	else if (disp[con].cmap.len) /* non default colormap? */
		copy_cmap(&disp[con].cmap, cmap, kspc ? 0 : 2);
	else
		copy_cmap(get_default_colormap(1 << disp[con].var.bits_per_pixel), cmap,
		kspc ? 0 : 2);
	return(0);
}

   /*
    *    Set the Colormap
    */

static int clgen_fb_set_cmap(struct fb_cmap *cmap, int kspc, int con, int fbidx)
{
	int err;
	int slotnum;

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		DEBUG printk(KERN_WARNING "clgen: Warning: Board not found in get_cmap)!!\n");
		return 0;
	}

	g_slotnum = slotnum;

	if (!disp[con].cmap.len) /* no colormap allocated? */
	{
		if ((err = alloc_cmap(&disp[con].cmap, 
			1<<disp[con].var.bits_per_pixel, 0)))
		return(err);
	}
	if (con == currcon)              /* current console? */
		return(do_fb_set_cmap(cmap, &disp[con].var, kspc));
	else
		copy_cmap(cmap, &disp[con].cmap, kspc ? 0 : 1);
	return(0);
}



/**********************
  Auxiliary cmap handling functions, again directly copied from cyberfb
**********************/

/* two tiny "wrappers" to avoid having to change prototypes in other places */
static int clgen_getcolreg(u_int regno, u_int *red, u_int *green, u_int *blue,
                         u_int *transp)
{
	unsigned char bred, bgreen, bblue;

	if (regno > 255 || regno < 0)
		return (1);
	RClut(regno, &bred, &bgreen, &bblue);

	*red    = (u_int)bred;
	*green  = (u_int)bgreen;
	*blue   = (u_int)bblue;
	*transp = 0;
	return (0);
}

static int clgen_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                         u_int transp)
{
	if (regno > 255 || regno < 0)
		return (1);

	/* "transparent" stuff is completely ignored. */
	WClut(regno, (red & 0xff), (green & 0xff), (blue & 0xff));

	return (0);
}


static struct fb_cmap *get_default_colormap(int len)
{
	if (len == 2)
		return &default_2_colors;
	return &default_16_colors;
}

#define CNVT_TOHW(val,width)     ((((val)<<(width))+0x7fff-(val))>>16)
#define CNVT_FROMHW(val,width)   (((width) ? ((((val)<<16)-(val)) / \
                                              ((1<<(width))-1)) : 0))

static int do_fb_get_cmap(struct fb_cmap *cmap, struct fb_var_screeninfo *var,
                          int kspc)
{
   int i, start;
   u_short *red, *green, *blue, *transp;
   u_int hred, hgreen, hblue, htransp;

   red = cmap->red;
   green = cmap->green;
   blue = cmap->blue;
   transp = cmap->transp;
   start = cmap->start;
   if (start < 0)
      return(-EINVAL);
   for (i = 0; i < cmap->len; i++) {
      if (fbhw->getcolreg(start++, &hred, &hgreen, &hblue, &htransp))
         return(0);
      hred = CNVT_FROMHW(hred, var->red.length);
      hgreen = CNVT_FROMHW(hgreen, var->green.length);
      hblue = CNVT_FROMHW(hblue, var->blue.length);
      htransp = CNVT_FROMHW(htransp, var->transp.length);
      if (kspc) {
         *red = hred;
         *green = hgreen;
         *blue = hblue;
         if (transp)
            *transp = htransp;
      } else {
         put_fs_word(hred, red);
         put_fs_word(hgreen, green);
         put_fs_word(hblue, blue);
         if (transp)
            put_fs_word(htransp, transp);
      }
      red++;
      green++;
      blue++;
      if (transp)
         transp++;
   }
   return(0);
}


static int do_fb_set_cmap(struct fb_cmap *cmap, struct fb_var_screeninfo *var,
                          int kspc)
{
   int i, start;
   u_short *red, *green, *blue, *transp;
   u_int hred, hgreen, hblue, htransp;

   red = cmap->red;
   green = cmap->green;
   blue = cmap->blue;
   transp = cmap->transp;
   start = cmap->start;

   if (start < 0)
      return(-EINVAL);
   for (i = 0; i < cmap->len; i++) {
      if (kspc) {
         hred = *red;
         hgreen = *green;
         hblue = *blue;
         htransp = transp ? *transp : 0;
      } else {
         hred = get_fs_word(red);
         hgreen = get_fs_word(green);
         hblue = get_fs_word(blue);
         htransp = transp ? get_fs_word(transp) : 0;
      }
      hred = CNVT_TOHW(hred, var->red.length);
      hgreen = CNVT_TOHW(hgreen, var->green.length);
      hblue = CNVT_TOHW(hblue, var->blue.length);
      htransp = CNVT_TOHW(htransp, var->transp.length);
      red++;
      green++;
      blue++;
      if (transp)
         transp++;
      if (fbhw->setcolreg(start++, hred, hgreen, hblue, htransp))
         return(0);
   }
   return(0);
}


static void do_install_cmap(int con)
{
   if (con != currcon)
      return;
   if (disp[con].cmap.len)
     do_fb_set_cmap(&disp[con].cmap, &disp[con].var, 1);
   else
      do_fb_set_cmap(get_default_colormap(1 << disp[con].var.bits_per_pixel),
                                          &disp[con].var, 1);
}

static void memcpy_fs(int fsfromto, void *to, void *from, int len)
{
   switch (fsfromto) {
      case 0:
         memcpy(to, from, len);
         return;
      case 1:
         memcpy_fromfs(to, from, len);
         return;
      case 2:
         memcpy_tofs(to, from, len);
         return;
   }
}


static void copy_cmap(struct fb_cmap *from, struct fb_cmap *to, int fsfromto)
{
   int size;
   int tooff = 0, fromoff = 0;

   if (to->start > from->start)
      fromoff = to->start-from->start;
   else
      tooff = from->start-to->start;
   size = to->len-tooff;
   if (size > from->len-fromoff)
      size = from->len-fromoff;
   if (size < 0)
      return;
   size *= sizeof(u_short);
   memcpy_fs(fsfromto, to->red+tooff, from->red+fromoff, size);
   memcpy_fs(fsfromto, to->green+tooff, from->green+fromoff, size);
   memcpy_fs(fsfromto, to->blue+tooff, from->blue+fromoff, size);
   if (from->transp && to->transp)
      memcpy_fs(fsfromto, to->transp+tooff, from->transp+fromoff, size);
}


static int alloc_cmap(struct fb_cmap *cmap, int len, int transp)
{
   int size = len*sizeof(u_short);

	DEBUG printk("entry alloc_cmap; len = %d, old cmap->len: %d\n", len, cmap->len);
   if (cmap->len != len) {
      if (cmap->red)
         kfree(cmap->red);
      if (cmap->green)
         kfree(cmap->green);
      if (cmap->blue)
         kfree(cmap->blue);
      if (cmap->transp)
         kfree(cmap->transp);
      cmap->red = cmap->green = cmap->blue = cmap->transp = NULL;
      cmap->len = 0;
      if (!len)
         return(0);
      if (!(cmap->red = kmalloc(size, GFP_ATOMIC)))
         return(-1);
      if (!(cmap->green = kmalloc(size, GFP_ATOMIC)))
         return(-1);
      if (!(cmap->blue = kmalloc(size, GFP_ATOMIC)))
         return(-1);
      if (transp) {
         if (!(cmap->transp = kmalloc(size, GFP_ATOMIC)))
            return(-1);
      } else
         cmap->transp = NULL;
   }
   cmap->start = 0;
   cmap->len = len;
   copy_cmap(get_default_colormap(len), cmap, 0);
   return(0);
}


/*******************************************************************
	clgen_WaitBLT()

	Wait for the BitBLT engine to complete a possible earlier job
*********************************************************************/

void clgen_WaitBLT()
{
	/* now busy-wait until we're done */
	while (RGfx(GR31) & 0x08)
		;

	/* These two may have been modified by clgen_RectFill, */
	/* and MUST be reset before anything else is done. */
	WGfx(GR0, 0);
	WGfx(GR1, 0);
}


/*******************************************************************
	clgen_BitBLT()

	perform accelerated "scrolling"
********************************************************************/

/* #############
These accel funcs are not yet ok for multiple boards!
############### */

void clgen_BitBLT (u_short curx, u_short cury, u_short destx, u_short desty,
		u_short width, u_short height, u_short line_length)
{
	u_short nwidth, nheight;
	u_long nsrc, ndest;
	u_char bltmode;

	nwidth = width - 1;
	nheight = height - 1;

	bltmode = 0x00;
	/* if source adr < dest addr, do the Blt backwards */
	if (cury <= desty)
	{
		if (cury == desty)
		{
			/* if src and dest are on the same line, check x */
			if (curx < destx)
				bltmode |= 0x01;
		}
		else
			bltmode |= 0x01;
	}

	if (!bltmode)
	{
		/* standard case: forward blitting */
		nsrc = (cury * line_length) + curx;
		ndest = (desty * line_length) + destx;
	}
	else
	{
		/* this means start addresses are at the end, counting backwards */
		nsrc = cury * line_length + curx + nheight * line_length + nwidth;
		ndest = desty * line_length + destx + nheight * line_length + nwidth;
	}

	clgen_WaitBLT(); /* ### NOT OK for multiple boards! */

	/*
		run-down of registers to be programmed:
		destination pitch
		source pitch
		BLT width/height
		source start
		destination start
		BLT mode
		BLT ROP
		GR0 / GR1: "fill color"
		start/stop
	*/

	/* pitch: set to line_length */
	WGfx(GR24, line_length & 0xff);	/* dest pitch low */
	WGfx(GR25, (line_length >> 8));	/* dest pitch hi */
	WGfx(GR26, line_length & 0xff);	/* source pitch low */
	WGfx(GR27, (line_length >> 8));	/* source pitch hi */

	/* BLT width: actual number of pixels - 1 */
	WGfx(GR20, nwidth & 0xff);	/* BLT width low */
	WGfx(GR21, (nwidth >> 8));	/* BLT width hi */

	/* BLT height: actual number of lines -1 */
	WGfx(GR22, nheight & 0xff);	/* BLT height low */
	WGfx(GR23, (nheight >> 8));	/* BLT width hi */

	/* BLT destination */
	WGfx(GR28, (u_char)(ndest & 0xff));	/* BLT dest low */
	WGfx(GR29, (u_char)(ndest >> 8));	/* BLT dest mid */
	WGfx(GR2A, (u_char)(ndest >> 16));	/* BLT dest hi */

	/* BLT source */
	WGfx(GR2C, (u_char)(nsrc & 0xff));	/* BLT src low */
	WGfx(GR2D, (u_char)(nsrc >> 8));	/* BLT src mid */
	WGfx(GR2E, (u_char)(nsrc >> 16));	/* BLT src hi */

	/* BLT mode */
	WGfx(GR30, bltmode);	/* BLT mode */

	/* BLT ROP: SrcCopy */
	WGfx(GR32, 0x0d);	/* BLT ROP */

	/* and finally: GO! */
	WGfx(GR31, 0x02);	/* BLT Start/status */
}

/*******************************************************************
	clgen_RectFill()

	perform accelerated rectangle fill
********************************************************************/

void clgen_RectFill (u_short x, u_short y, u_short width, u_short height,
                     u_char color, u_short line_length)
{
	u_short nwidth, nheight;
	u_long ndest;

	nwidth = width - 1;
	nheight = height - 1;

	ndest = (y * line_length) + x;

	clgen_WaitBLT(); /* ### NOT OK for multiple boards! */

	/* pitch: set to line_length */
	WGfx(GR24, line_length & 0xff);	/* dest pitch low */
	WGfx(GR25, (line_length >> 8));	/* dest pitch hi */
	WGfx(GR26, line_length & 0xff);	/* source pitch low */
	WGfx(GR27, (line_length >> 8));	/* source pitch hi */

	/* BLT width: actual number of pixels - 1 */
	WGfx(GR20, nwidth & 0xff);	/* BLT width low */
	WGfx(GR21, (nwidth >> 8));	/* BLT width hi */

	/* BLT height: actual number of lines -1 */
	WGfx(GR22, nheight & 0xff);	/* BLT height low */
	WGfx(GR23, (nheight >> 8));	/* BLT width hi */

	/* BLT destination */
	WGfx(GR28, (u_char)(ndest & 0xff));	/* BLT dest low */
	WGfx(GR29, (u_char)(ndest >> 8));	/* BLT dest mid */
	WGfx(GR2A, (u_char)(ndest >> 16));	/* BLT dest hi */

	/* BLT source: set to 0 (is a dummy here anyway) */
	WGfx(GR2C, 0x00);	/* BLT src low */
	WGfx(GR2D, 0x00);	/* BLT src mid */
	WGfx(GR2E, 0x00);	/* BLT src hi */

	/* This is a ColorExpand Blt, using the */
	/* same color for foreground and background */
	WGfx(GR0, color);	/* foreground color */
	WGfx(GR1, color);	/* background color */

	/* BLT mode: color expand, Enable 8x8 copy (faster?) */
	WGfx(GR30, 0xc0);	/* BLT mode */

	/* BLT ROP: SrcCopy */
	WGfx(GR32, 0x0d);	/* BLT ROP */

	/* and finally: GO! */
	WGfx(GR31, 0x02);	/* BLT Start/status */
}

/*************************************************************************
	clgen_fb_ioctl()

	performs various (none yet) ioctl()s on the current screen mode
**************************************************************************/

static int clgen_fb_ioctl(struct inode *inode, struct file *file,
    unsigned int cmd, unsigned long arg, int con, int fbidx)
{
	int slotnum;

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		DEBUG printk(KERN_WARNING "clgen: Warning: Board not found in ioctl)!!\n");
		return 0;
	}

	g_slotnum = slotnum;

	switch(cmd)
	{
#if 0
		case FBIOSWITCH_MONIBIT:
			/* set monitor bit */
			if (arg == 0)
			{
				/* set to "output from Amiga" */
				switch(clboards[slotnum].boardtype)
				{
					case BT_SD64:
						clboards[slotnum].SFR &= 0xde;
						WSFR(clboards[slotnum].SFR);
						break;

					case BT_PICCOLO:
						clboards[slotnum].SFR &= 0xd7;
						WSFR(clboards[slotnum].SFR);
						break;

					case BT_PICASSO:
						clboards[slotnum].SFR = 0xff;
						WSFR2(clboards[slotnum].SFR);
						break;

					case BT_SPECTRUM:
						clboards[slotnum].SFR = 0x4f;
						WSFR(clboards[slotnum].SFR);
						break;

					case BT_PICASSO4:
						/* nothing - dummy func */
						break;

					default:
						DEBUG printk("clgen: Huh?\n");
						break;
				}
			}
			else
			{
				/* set to "output from VGA controller" */
				switch(clboards[slotnum].boardtype)
				{
					case BT_SD64:
						clboards[slotnum].SFR |= 0x21;
						WSFR(clboards[slotnum].SFR);
						break;

					case BT_PICCOLO:
						clboards[slotnum].SFR |= 0x28;
						WSFR(clboards[slotnum].SFR);
						break;

					case BT_PICASSO:
						clboards[slotnum].SFR = 0xff;
						WSFR(clboards[slotnum].SFR);
						break;

					case BT_SPECTRUM:
						clboards[slotnum].SFR = 0x6f;
						WSFR(clboards[slotnum].SFR);
						break;

					case BT_PICASSO4:
						/* nothing - dummy func */
						break;

					default:
						DEBUG printk("clgen: Huh?\n");
						break;
				}
			}
			break;
#endif
		default:
			return -EINVAL;
			break;
	}

	return(0);
}

/*************************************************************************
	clgen_fb_pan_display()

	performs display panning - provided hardware permits this
**************************************************************************/
static int clgen_fb_pan_display(struct fb_var_screeninfo *var, int val, int fbidx)
{
	int xbyte = 0;
	unsigned long base;
	unsigned char tmp = 0, tmp2 = 0, xpix;
	int slotnum;

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		DEBUG printk("clgen: Warning: Board not found in pan_display)!!\n");
		return 0;
	}

	g_slotnum = slotnum;

	/* pan display: check bounds, and if ok, set hardware registers */
	/* and shadow in clboard structure accordingly. */

	if (var->xoffset > (var->xres_virtual - var->xres))
		return(-EINVAL);
	if (var->yoffset > (var->yres_virtual - var->yres))
		return(-EINVAL);

	clboards[slotnum].xoffset = var->xoffset;
	clboards[slotnum].yoffset = var->yoffset;

	if (clboards[slotnum].bpp == 1)
	{
		xbyte = var->xoffset / 8;  /* scale down to (mono)bytes */
	}
 	else if (clboards[slotnum].bpp == 8) 
	{
		xbyte = var->xoffset; /* one byte per pixel in 8 bit pseudocolor */
	}
 	else if (clboards[slotnum].bpp == 32) 
	{
		xbyte = var->xoffset * 4; /* 4 bytes per pixel in 32 bit truecolor */
	}
	else
		printk(KERN_WARNING "clgen: This must not happen. Reconfigure the plasma grid.\n");

	base = var->yoffset * clboards[slotnum].line_length + xbyte;

	if (clboards[slotnum].bpp > 1)
	{
		base = base / 4;
		xpix = (unsigned char)((xbyte % 4) * 2);
	}
	else
	{
		/* base is already correct */
		xpix = (unsigned char)(var->xoffset % 8);
	}

	/* lower 8 + 8 bits of screen start address */
	WCrt(CRTD, (unsigned char)(base & 0xff));
	WCrt(CRTC, (unsigned char)(base >> 8));

	/* construct bits 16, 17 and 18 of screen start address */
	if (base & 0x10000)
		tmp |= 0x01;
	if (base & 0x20000)
		tmp |= 0x04;
	if (base & 0x40000)
		tmp |= 0x08;
	tmp2 = (RCrt(CRT1B) & 0xf2) | tmp; /* 0xf2 is %11110010, exclude tmp bits */
	WCrt(CRT1B, tmp2);
	/* construct bit 19 of screen start address (only on SD64) */
	if (clboards[slotnum].boardtype == BT_SD64 ||
		clboards[slotnum].boardtype == BT_PICASSO4)
	{
		tmp2 = 0;
		if (base & 0x80000)
			tmp2 = 0x80;
		WCrt(CRT1D, tmp2);
	}

	/* write pixel panning value to AR33; this does not quite work in 8bpp */
	/* ### Piccolo..? Will this work? */
	if (clboards[slotnum].bpp == 1)
		WAttr(AR33, xpix);

	return(0);
}

/*************************************************************************
	clgen_fb_get_monitorspec()

	obtains current monitor specifications
**************************************************************************/
static int clgen_get_monitorspec(struct fb_monitorspec *spec, int val, int fbidx)
{
	int slotnum;

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		DEBUG printk(KERN_WARNING "clgen: Warning: Board not found in get_monispec)!!\n");
		return 0;
	}

	spec->hfmin = clboards[slotnum].mon_hfmin;
	spec->hfmax = clboards[slotnum].mon_hfmax;
	spec->vfmin = clboards[slotnum].mon_vfmin;
	spec->vfmax = clboards[slotnum].mon_vfmax;

	return 0;
}

/*************************************************************************
	clgen_fb_put_monitorspec()

	sets new values for current monitor specifications
**************************************************************************/
static int clgen_put_monitorspec(struct fb_monitorspec *spec, int val, int fbidx)
{
	int slotnum;

	slotnum = find_clboard(fbidx);
	if (slotnum == -1)
	{
		DEBUG printk(KERN_WARNING "clgen: Warning: Board not found in put_monispec)!!\n");
		return 0;
	}

	clboards[slotnum].mon_hfmin = spec->hfmin;
	clboards[slotnum].mon_hfmax = spec->hfmax;
	clboards[slotnum].mon_vfmin = spec->vfmin;
	clboards[slotnum].mon_vfmax = spec->vfmax;

	return 0;
}

static struct fb_ops clgen_fb_ops = {
    clgen_fb_get_fix, clgen_fb_get_var, clgen_fb_set_var,
    clgen_fb_get_cmap, clgen_fb_set_cmap, clgen_fb_pan_display,
    clgen_fb_ioctl, clgen_get_monitorspec, clgen_put_monitorspec };


/********************************************************************/
/* find_clboard() - locate clboard struct for an fbidx              */
/********************************************************************/
int find_clboard(int fbidx)
{
	int i;

	for (i = 0; i < MAX_NUM_BOARDS; i++)
	{
		if (clboards[i].fbnum == fbidx)
			return i;
	}
	return -1;
}


/********************************************************************/
/* clgen_fb_init() - master initialization function                 */
/********************************************************************/
struct fb_info *clgen_fb_init(long *mem_start)
{
	int key = 0, key2 = 0, key3 = 0;
	struct ConfigDev *cd = NULL, *cd2 = NULL, *cd3 = NULL;
	int err, i, slotnum, p4flag = 0;
	int btype;
	static int firstpass = 0;
	static struct fb_var_screeninfo init_var;

	printk(KERN_INFO "clgen: Driver for Cirrus Logic based graphic boards, v" CLGEN_VERSION "\n");

	fbhw = &clgen_hwswitch;
	/* ### fbhw->init()? */

	/* Free all slots */
	for (i = 0; i < MAX_NUM_BOARDS; i++)
		clboards[i].fbnum = -1;

	do
	{
		btype = -1;

		key  = zorro_find(MANUF_HELFRICH2, PROD_SD64_RAM, 0, 0);
		key2 = zorro_find(MANUF_HELFRICH2, PROD_SD64_REG, 0, 0);
		if (key != 0)
		{
			btype = BT_SD64;
			printk(KERN_INFO "clgen: SD64 board detected; ");
		}

		/* none found, check next board type (Piccolo) */
		if (btype == -1)
		{
			key  = zorro_find(MANUF_HELFRICH2, PROD_PICCOLO_RAM, 0, 0);
			key2 = zorro_find(MANUF_HELFRICH2, PROD_PICCOLO_REG, 0, 0);
			if (key != 0)
			{
				btype = BT_PICCOLO;
				printk(KERN_INFO "clgen: Piccolo board detected; ");
			}
		}

		/* none found, check next board type (Picasso II) */
		if (btype == -1)
		{
			key  = zorro_find(MANUF_VILLAGE_TRONIC, PROD_PICASSO_II_RAM, 0, 0);
			key2 = zorro_find(MANUF_VILLAGE_TRONIC, PROD_PICASSO_II_REG, 0, 0);
			if (key != 0)
			{
				btype = BT_PICASSO;
				printk(KERN_INFO "clgen: Picasso II board detected; ");
			}
		}

		/* none found, check next board type (Spectrum) */
		if (btype == -1)
		{
			key  = zorro_find(MANUF_GVP2, PROD_SPECTRUM_RAM, 0, 0);
			key2 = zorro_find(MANUF_GVP2, PROD_SPECTRUM_REG, 0, 0);
			if (key != 0)
			{
				btype = BT_SPECTRUM;
				printk(KERN_INFO "clgen: Spectrum board detected; ");
			}
		}


		/* none found, check next board type (Picasso 4, 4th ProdID (Zorro 3)) */
		if (btype == -1)
		{
			key  = zorro_find(MANUF_VILLAGE_TRONIC, PROD_PICASSO_IV_4, 0, 0);
			if (key != 0)
			{
				btype = BT_PICASSO4;
				printk(KERN_INFO "clgen: Picasso IV board detected; ");
				p4flag = 0;
			}
		}


		/* none found, check next board type (Picasso 4 In Zorro 2) */
		if (btype == -1)
		{
			key  = zorro_find(MANUF_VILLAGE_TRONIC, PROD_PICASSO_IV, 0, 0);
			key2 = zorro_find(MANUF_VILLAGE_TRONIC, PROD_PICASSO_IV_2, 0, 0);
			key3 = zorro_find(MANUF_VILLAGE_TRONIC, PROD_PICASSO_IV_3, 0, 0);
			if (key != 0)
			{
				btype = BT_PICASSO4;
				printk(KERN_INFO "clgen: Picasso IV board (in Zorro II) detected; ");
				p4flag = 1;
			}
		}


		/* When we get here with != -1, we have a board to be init'ed. */
		if (btype != -1)
		{
			cd = zorro_get_board(key);
			cd2 = zorro_get_board(key2);
			if (p4flag)
				cd3 = zorro_get_board(key3);

			/* search for a free slot in the clboards[] array */
			slotnum = find_clboard(-1);
			if (slotnum == -1)
			{
				printk(KERN_ERR "clgen: Warning: Could not get a free slot for board!\n");
				return(NULL);
			}

			clboards[slotnum].p4flag = 0;

			DEBUG printk(KERN_INFO "clgen: Found a free slot for this board at array elem #%d\n", slotnum);
			/* set address for RAM area of board */
			if (btype != BT_PICASSO4)
			{
				clboards[slotnum].PhysAddr = (unsigned long)cd->cd_BoardAddr;
				clboards[slotnum].size = cd->cd_BoardSize;
			}
			else
			{
				/* p4flag set: P4 in Z2 mode, else in Z3 */
				if (!p4flag)
				{
					/* To be precise, for the P4 this is not the */
					/* begin of the board, but the begin of RAM. */
					clboards[slotnum].PhysAddr = (unsigned long)cd->cd_BoardAddr + 0x1000000;
					clboards[slotnum].size = cd->cd_BoardSize;
				}
				else
				{
					clboards[slotnum].PhysAddr = (unsigned long)cd->cd_BoardAddr;
					clboards[slotnum].size = cd->cd_BoardSize;
					clboards[slotnum].smallboard = TRUE;
					if (key2)
					{
						DEBUG printk(KERN_INFO "clgen: detected Picasso IV in Z2 with both banks enabled (4 MB)\n");
						clboards[slotnum].size += cd2->cd_BoardSize;
						clboards[slotnum].smallboard = FALSE;
					}
					else
					{
						DEBUG printk(KERN_INFO "clgen: detected Picasso IV in Z2 with one bank enabled (2 MB)\n");
					}
					/* flag: this board needs no RAM amount check */
					clboards[slotnum].p4flag = 1;
				}
			}

			/* map the physical board address to a virtual address in */
			/* kernel space; but only if it's really in Zorro III address */
			/* space; Z2 boards (like the Picasso) are in the low 16 MB region */
			/* which is automatically mapped so that no kernel_map is */
			/* necessary there (only Z2 address translation) */
			if (btype != BT_PICASSO4 || p4flag)
			{
				if (clboards[slotnum].PhysAddr > 0x01000000)
				{
					clboards[slotnum].VirtAddr = (unsigned char *)kernel_map(
						(unsigned long)cd->cd_BoardAddr, 
						cd->cd_BoardSize, KERNELMAP_NOCACHE_SER, mem_start);
				}
				else
					clboards[slotnum].VirtAddr = (unsigned char *)(ZTWO_VADDR(clboards[slotnum].PhysAddr));

				/* for most boards, start of DRAM = start of board */
				clboards[slotnum].VirtRAMAddr = clboards[slotnum].VirtAddr;
			}
			else
			{
				/* for P4 (Z3), map in its address space in 2 chunks (### TEST! ) */
				/* (note the ugly hardcoded 16M number) */
				clboards[slotnum].VirtRegBase = (unsigned char *)kernel_map(
					(unsigned long)cd->cd_BoardAddr, 
					16777216, KERNELMAP_NOCACHE_SER, mem_start);

				clboards[slotnum].VirtRegBase += 0x600000;

				clboards[slotnum].VirtRAMAddr = (unsigned char *)kernel_map(
					(unsigned long)cd->cd_BoardAddr + 16777216, 
					16777216, KERNELMAP_NOCACHE_SER, mem_start);
			}

			printk(KERN_INFO "clgen: Virtual address for board set to: $%p\n", clboards[slotnum].VirtAddr);
			printk(KERN_INFO "clgen: (RAM start set to: $%p)\n", clboards[slotnum].VirtRAMAddr);
#if 0
			DEBUG printk(KERN_INFO "clgen: Virtual address for board set to: $%p\n", clboards[slotnum].VirtAddr);
			DEBUG printk(KERN_INFO "clgen: (RAM start set to: $%p)\n", clboards[slotnum].VirtRAMAddr);
#endif

			/* set address for REG area of board */
			if (btype != BT_PICASSO4 || p4flag)
			{
				if (!p4flag)
					clboards[slotnum].RegBase = cd2->cd_BoardAddr;
				else
					clboards[slotnum].RegBase = cd3->cd_BoardAddr + 0x10000;

				clboards[slotnum].VirtRegBase = 
					(unsigned char *)ZTWO_VADDR(clboards[slotnum].RegBase);
			}


			/* set defaults for monitor specifications */
			/* these should be careful values for most, even old, monitors */

			/* set them only if they haven't been set before */
			if (clboards[slotnum].mon_hfmin == -1)
			{
				clboards[slotnum].mon_hfmin = 30000;
				clboards[slotnum].mon_hfmax = 38000;
				clboards[slotnum].mon_vfmin = 50;
				clboards[slotnum].mon_vfmax = 90;
			}

			/* set up a few more things, register framebuffer driver etc */
			err = register_framebuffer("CL Generic", 
				&clboards[slotnum].node, &clgen_fb_ops,
				NUM_TOTAL_MODES, clgen_fb_predefined);
			if (err)
			{
				printk(KERN_ERR "clgen: WARNING - could not register fb device; err = %d!\n", err);
				return(NULL);
			}

			/* create fbnum from inode */
			clboards[slotnum].fbnum = GET_FB_IDX(clboards[slotnum].node);
			clboards[slotnum].boardtype = btype;

			/* mark this board as "autoconfigured" */
			zorro_config_board(key, 0);
			if (btype != BT_PICASSO4)
				zorro_config_board(key2, 0);
			else
			{
				/* configure the other 2 areas of Z2 P4 if found */
				if (p4flag)
				{
					if (key2)
						zorro_config_board(key2, 0);
					if (key3)
						zorro_config_board(key3, 0);
				}
			}

			num_inited++;
			DEBUG printk(KERN_INFO "clgen: Number of init'ed CLGEN boards at this time: %d\n", num_inited);

			DEBUG printk(KERN_INFO "clgen: Registering %d predefined modes\n", NUM_TOTAL_MODES);
			DEBUG printk(KERN_INFO "clgen: node set to %d\n", clboards[slotnum].node);
			DEBUG printk(KERN_INFO "clgen: fbnum for this board set to: %d\n", clboards[slotnum].fbnum);
			DEBUG printk(KERN_INFO "clgen: VirtAddr is now at $%p\n", clboards[slotnum].VirtAddr);

			DEBUG printk(KERN_INFO "clgen: BoardType set to: ");
			switch(clboards[slotnum].boardtype)
			{
				case BT_PICCOLO:
					DEBUG printk("Piccolo\n");
					break;
				case BT_PICASSO:
					DEBUG printk("Picasso\n");
					break;
				case BT_SD64:
					DEBUG printk("Piccolo SD64\n");
					break;
				case BT_SPECTRUM:
					DEBUG printk("GVP Spectrum\n");
					break;
				case BT_PICASSO4:
					DEBUG printk("Picasso IV\n");
					break;
				default: 
					DEBUG printk("Unidentified - Huh?\n");
					break;
			}

			/* now that we know the board has been registered n' stuff, we */
			/* can finally initialize it to a default mode (640x480) */
			/* open initial screen: 640x480, 31.5 kHz, 60 Hz */

			/* initialize chip */
			init_vgachip(clboards[slotnum].fbnum);

			/* board gets init'ed with 640x480 mode, 31.25 kHz, 60 Hz, 8bit */
			init_var = clgen_fb_predefined[g_startmode];
			init_var.activate = FB_ACTIVATE_NOW;
			err = clgen_fb_set_var(&init_var, 0, clboards[slotnum].fbnum);
			if (err)
			{
				/* This monitor limits/video mode combo */
				/* SHOULD never fail. */
				printk(KERN_WARNING "clgen: Unable to set video mode; resetting to default.\n");
				clboards[slotnum].mon_hfmin = 30000;
				clboards[slotnum].mon_hfmax = 38000;
				clboards[slotnum].mon_vfmin = 50;
				clboards[slotnum].mon_vfmax = 90;
				init_var = clgen_fb_predefined[1];
				init_var.activate = FB_ACTIVATE_NOW;
				clgen_fb_set_var(&init_var, 0, clboards[slotnum].fbnum);
			}

			/* only the first board found can be the console */
			if (firstpass == 0)
			{
				firstpass = 1;
				strcpy(fb_info.modename, "CLGEN-default");
				fb_info.disp = disp;
				fb_info.switch_con = &clgen_switch;
				fb_info.updatevar = &clgen_updatevar;
				fb_info.blank = &clgen_blank;
				return(&fb_info);
			}
		}
	} while (btype != -1);
	return(NULL);    /* shouldn't ever come here.. */

	/* done */
}

/**************************/
/* small support functions copied over from cyberfb.c. */
/* a typical meeting hack.. dunno if this can work at all. It's 1:30 a.m. */
/* Too many beers. */
/**************************/

static int clgen_switch(int con)
{
	/* Do we have to save the colormap? */
	if (disp[currcon].cmap.len)
		do_fb_get_cmap(&disp[currcon].cmap, &disp[currcon].var, 1);

	/* ### WARNING - requires fbidx patch here! */
	currcon = con;
	/* set mode in harware, WITH clearflag set */
	clgen_set_var(&disp[con].var, 1);
	/* Install new colormap */
	do_install_cmap(con);
	return(0);
}

static int clgen_updatevar(int con)
{
   return(0);
}

static void clgen_blank(int blank)
{
	unsigned char val;

	if (blank)
		/* activate blanker */
	{
		val = RSeq(SR1);
		WSeq(SR1, val | 0x20); /* set "FullBandwidth" bit */
	}
	else
	{
		val = RSeq(SR1);
		WSeq(SR1, val & 0xdf); /* clear "FullBandwidth" bit */
	}
/*	printk(KERN_WARNING "CLGEN: Blank-di-blank: called with blank=%d...\n", blank); */
}



/******************************************************************/
/* clgen_probe() - tiny function to see if the required board(s)  */
/* is there at all. Mimics the behaviour of Cyber_probe()         */
/******************************************************************/

int clgen_probe()
{
	int bla;

	/* if the DRAM area of any of these boards is found, I can be */
	/* pretty sure the rest is also there - enough anyway to attempt */	
	/* to start to initialize the console on it. */

	/* SD64? */
	bla = zorro_find(MANUF_HELFRICH2, PROD_SD64_RAM, 0, 0);
	if (bla)
		return bla;

	/* Piccolo? */
	bla = zorro_find(MANUF_HELFRICH2, PROD_PICCOLO_RAM, 0, 0);
	if (bla)
		return bla;

	/* Picasso II? */
	bla = zorro_find(MANUF_VILLAGE_TRONIC, PROD_PICASSO_II_RAM, 0, 0);
	if (bla)
		return bla;

	/* GVP Spectrum? */
	bla = zorro_find(MANUF_GVP2, PROD_SPECTRUM_RAM, 0, 0);
	if (bla)
		return bla;

	/* Picasso IV in Z3? */
	bla = zorro_find(MANUF_VILLAGE_TRONIC, PROD_PICASSO_IV_4, 0, 0);
	if (bla)
		return bla;

	/* Picasso IV in Z2? */
	bla = zorro_find(MANUF_VILLAGE_TRONIC, PROD_PICASSO_IV, 0, 0);
	if (bla)
		return bla;

	return 0;
}


/*****************************************************************/
/* clgen_video_setup() might be used later for parsing possible  */
/* arguments to the video= bootstrap parameter. Right now, there */
/* is nothing I do here.                                         */
/*****************************************************************/

void clgen_video_setup(char *options, int *ints)
{
	char *this_opt;
	char mcap_spec[80];

	/* ### Warning BAD HACK - will not work with multiple boards */
	clboards[0].mon_hfmin = -1;
	clboards[0].mon_hfmax = -1;
	clboards[0].mon_vfmin = -1;
	clboards[0].mon_vfmax = -1;

	mcap_spec[0] = '\0';
	fb_info.fontname[0] = '\0';

	if (!options || !*options)
		return;

	if (!strncmp(options, "clgen:", 6))
		options += 6;

	for (this_opt = strtok(options, ","); this_opt; this_opt = strtok(NULL, ","))
	{
		char *p;
		if (!strncmp(this_opt, "monitorcap:", 11))
			strcpy(mcap_spec, this_opt+11);
		else if (!strncmp(this_opt, "mode:", 5))
		{
			p = this_opt + 5;
			/* slightly hardcoded indices into mode table, sorry */
			if (!strncmp(p, "low", 3))
				g_startmode = 1;
			else if (!strncmp(p, "med", 3))
				g_startmode = 2;
			else if (!strncmp(p, "high", 4))
				g_startmode = 3;
			else
				g_startmode = 1;
		}
	}

	if (*mcap_spec)
	{
		char *p;
		int vmin, vmax, hmin, hmax;

	/* Format for monitor capabilities is: <Vmin>;<Vmax>;<Hmin>;<Hmax>
	 * <V*> vertical freq. in Hz
	 * <H*> horizontal freq. in kHz
	 */

		if (!(p = strtoke(mcap_spec, ";")) || !*p)
			goto cap_invalid;
		vmin = simple_strtoul(p, NULL, 10);
		if (vmin <= 0)
			goto cap_invalid;
		if (!(p = strtoke(NULL, ";")) || !*p)
			goto cap_invalid;
		vmax = simple_strtoul(p, NULL, 10);
		if (vmax <= 0 || vmax <= vmin)
			goto cap_invalid;
		if (!(p = strtoke(NULL, ";")) || !*p)
			goto cap_invalid;
		hmin = 1000 * simple_strtoul(p, NULL, 10);
		if (hmin <= 0)
			goto cap_invalid;
		if (!(p = strtoke(NULL, "")) || !*p)
			goto cap_invalid;
		hmax = 1000 * simple_strtoul(p, NULL, 10);
		if (hmax <= 0 || hmax <= hmin)
			goto cap_invalid;

		/* ### Warning BAD HACK - will not work with multiple boards */
		clboards[0].mon_hfmin = hmin;
		clboards[0].mon_hfmax = hmax;
		clboards[0].mon_vfmin = vmin;
		clboards[0].mon_vfmax = vmax;
		printk(KERN_INFO "clgen: Monitor limits set to H: %d - %d Hz, V: %d - %d Hz\n", hmin, hmax, vmin, vmax);
cap_invalid:
		;
	}
}


/*
 * A strtok which returns empty strings, too
 * (Taken from Geert's amiga/amifb.c)
 */

static char *strtoke(char *s,const char *ct)
{
	char *sbegin, *send;
	static char *ssave = NULL;

	sbegin  = s ? s : ssave;
	if (!sbegin)
		return NULL;
	if (*sbegin == '\0') {
		ssave = NULL;
		return NULL;
	}
	send = strpbrk(sbegin, ct);
	if (send && *send != '\0')
		*send++ = '\0';
	ssave = send;
	return sbegin;
}
