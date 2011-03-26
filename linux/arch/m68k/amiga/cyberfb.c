/*
 * linux/arch/m68k/amiga/cyberfb.c -- Low level implementation of the
 *                                    Cybervision frame buffer device
 *
 *    Copyright (C) 1996 Martin Apel
 *                       Geert Uytterhoeven
 *
 *
 * This file is based on the Amiga frame buffer device (amifb.c):
 *
 *    Copyright (C) 1995 Geert Uytterhoeven
 *
 *
 * History:
 *   - 22 Dec 95: Original version by Martin Apel
 *   - 05 Jan 96: Geert: integration into the current source tree
 *
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 */


#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/malloc.h>
#include <linux/delay.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/zorro.h>
#include <asm/pgtable.h>
#include <asm/amigahw.h>
#include <linux/fb.h>
#include "s3blit.h"


/* FN */
#define FB_MODES_SHIFT    5 /* 32 modes per framebuffer */
#define GET_FB_IDX(node) (MINOR(node) >> FB_MODES_SHIFT)

#define arraysize(x)    (sizeof(x)/sizeof(*(x)))

#if 1
#define vgawb_3d(reg,dat) \
	if (cv3d_on_zorro2) { \
		*((unsigned char volatile *)((Cyber_vcode_switch_base) + 0x04)) = \
		(0x01 & 0xffff); asm volatile ("nop"); \
	} \
		(*((unsigned char *)(CyberVGARegs + (reg ^ 3))) = dat); \
	if (cv3d_on_zorro2) { \
		*((unsigned char volatile *)((Cyber_vcode_switch_base) + 0x04)) = \
		(0x02 & 0xffff); asm volatile ("nop"); \
	}
#define vgaww_3d(reg,dat) \
		(*((unsigned word *)(CyberVGARegs + (reg ^ 2))) = swab16(dat))
#define vgawl_3d(reg,dat) \
		(*((unsigned long *)(CyberVGARegs + reg)) = swab32(dat))
#else
/*
 * Dunno why this doesn't work at the moment - we'll have to look at
 * it later.
 */
#define vgawb_3d(reg,dat) \
		(*((unsigned char *)(CyberRegs + 0x8000 + reg)) = dat)
#define vgaww_3d(reg,dat) \
		(*((unsigned word *)(CyberRegs + 0x8000 + reg)) = dat)
#define vgawl_3d(reg,dat) \
		(*((unsigned long *)(CyberRegs + 0x8000 + reg)) = dat)
#endif

/*
 * We asume P5 mapped the big-endian version of these registers.
 */
#define wb_3d(reg,dat) \
		(*((unsigned char volatile *)(CyberRegs + reg)) = dat)
#define ww_3d(reg,dat) \
		(*((unsigned word volatile *)(CyberRegs + reg)) = dat)
#define wl_3d(reg,dat) \
		(*((unsigned long volatile *)(CyberRegs + reg)) = dat)

#define rl_3d(reg) \
		(*((unsigned long volatile *)(CyberRegs + reg)))


#define wb_64(reg,dat) (*((unsigned char volatile *)CyberRegs + reg) = dat)
#define ww_64(reg,dat) (*((unsigned short volatile *)(CyberRegs + reg)) = dat)


struct Cyber_fb_par {
	int xres;
	int yres;
	int bpp;
	int pixclock;
};

static struct Cyber_fb_par current_par;

static int current_par_valid = 0;
static int currcon = 0;

static struct display disp[MAX_NR_CONSOLES];
static struct fb_info fb_info;

static int node;        /* node of the /dev/fb?current file */


/*
 *    Switch for Chipset Independency
 */

static struct fb_hwswitch {

	/* Initialisation */

	int (*init)(void);

	/* Display Control */

	int (*encode_fix)(struct fb_fix_screeninfo *fix, struct Cyber_fb_par *par);
	int (*decode_var)(struct fb_var_screeninfo *var, struct Cyber_fb_par *par);
	int (*encode_var)(struct fb_var_screeninfo *var, struct Cyber_fb_par *par);
	int (*getcolreg)(u_int regno, u_int *red, u_int *green, u_int *blue,
			 u_int *transp);
	int (*setcolreg)(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp);
	void (*blank)(int blank);
} *fbhw;


/*
 *    Frame Buffer Name
 */

static char Cyber_fb_name[16] = "Cybervision";


/*
 *    Cybervision Graphics Board
 */


static int CyberKey = 0;
static unsigned char Cyber_colour_table [256][4];
static unsigned long CyberMem;
static unsigned long CyberSize;
volatile unsigned char *CyberRegs;
static volatile unsigned long CyberVGARegs;
static unsigned long Cyber_register_base;
static unsigned long Cyber_vcode_switch_base;
static unsigned char cv3d_on_zorro2;

static long *memstart;


/*
 *    Predefined Video Mode Names
 */

static char *Cyber_fb_modenames[] = {

	/*
	 *    Autodetect (Default) Video Mode
	 */

	"default",

	/*
	 *    Predefined Video Modes
	 */
    
	"640x480-8",
	"800x600-8",
	"1024x768-8",
	"1152x886-8",
	"1280x1024-8",
	"1600x1200-8",
	"800x600-16",

	/*
	 *    Dummy Video Modes
	 */

	"dummy", "dummy", "dummy", "dummy", "dummy", "dummy", "dummy", "dummy",
	"dummy", "dummy", "dummy", "dummy", "dummy", "dummy", "dummy", "dummy",

	/*
	 *    User Defined Video Modes
	 *
	 *    This doesn't work yet!!
	 */

	"user0", "user1", "user2", "user3", "user4", "user5", "user6", "user7"
};


/*
 *    Predefined Video Mode Definitions
 */

