/*
 * besta/unzip.c -- Implementation of compressed kernel loading.
 *
 * Written 1996, 1997	    Dmitry K. Butskoy
 *			    <buc@citadel.stu.neva.ru>
 *
 * This code adapted from `drivers/block/rd.c'.
 * `lib/inflate.c' is included.
 *
 * The same Copyright conditions as these files following.
 *
 */

#define NULL    (0)
extern void *memset (void *, char, unsigned int);
extern void *memcpy (void *, const void *, unsigned int);

#define OF(args)  args

#define memzero(s, n)     memset ((s), 0, (n))

typedef unsigned char  uch;
typedef unsigned short ush;
typedef unsigned long  ulg;

#define INBUFSIZ 4096
#define WSIZE 0x8000    /* window size--must be a power of two, and */
			/*  at least 32K for zip's deflate method */

static uch *window;
static uch *inbuf, *outbuf;
static unsigned inlen, outlen;

static unsigned inptr = 0;   /* index of next byte to be processed in inbuf */
static unsigned outcnt = 0;  /* bytes in output buffer */
static exit_code = 0;
static long bytes_out = 0;

#define get_byte()  (inptr < inlen ? inbuf[inptr++] : (-1))

/* Diagnostic functions (stubbed out) */
#define Assert(cond,msg)
#define Trace(x)
#define Tracev(x)
#define Tracevv(x)
#define Tracec(c,x)
#define Tracecv(c,x)

#define STATIC static

static void flush_window(void);
static void *malloc (int size);
static void free (void *where);
static inline void error (char *m) {  exit_code = 1;  }
static inline void gzip_mark (void **ptr) { }
static inline void gzip_release (void **ptr) { }

#include <../lib/inflate.c>

static void *heap_start;
static int heap_len;
static void *curr_heap;
static void *heap_end;

static void *malloc (int size) {
	unsigned long *lp;
	void *tmp;

	size = (size + 3) & ~3;

	if (curr_heap + sizeof (*lp) + size <= heap_end) {
	    lp = curr_heap;
	    *lp = size;

	    curr_heap += sizeof (*lp);
	    tmp = curr_heap;

	    curr_heap += size;
	    return  tmp;
	}

	for (tmp = heap_start; tmp < heap_end; ) {

	    lp = tmp;

	    if (!(*lp & 0x01) ||
		(*lp & ~0x01) < size
	    )  tmp += sizeof (*lp) + (*lp & ~0x01);
	    else {
		unsigned int left;

		*lp &= ~0x01;
		left = size - *lp;

		*lp = size;
		tmp += sizeof (*lp);    /*  value to return   */

		if (left > 0) {
		    lp = (tmp + size);
		    left -= sizeof (*lp);
		    *lp = left | 0x01;
		}

		return tmp;
	    }
	}

	return 0;   /*  is it good ?  */
}

static void free (void *where) {
	unsigned long *lp = where;

	lp--;

	*lp |= 0x01;    /*  mark it free   */
}


/* ===========================================================================
 * Write the output window window[0..outcnt-1] and update crc and bytes_out.
 * (Used for the decompressed data only.)
 */
static void flush_window (void) {
    ulg c = crc;         /* temporary variable */
    unsigned n;
    uch *in, ch;

    memcpy (outbuf, window, outcnt);
    outbuf += outcnt;

    in = window;
    for (n = 0; n < outcnt; n++) {
	    ch = *in++;
	    c = crc_32_tab[((int)c ^ ch) & 0xff] ^ (c >> 8);
    }
    crc = c;
    bytes_out += (ulg)outcnt;
    outcnt = 0;
}


void *unzip (void *from, int fromlen, void *to, int tolen,
					void *heap, int heaplen) {

	heap_start = heap;
	heap_len = heaplen;
	heap_end = heap_start + heaplen;
	curr_heap = heap_start;

	inbuf = from;
	inlen = fromlen;
	outbuf = to;
	outlen = tolen;

	/*  because unzip() may be called more than once   */
	inptr = 0;
	outcnt = 0;
	exit_code = 0;
	bytes_out = 0;

	window = malloc (WSIZE);

	makecrc();

	gunzip();

	free (window);

	return outbuf;      /*  to determine size of uncompressed data   */
}