static struct fb_var_screeninfo Cyber_fb_predefined[] = {

	/*
	 *    Autodetect (Default) Video Mode
	 */

	{ 0, },

	/*
	 *    Predefined Video Modes
	 */
    
	{
	/* Cybervision 8 bpp */
	640, 480, 640, 480, 0, 0, 8, 0,
	{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
	0, 0, -1, -1, FB_ACCEL_NONE, 12500, 64, 96, 35, 12, 112, 2,
	FB_SYNC_COMP_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	}, {
	/* Cybervision 8 bpp */
	800, 600, 800, 600, 0, 0, 8, 0,
	{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
	0, 0, -1, -1, FB_ACCEL_NONE, 12500, 64, 96, 35, 12, 112, 2,
	FB_SYNC_COMP_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	}, {
	/* Cybervision 8 bpp */
	1024, 768, 1024, 768, 0, 0, 8, 0,
	{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
	0, 0, -1, -1, FB_ACCEL_NONE, 12500, 64, 96, 35, 12, 112, 2,
	FB_SYNC_COMP_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	}, {
	/* Cybervision 8 bpp */
	1152, 886, 1152, 886, 0, 0, 8, 0,
	{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
	0, 0, -1, -1, FB_ACCEL_NONE, 12500, 64, 96, 35, 12, 112, 2,
	FB_SYNC_COMP_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	}, {
	/* Cybervision 8 bpp */
	1280, 1024, 1280, 1024, 0, 0, 8, 0,
	{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
	0, 0, -1, -1, FB_ACCEL_NONE, 12500, 64, 96, 35, 12, 112, 2,
	FB_SYNC_COMP_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	}, {
	/* Cybervision 8 bpp */
	1600, 1200, 1600, 1200, 0, 0, 8, 0,
	{0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
	0, 0, -1, -1, FB_ACCEL_NONE, 12500, 64, 96, 35, 12, 112, 2,
	FB_SYNC_COMP_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	}, {
	/* Cybervision 16 bpp */
	800, 600, 800, 600, 0, 0, 16, 0,
	{11, 5, 0}, {5, 6, 0}, {0, 5, 0}, {0, 0, 0},
	0, 0, -1, -1, FB_ACCEL_NONE, 25000, 64, 96, 35, 12, 112, 2,
	FB_SYNC_COMP_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT, FB_VMODE_NONINTERLACED
	},

	/*
	 *    Dummy Video Modes
	 */

	{ 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, },
	{ 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, },

	/*
	 *    User Defined Video Modes
	 */

	{ 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }, { 0, }
};


#define NUM_TOTAL_MODES    arraysize(Cyber_fb_predefined)
#define NUM_PREDEF_MODES   7


static int Cyberfb_inverse = 0;
static int Cyberfb_mode = 0;

static int CV3D = 0;


/*
 *    Interface used by the world
 */

int Cyber_probe(void);
void Cyber_video_setup(char *options, int *ints);

static int Cyber_fb_get_fix(struct fb_fix_screeninfo *fix, int con, int fbidx);
static int Cyber_fb_get_var(struct fb_var_screeninfo *var, int con, int fbidx);
static int Cyber_fb_set_var(struct fb_var_screeninfo *var, int con, int fbidx);
static int Cyber_fb_get_cmap(struct fb_cmap *cmap, int kspc,
			     int con, int fbidx);
static int Cyber_fb_set_cmap(struct fb_cmap *cmap, int kspc,
			     int con, int fbidx);
static int Cyber_fb_pan_display(struct fb_var_screeninfo *var,
				int con, int fbidx);
static int Cyber_fb_ioctl(struct inode *inode, struct file *file, u_int cmd,
                          u_long arg, int con, int fbidx);
static int Cyber_fb_get_monitorspec(struct fb_monitorspec *spec,
				    int val, int fbidx);
static int Cyber_fb_put_monitorspec(struct fb_monitorspec *spec,
				    int val, int fbidx);


/*
 *    Interface to the low level console driver
 */

struct fb_info *Cyber_fb_init(unsigned long *mem_start);
static int Cyberfb_switch(int con);
static int Cyberfb_updatevar(int con);
static void Cyberfb_blank(int blank);


/*
 *    Accelerated Functions used by the low level console driver
 */

void Cyber_WaitQueue(u_short fifo);
void Cyber_WaitBlit(void);
void Cyber_BitBLT(u_short curx, u_short cury, u_short destx, u_short desty,
                  u_short width, u_short height, u_short mode);
void Cyber_RectFill(u_short x, u_short y, u_short width, u_short height,
                    u_short mode, u_short color);
void Cyber_MoveCursor(u_short x, u_short y);


/*
 *   Hardware Specific Routines
 */

static int Cyber_init(void);
static int Cyber_encode_fix(struct fb_fix_screeninfo *fix,
			    struct Cyber_fb_par *par);
static int Cyber_decode_var(struct fb_var_screeninfo *var,
			    struct Cyber_fb_par *par);
static int Cyber_encode_var(struct fb_var_screeninfo *var,
			    struct Cyber_fb_par *par);
static int Cyber_getcolreg(u_int regno, u_int *red, u_int *green, u_int *blue,
			   u_int *transp);
static int Cyber_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			   u_int transp);
static void Cyber_blank(int blank);


/*
 *    Internal routines
 */

static void Cyber_fb_get_par(struct Cyber_fb_par *par);
static void Cyber_fb_set_par(struct Cyber_fb_par *par);
static int do_fb_set_var(struct fb_var_screeninfo *var, int isactive);
static struct fb_cmap *get_default_colormap(int bpp);
static int do_fb_get_cmap(struct fb_cmap *cmap, struct fb_var_screeninfo *var,
                          int kspc);
static int do_fb_set_cmap(struct fb_cmap *cmap, struct fb_var_screeninfo *var,
                          int kspc);
static void do_install_cmap(int con);
static void memcpy_fs(int fsfromto, void *to, void *from, int len);
static void copy_cmap(struct fb_cmap *from, struct fb_cmap *to, int fsfromto);
static int alloc_cmap(struct fb_cmap *cmap, int len, int transp);
static void Cyber_fb_set_disp(int con);
static int get_video_mode(const char *name);


/* -------------------- Hardware specific routines ------------------------- */


/*
 *    Initialization
 *
 *    Set the default video mode for this chipset. If a video mode was
 *    specified on the command line, it will override the default mode.
 */

static int Cyber_init(void)
{
	int i;
	char size;
	volatile unsigned long *CursorBase;
	unsigned long board_addr;
	struct ConfigDev *cd;

	cd = zorro_get_board (CyberKey);
	zorro_config_board (CyberKey, 0);
	board_addr = (unsigned long)cd->cd_BoardAddr;


	for (i = 0; i < 256; i++){
		Cyber_colour_table [i][0] = i;
		Cyber_colour_table [i][1] = i;
		Cyber_colour_table [i][2] = i;
		Cyber_colour_table [i][3] = 0;
	}

	*memstart = (*memstart + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	/* This includes the video memory as well as the S3 register set */

	if (!CV3D)
		CyberMem = kernel_map(board_addr + 0x01400000, 0x01000000,
				      KERNELMAP_NOCACHE_SER, memstart);
	else
		if((unsigned long)cd->cd_BoardAddr < 0x01000000){
			unsigned long cv3d_z2base = ZTWO_VADDR(board_addr);

			if (cd->cd_BoardSize < 0x400000){
				printk("CV3D detected in Z2 mode, board is <4MB and is not supported!\n");
				return -ENODEV;
			}

			/*
			 * Ok we got the board running in Z2 space.
			 */
			CyberVGARegs = cv3d_z2base + 0x3c0000;
			CyberRegs = (unsigned char *)(cv3d_z2base + 0x3e0000);
			CyberMem = cv3d_z2base;
			
			Cyber_register_base = (unsigned long)(cv3d_z2base + 0x3c8000);
			Cyber_vcode_switch_base = (unsigned long)(cv3d_z2base + 0x3a0000);
			cv3d_on_zorro2 = 1;
			printk("CV3D detected running in Z2 mode.\n");
		}else{
			CyberVGARegs = kernel_map(board_addr + 0x0c000000,
						       0x00010000,
						       KERNELMAP_NOCACHE_SER,
						       memstart);
			CyberRegs = (unsigned char *)kernel_map(board_addr +0x05000000,
						       0x00010000,
						       KERNELMAP_NOCACHE_SER,
						       memstart);
			CyberMem = kernel_map(board_addr + 0x04800000,
					      0x00400000,
					      KERNELMAP_NOCACHE_SER,
					      memstart);
			printk("CV3D detected running in Z3 mode.\n");
		}

	memset ((char*)CyberMem, 0, 1280*1024);

	if (CV3D){
		CyberSize = 0x00200000; /* 2 MB */

		vgawb_3d(0x3c8, 255);
		vgawb_3d(0x3c9, 56);
		vgawb_3d(0x3c9, 100);
		vgawb_3d(0x3c9, 160);

		vgawb_3d(0x3c8, 254);
		vgawb_3d(0x3c9, 0);
		vgawb_3d(0x3c9, 0);
		vgawb_3d(0x3c9, 0);

		/* Disable hardware cursor */
		vgawb_3d(S3_CRTC_ADR, S3_REG_LOCK2);
		vgawb_3d(S3_CRTC_DATA, 0xa0);
		vgawb_3d(S3_CRTC_ADR, S3_HGC_MODE);
		vgawb_3d(S3_CRTC_DATA, 0x00);
		vgawb_3d(S3_CRTC_ADR, S3_HWGC_DX);
		vgawb_3d(S3_CRTC_DATA, 0x00);
		vgawb_3d(S3_CRTC_ADR, S3_HWGC_DY);
		vgawb_3d(S3_CRTC_DATA, 0x00);

		return 0;
	}else{
		CyberRegs = (unsigned char*) (CyberMem + 0x00c00000);

		/* Disable hardware cursor */
		wb_64(S3_CRTC_ADR, S3_REG_LOCK2);
		wb_64(S3_CRTC_DATA, 0xa0);
		wb_64(S3_CRTC_ADR, S3_HGC_MODE);
		wb_64(S3_CRTC_DATA, 0x00);
		wb_64(S3_CRTC_ADR, S3_HWGC_DX);
		wb_64(S3_CRTC_DATA, 0x00);
		wb_64(S3_CRTC_ADR, S3_HWGC_DY);
		wb_64(S3_CRTC_DATA, 0x00);
	}

	/* Get memory size (if not 2MB it is 4MB) */
	*(CyberRegs + S3_CRTC_ADR) = S3_LAW_CTL;
	size = *(CyberRegs + S3_CRTC_DATA);
	if ((size & 0x03) == 0x02)
		CyberSize = 0x00200000; /* 2 MB */
	else
		CyberSize = 0x00400000; /* 4 MB */

	/* Initialize hardware cursor */
	CursorBase = (u_long *)((char *)(CyberMem) + CyberSize - 0x400);
	for (i=0; i < 8; i++){
		*(CursorBase  +(i*4)) = 0xffffff00;
		*(CursorBase+1+(i*4)) = 0xffff0000;
		*(CursorBase+2+(i*4)) = 0xffff0000;
		*(CursorBase+3+(i*4)) = 0xffff0000;
	}

	for (i=8; i < 64; i++){
		*(CursorBase  +(i*4)) = 0xffff0000;
		*(CursorBase+1+(i*4)) = 0xffff0000;
		*(CursorBase+2+(i*4)) = 0xffff0000;
		*(CursorBase+3+(i*4)) = 0xffff0000;
	}

	Cyber_setcolreg (255, 56, 100, 160, 0);
	Cyber_setcolreg (254, 0, 0, 0, 0);

	return 0;
}


/*
 *    This function should fill in the `fix' structure based on the
 *    values in the `par' structure.
 */

static int Cyber_encode_fix(struct fb_fix_screeninfo *fix,
			    struct Cyber_fb_par *par)
{
	memset(fix, 0, sizeof(struct fb_fix_screeninfo));
	strcpy(fix->id, Cyber_fb_name);
	fix->smem_start = (char *)CyberMem;
	fix->smem_len = CyberSize;

	fix->type = FB_TYPE_PACKED_PIXELS;
	if (CV3D)
		fix->accel = FB_ACCEL_S3VIRGE;
	else
		fix->accel = FB_ACCEL_S3TRIO64;
			
	fix->type_aux = 0;
	if (par->bpp == 8)
		fix->visual = FB_VISUAL_PSEUDOCOLOR;
	else
		fix->visual = FB_VISUAL_DIRECTCOLOR;

	fix->xpanstep = 0;
	fix->ypanstep = 0;
	fix->ywrapstep = 0;
	fix->line_length = 0;

	/* Is this correct for the CV64/3D too? What about CyberVGARegs?? */
	fix->mmio_start = (unsigned char *)CyberRegs;
	fix->mmio_len = 0x10000;

	return 0;
}


/*
 *    Get the video params out of `var'. If a value doesn't fit, round
 *    it up, if it's too big, return -EINVAL.
 */

static int Cyber_decode_var(struct fb_var_screeninfo *var,
			    struct Cyber_fb_par *par)
{
	par->xres = var->xres;
	par->yres = var->yres;
	par->bpp = var->bits_per_pixel;
	par->pixclock = var->pixclock;

	return 0;
}


/*
 *    Fill the `var' structure based on the values in `par' and maybe
 *    other values read out of the hardware.
 */

static int Cyber_encode_var(struct fb_var_screeninfo *var,
			    struct Cyber_fb_par *par)
{
	int i;

	var->xres = par->xres;
	var->yres = par->yres;
	var->xres_virtual = par->xres;
	var->yres_virtual = par->yres;
	var->xoffset = 0;
	var->yoffset = 0;

	var->bits_per_pixel = par->bpp;
	var->grayscale = 0;

	if (par->bpp == 8) {
		var->red.offset = 0;
		var->red.length = 8;
		var->red.msb_right = 0;
		var->blue = var->green = var->red;
	} else {
		var->red.offset = 11;
		var->red.length = 5;
		var->red.msb_right = 0;
		var->green.offset = 5;
		var->green.length = 6;
		var->green.msb_right = 0;
		var->blue.offset = 0;
		var->blue.length = 5;
		var->blue.msb_right = 0;
	}
	var->transp.offset = 0;
	var->transp.length = 0;
	var->transp.msb_right = 0;

	var->nonstd = 0;
	var->activate = 0;

	var->height = -1;
	var->width = -1;

	if (CV3D)
		var->accel = FB_ACCEL_S3VIRGE;
	else
		var->accel = FB_ACCEL_S3TRIO64;

	var->vmode = FB_VMODE_NONINTERLACED;

	/* Dummy values */

	var->pixclock = par->pixclock;
	var->sync = 0;
	var->left_margin = 64;
	var->right_margin = 96;
	var->upper_margin = 35;
	var->lower_margin = 12;
	var->hsync_len = 112;
	var->vsync_len = 2;

	for (i = 0; i < arraysize(var->reserved); i++)
		var->reserved[i] = 0;

	return 0;
}


/*
 *    Set a single color register. The values supplied are already
 *    rounded down to the hardware's capabilities (according to the
 *    entries in the var structure). Return != 0 for invalid regno.
 */

static int Cyber_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			   u_int transp)
{
	if (regno > 255)
		return -EINVAL;

	if (CV3D){
		vgawb_3d(0x3c8, (unsigned char) regno);
		Cyber_colour_table [regno][0] = red & 0xff;
		Cyber_colour_table [regno][1] = green & 0xff;
		Cyber_colour_table [regno][2] = blue & 0xff;
		Cyber_colour_table [regno][3] = transp;

		vgawb_3d(0x3c9, ((red & 0xff) >> 2));
		vgawb_3d(0x3c9, ((green & 0xff) >> 2));
		vgawb_3d(0x3c9, ((blue & 0xff) >> 2));
	}
	else{
		wb_64(0x3c8, (unsigned char) regno);
		Cyber_colour_table [regno][0] = red & 0xff;
		Cyber_colour_table [regno][1] = green & 0xff;
		Cyber_colour_table [regno][2] = blue & 0xff;
		Cyber_colour_table [regno][3] = transp;

		wb_64(0x3c9, (red & 0xff) >> 2);
		wb_64(0x3c9, (green & 0xff) >> 2);
		wb_64(0x3c9, (blue & 0xff) >> 2);
	}

	return 0;
}


/*
 *    Read a single color register and split it into
 *    colors/transparent. Return != 0 for invalid regno.
 */

static int Cyber_getcolreg(u_int regno, u_int *red, u_int *green, u_int *blue,
			   u_int *transp)
{
	if (regno > 255)
		return -EINVAL;

	*red    = Cyber_colour_table[regno][0];
	*green  = Cyber_colour_table[regno][1];
	*blue   = Cyber_colour_table[regno][2];
	*transp = Cyber_colour_table[regno][3];
	return 0;
}


/*
 *    (Un)Blank the screen
 */

void Cyber_blank(int blank)
{
	short i;

	if (CV3D){
		if (blank)
			for (i = 0; i < 256; i++){
				vgawb_3d(0x3c8, (unsigned char) i);
				vgawb_3d(0x3c9, 0);
				vgawb_3d(0x3c9, 0);
				vgawb_3d(0x3c9, 0);
			}
		else
			for (i = 0; i < 256; i++){
				vgawb_3d(0x3c8, (unsigned char) i);
				vgawb_3d(0x3c9, Cyber_colour_table[i][0] >> 2);
				vgawb_3d(0x3c9, Cyber_colour_table[i][1] >> 2);
				vgawb_3d(0x3c9, Cyber_colour_table[i][2] >> 2);
			}
	}else{
		if (blank)
			for (i = 0; i < 256; i++){
				wb_64(0x3c8, (unsigned char) i);
				wb_64(0x3c9, 0);
				wb_64(0x3c9, 0);
				wb_64(0x3c9, 0);
			}
		else
			for (i = 0; i < 256; i++){
				wb_64(0x3c8, (unsigned char) i);
				wb_64(0x3c9, Cyber_colour_table[i][0] >> 2);
				wb_64(0x3c9, Cyber_colour_table[i][1] >> 2);
				wb_64(0x3c9, Cyber_colour_table[i][2] >> 2);
			}
	}
}

/*
 * CV3D low-level support
 */
#ifdef CONFIG_FB_CV3D

#define	Cyber3D_WaitQueue(v)	 { do {while (((rl_3d(0x8504) & 0x1f00) < (((v)+2) << 8)); } while (0); }

static inline void Cyber3D_WaitBusy(void){
	unsigned long status;

	do{
		status = rl_3d(0x8504);
	}while (!(status & (1 << 13)));
}

#define S3V_BITBLT	(0x0 << 27)
#define S3V_RECTFILL	(0x2 << 27)
#define S3V_AUTOEXE	0x01
#define S3V_HWCLIP	0x02
#define S3V_DRAW	0x20
#define S3V_DST_8BPP	0x00
#define S3V_DST_16BPP	0x04
#define S3V_DST_24BPP	0x08
#define S3V_MONO_PAT	0x100

#define S3V_BLT_COPY	(0xcc<<17)
#define S3V_BLT_CLEAR	(0x00<<17)
#define S3V_BLT_SET	(0xff<<17)

/*
 * BitBLT - Through the Plane
 */
void Cyber3d_BitBLT (u_short curx, u_short cury, u_short destx, u_short desty,
		     u_short width, u_short height)
{
	unsigned int blitcmd = S3V_BITBLT | S3V_DRAW | S3V_DST_8BPP;

	blitcmd |= S3V_BLT_COPY;

	/* Set drawing direction */
	/* -Y, X maj, -X (default) */
	if (curx > destx)
		blitcmd |= (1 << 25);  /* Drawing direction +X */
	else{
		curx  += (width - 1);
		destx += (width - 1);
	}

	if (cury > desty)
		blitcmd |= (1 << 26);  /* Drawing direction +Y */
	else{
		cury  += (height - 1);
		desty += (height - 1);
	}

	wl_3d(0xa4f4, 1); /* pattern fb color */

	wl_3d(0xa4e8, ~0); /* mono pat 0 */
	wl_3d(0xa4ec, ~0); /* mono pat 1 */

	wl_3d(0xa504, ((width << 16) | height));	/* rwidth_height */
	wl_3d(0xa508, ((curx << 16)  | cury));		/* rsrc_xy */
	wl_3d(0xa50c, ((destx << 16) | desty));		/* rdest_xy */

	wl_3d(0xa500, blitcmd);				/* GO! */

	Cyber3D_WaitBusy();
}

/*
 * Rectangle Fill Solid
 */
void Cyber3d_RectFill (u_short x, u_short y, u_short width, u_short height,
		       u_short color)
{
	unsigned int tmp;
	unsigned int blitcmd = S3V_RECTFILL | S3V_DRAW | S3V_DST_8BPP |
		S3V_BLT_CLEAR | S3V_MONO_PAT | (1 << 26) | (1 << 25);

	tmp = color & 0xff;
	wl_3d(0xa4f4, tmp);

	wl_3d(0xa504, ((width << 16) | height));	/* rwidth_height */
	wl_3d(0xa50c, ((x << 16) | y));			/* rdest_xy */

	wl_3d(0xa500, blitcmd);				/* GO! */
	Cyber3D_WaitBusy();
}
#endif

#ifdef CONFIG_FB_CYBER
/**************************************************************
 * We are waiting for "fifo" FIFO-slots empty
 */
void Cyber_WaitQueue (u_short fifo)
{
	unsigned short status;

	do{
		status = *((u_short volatile *)(CyberRegs + S3_GP_STAT));
	}
	while (status & fifo);
}

/**************************************************************
 * We are waiting for Hardware (Graphics Engine) not busy
 */
void Cyber_WaitBlit (void)
{
	unsigned short status;

	do{
		status = *((u_short volatile *)(CyberRegs + S3_GP_STAT));
	}
	while (status & S3_HDW_BUSY);
}

/**************************************************************
 * BitBLT - Through the Plane
 */
void Cyber_BitBLT (u_short curx, u_short cury, u_short destx, u_short desty,
                   u_short width, u_short height, u_short mode)
{
	u_short blitcmd = S3_BITBLT;

	/* Set drawing direction */
	/* -Y, X maj, -X (default) */
	if (curx > destx)
		blitcmd |= 0x0020;  /* Drawing direction +X */
	else{
		curx  += (width - 1);
		destx += (width - 1);
	}

	if (cury > desty)
		blitcmd |= 0x0080;  /* Drawing direction +Y */
	else{
		cury  += (height - 1);
		desty += (height - 1);
	}

	Cyber_WaitQueue (0x8000);

	*((u_short volatile *)(CyberRegs + S3_PIXEL_CNTL)) = 0xa000;
	*((u_short volatile *)(CyberRegs + S3_FRGD_MIX)) = (0x0060 | mode);

	*((u_short volatile *)(CyberRegs + S3_CUR_X)) = curx;
	*((u_short volatile *)(CyberRegs + S3_CUR_Y)) = cury;

	*((u_short volatile *)(CyberRegs + S3_DESTX_DIASTP)) = destx;
	*((u_short volatile *)(CyberRegs + S3_DESTY_AXSTP)) = desty;

	*((u_short volatile *)(CyberRegs + S3_MIN_AXIS_PCNT)) = height - 1;
	*((u_short volatile *)(CyberRegs + S3_MAJ_AXIS_PCNT)) = width  - 1;

	*((u_short volatile *)(CyberRegs + S3_CMD)) = blitcmd;
}

/**************************************************************
 * Rectangle Fill Solid
 */
void Cyber_RectFill (u_short x, u_short y, u_short width, u_short height,
                     u_short mode, u_short color)
{
	u_short blitcmd = S3_FILLEDRECT;

	Cyber_WaitQueue (0x8000);

	*((u_short volatile *)(CyberRegs + S3_PIXEL_CNTL)) = 0xa000;
	*((u_short volatile *)(CyberRegs + S3_FRGD_MIX)) = (0x0020 | mode);

	*((u_short volatile *)(CyberRegs + S3_MULT_MISC)) = 0xe000;
	*((u_short volatile *)(CyberRegs + S3_FRGD_COLOR)) = color;

	*((u_short volatile *)(CyberRegs + S3_CUR_X)) = x;
	*((u_short volatile *)(CyberRegs + S3_CUR_Y)) = y;

	*((u_short volatile *)(CyberRegs + S3_MIN_AXIS_PCNT)) = height - 1;
	*((u_short volatile *)(CyberRegs + S3_MAJ_AXIS_PCNT)) = width  - 1;

	*((u_short volatile *)(CyberRegs + S3_CMD)) = blitcmd;
}
#endif


/**************************************************************
 * Move cursor to x, y
 */
void Cyber_MoveCursor (u_short x, u_short y)
{

	if (CV3D){
		printk("Yuck .... MoveCursor on a 3D\n");
		return;
	}

	*(CyberRegs + S3_CRTC_ADR)  = 0x39;
	*(CyberRegs + S3_CRTC_DATA) = 0xa0;

	*(CyberRegs + S3_CRTC_ADR)  = S3_HWGC_ORGX_H;
	*(CyberRegs + S3_CRTC_DATA) = (char)((x & 0x0700) >> 8);
	*(CyberRegs + S3_CRTC_ADR)  = S3_HWGC_ORGX_L;
	*(CyberRegs + S3_CRTC_DATA) = (char)(x & 0x00ff);

	*(CyberRegs + S3_CRTC_ADR)  = S3_HWGC_ORGY_H;
	*(CyberRegs + S3_CRTC_DATA) = (char)((y & 0x0700) >> 8);
	*(CyberRegs + S3_CRTC_ADR)  = S3_HWGC_ORGY_L;
	*(CyberRegs + S3_CRTC_DATA) = (char)(y & 0x00ff);
}


/* -------------------- Interfaces to hardware functions ------------------- */


static struct fb_hwswitch Cyber_switch = {
	Cyber_init, Cyber_encode_fix, Cyber_decode_var, Cyber_encode_var,
	Cyber_getcolreg, Cyber_setcolreg, Cyber_blank
};


/* -------------------- Generic routines ----------------------------------- */


/*
 *    Fill the hardware's `par' structure.
 */

static void Cyber_fb_get_par(struct Cyber_fb_par *par)
{
	if (current_par_valid)
		*par = current_par;
	else
		fbhw->decode_var(&Cyber_fb_predefined[Cyberfb_mode], par);
}


static void Cyber_fb_set_par(struct Cyber_fb_par *par)
{
	current_par = *par;
	current_par_valid = 1;
}


static void cyber_set_video(struct fb_var_screeninfo *var)
{
	if (CV3D){
		unsigned int clip;
		/* Set clipping rectangle to current screen size */

		clip = ((0 << 16) | (var->xres - 1));
		wl_3d(0xa4dc, clip);
		clip = ((0 << 16) | (var->yres - 1));
		wl_3d(0xa4e0, clip);
	}else{
		/* Set clipping rectangle to current screen size */
		ww_64(0xbee8, 0x1000);
		ww_64(0xbee8, 0x2000);
		ww_64(0xbee8, (0x3000 | (var->yres - 1)));
		ww_64(0xbee8, (0x4000 | (var->xres - 1)));
	}
}

static int do_fb_set_var(struct fb_var_screeninfo *var, int isactive)
{
	int err, activate;
	struct Cyber_fb_par par;

	if ((err = fbhw->decode_var(var, &par)))
		return(err);
	activate = var->activate;
	if ((var->activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_NOW && isactive)
		Cyber_fb_set_par(&par);
	fbhw->encode_var(var, &par);
	var->activate = activate;

	cyber_set_video(var);
	return 0;
}


/*
 *    Default Colormaps
 */

static u_short red16[] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa,
    0x5555, 0x5555, 0x5555, 0x5555, 0xffff, 0xffff, 0xffff, 0xffff
};
static u_short green16[] = {
    0x0000, 0x0000, 0xaaaa, 0xaaaa, 0x0000, 0x0000, 0xaaaa, 0xaaaa,
    0x5555, 0x5555, 0xffff, 0xffff, 0x5555, 0x5555, 0xffff, 0xffff
};
static u_short blue16[] = {
    0x0000, 0xaaaa, 0x0000, 0xaaaa, 0x0000, 0xaaaa, 0x0000, 0xaaaa,
    0x5555, 0xffff, 0x5555, 0xffff, 0x5555, 0xffff, 0x5555, 0xffff
};

static struct fb_cmap default_16_colors =
{ 0, 16, red16, green16, blue16, NULL };


static struct fb_cmap *get_default_colormap(int bpp)
{
	return(&default_16_colors);
}


#define CNVT_TOHW(val,width)     ((((val)<<(width))+0x7fff-(val))>>16)
#define CNVT_FROMHW(val,width)   (((width) ? ((((val)<<16)-(val)) / \
                                              ((1<<(width))-1)) : 0))

static int do_fb_get_cmap(struct fb_cmap *cmap,
			  struct fb_var_screeninfo *var, int kspc)
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
			return 0;

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
	return 0;
}


static int do_fb_set_cmap(struct fb_cmap *cmap,
			  struct fb_var_screeninfo *var, int kspc)
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
		return -EINVAL;

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
			return 0;
	}
	return 0;
}


static void do_install_cmap(int con)
{
	if (con != currcon)
		return;
	if (disp[con].cmap.len)
		do_fb_set_cmap(&disp[con].cmap, &disp[con].var, 1);
	else
		do_fb_set_cmap(get_default_colormap(disp[con].var.bits_per_pixel),
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
		memcpy_fs(fsfromto, to->transp+tooff,
			  from->transp+fromoff, size);
}


static int alloc_cmap(struct fb_cmap *cmap, int len, int transp)
{
	int size = len*sizeof(u_short);

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
			return -1;
		if (!(cmap->green = kmalloc(size, GFP_ATOMIC)))
			return -1;
		if (!(cmap->blue = kmalloc(size, GFP_ATOMIC)))
			return -1;
		if (transp) {
			if (!(cmap->transp = kmalloc(size, GFP_ATOMIC)))
				return(-1);
		} else
			cmap->transp = NULL;
	}
	cmap->start = 0;
	cmap->len = len;
	copy_cmap(get_default_colormap(len), cmap, 0);
	return 0;
}


/*
 *    Get the Fixed Part of the Display
 */

static int Cyber_fb_get_fix(struct fb_fix_screeninfo *fix, int con, int fbidx)
{
	struct Cyber_fb_par par;
	int error = 0;

	if (con == -1)
		Cyber_fb_get_par(&par);
	else
		error = fbhw->decode_var(&disp[con].var, &par);
	return(error ? error : fbhw->encode_fix(fix, &par));
}


/*
 *    Get the User Defined Part of the Display
 */

static int Cyber_fb_get_var(struct fb_var_screeninfo *var, int con, int fbidx)
{
	struct Cyber_fb_par par;
	int error = 0;

	if (con == -1) {
		Cyber_fb_get_par(&par);
		error = fbhw->encode_var(var, &par);
	} else
		*var = disp[con].var;
	return(error);
}


static void Cyber_fb_set_disp(int con)
{
	struct fb_fix_screeninfo fix;

	/* ### FN: Needs fixes later */
	Cyber_fb_get_fix(&fix, con, 0);
	if (con == -1)
		con = 0;
	disp[con].screen_base = fix.smem_start;
	disp[con].visual = fix.visual;
	disp[con].type = fix.type;
	disp[con].type_aux = fix.type_aux;
	disp[con].ypanstep = fix.ypanstep;
	disp[con].ywrapstep = fix.ywrapstep;
	disp[con].can_soft_blank = 1;
	disp[con].inverse = Cyberfb_inverse;
}


/*
 *    Set the User Defined Part of the Display
 */

static int Cyber_fb_set_var(struct fb_var_screeninfo *var, int con, int fbidx)
{
	int err, oldxres, oldyres, oldvxres, oldvyres, oldbpp;

	if ((err = do_fb_set_var(var, con == currcon)))
		return err;
	if ((var->activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_NOW) {
		oldxres = disp[con].var.xres;
		oldyres = disp[con].var.yres;
		oldvxres = disp[con].var.xres_virtual;
		oldvyres = disp[con].var.yres_virtual;
		oldbpp = disp[con].var.bits_per_pixel;
		disp[con].var = *var;
		if (oldxres != var->xres || oldyres != var->yres ||
		    oldvxres != var->xres_virtual ||
		    oldvyres != var->yres_virtual ||
		    oldbpp != var->bits_per_pixel) {
			Cyber_fb_set_disp(con);
			(*fb_info.changevar)(con);
			alloc_cmap(&disp[con].cmap, 0, 0);
			do_install_cmap(con);
		}
	}
	var->activate = 0;
	return 0;
}


/*
 *    Get the Colormap
 */

static int Cyber_fb_get_cmap(struct fb_cmap *cmap, int kspc,
			     int con, int fbidx)
{
	if (con == currcon) /* current console? */
		return(do_fb_get_cmap(cmap, &disp[con].var, kspc));
	else if (disp[con].cmap.len) /* non default colormap? */
		copy_cmap(&disp[con].cmap, cmap, kspc ? 0 : 2);
	else
		copy_cmap(get_default_colormap(disp[con].var.bits_per_pixel),
			  cmap, kspc ? 0 : 2);
	return 0;
}


/*
 *    Set the Colormap
 */

static int Cyber_fb_set_cmap(struct fb_cmap *cmap, int kspc, int con, int fbidx)
{
	int err;

	if (!disp[con].cmap.len) {       /* no colormap allocated? */
		if ((err = alloc_cmap(&disp[con].cmap,
				      1<<disp[con].var.bits_per_pixel, 0)))
			return err;
	}
	if (con == currcon)              /* current console? */
		return(do_fb_set_cmap(cmap, &disp[con].var, kspc));
	else
		copy_cmap(cmap, &disp[con].cmap, kspc ? 0 : 1);
	return 0;
}


/*
 *    Pan or Wrap the Display
 *
 *    This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
 */

static int Cyber_fb_pan_display(struct fb_var_screeninfo *var,
				int con, int fbidx)
{
	return -EINVAL;
}


/*
 *    Cybervision Frame Buffer Specific ioctls
 */

static int Cyber_fb_ioctl(struct inode *inode, struct file *file,
                          u_int cmd, u_long arg, int con, int fbidx)
{
	return -EINVAL;
}

/*
 * Monitor limit settings - these are currently dummy functions
 */

static int Cyber_fb_get_monitorspec(struct fb_monitorspec *spec,
				    int val, int fbidx)
{
	spec->hfmin = -1;
	spec->hfmax = -1;
	spec->vfmin = -1;
	spec->vfmax = -1;
	return 0;
}

static int Cyber_fb_put_monitorspec(struct fb_monitorspec *spec,
				    int val, int fbidx)
{
	return 0;
}

static struct fb_ops Cyber_fb_ops = {
	Cyber_fb_get_fix, Cyber_fb_get_var, Cyber_fb_set_var,
	Cyber_fb_get_cmap, Cyber_fb_set_cmap, Cyber_fb_pan_display,
	Cyber_fb_ioctl, Cyber_fb_get_monitorspec, Cyber_fb_put_monitorspec
};


int Cyber_probe(void)
{
#ifdef CONFIG_FB_CYBER
	if ((CyberKey = zorro_find(MANUF_PHASE5, PROD_CYBERVISION, 0, 0)))
		CV3D = 0;
	else
#endif
#ifdef CONFIG_FB_CV3D
		if ((CyberKey = zorro_find(MANUF_PHASE5,
					   PROD_CYBERVISION3D, 0, 0)))
			CV3D = 1;
#endif
	return CyberKey;
}


void Cyber_video_setup(char *options, int *ints)
{
	char *this_opt;
	int i;

	fb_info.fontname[0] = '\0';

	if (!options || !*options)
		return;

	if (!strncmp(options, "cyberfb:", 8))
		options += 8;
	if (!strncmp(options, "virgefb:", 8))
		options += 8;

	for (this_opt = strtok(options, ","); this_opt; this_opt = strtok(NULL, ","))
		if (!strcmp(this_opt, "inverse")) {
			Cyberfb_inverse = 1;
			for (i = 0; i < 16; i++) {
				red16[i] = ~red16[i];
				green16[i] = ~green16[i];
				blue16[i] = ~blue16[i];
			}
		} else if (!strncmp(this_opt, "font:", 5))
			strcpy(fb_info.fontname, this_opt+5);
		else if (!strcmp (this_opt, "cyber8")){
			Cyberfb_mode = 1;
			Cyber_fb_predefined[0] = Cyber_fb_predefined[1];
		}
		else if (!strcmp (this_opt, "cyber16")){
			Cyber_fb_predefined[0] = Cyber_fb_predefined[7];
			Cyberfb_mode = 7;
		}
		else
			Cyberfb_mode = get_video_mode(this_opt);
}


/*
 *    Initialization
 */

struct fb_info *Cyber_fb_init(unsigned long *mem_start)
{
	int err;
	struct Cyber_fb_par par;

	memstart = mem_start;

	fbhw = &Cyber_switch;

	fbhw->init();

	strcpy(fb_info.modename, Cyber_fb_name);
	fb_info.disp = disp;
	fb_info.switch_con = &Cyberfb_switch;
	fb_info.updatevar = &Cyberfb_updatevar;
	fb_info.blank = &Cyberfb_blank;

	err = register_framebuffer(Cyber_fb_name, &node, &Cyber_fb_ops,
				   NUM_TOTAL_MODES, Cyber_fb_predefined);
	if (err < 0)
		panic("Cannot register frame buffer\n");

	if (Cyberfb_mode == -1)
		Cyberfb_mode = 1;

	fbhw->decode_var(&Cyber_fb_predefined[Cyberfb_mode], &par);
	fbhw->encode_var(&Cyber_fb_predefined[0], &par);

	do_fb_set_var(&Cyber_fb_predefined[0], 0);
	Cyber_fb_get_var(&disp[0].var, -1, GET_FB_IDX(node));
	Cyber_fb_set_disp(-1);
	do_install_cmap(0);

	return &fb_info;
}


static int Cyberfb_switch(int con)
{
	/* Do we have to save the colormap? */
	if (disp[currcon].cmap.len)
		do_fb_get_cmap(&disp[currcon].cmap, &disp[currcon].var, 1);

	do_fb_set_var(&disp[con].var, 1);
	currcon = con;
	/* Install new colormap */
	do_install_cmap(con);

	return 0;
}


/*
 *    Update the `var' structure (called by fbcon.c)
 *
 *    This call looks only at yoffset and the FB_VMODE_YWRAP flag in `var'.
 *    Since it's called by a kernel driver, no range checking is done.
 */

static int Cyberfb_updatevar(int con)
{
	return 0;
}


/*
 *    Blank the display.
 */

static void Cyberfb_blank(int blank)
{
	fbhw->blank(blank);
}


/*
 *    Get a Video Mode
 */

static int get_video_mode(const char *name)
{
	int i;

	for (i = 1; i <= NUM_PREDEF_MODES; i++)
		if (!strcmp(name, Cyber_fb_modenames[i])){
			Cyber_fb_predefined[0] = Cyber_fb_predefined[i];
			return i;
		}
	return -1;
}
