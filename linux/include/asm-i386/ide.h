#ifndef _I386_IDE_H
#define _I386_IDE_H

typedef int ide_ioreg_t;

#define IDE_IO_PORT_IO

#ifndef MAX_HWIFS
#define MAX_HWIFS	4	/* an arbitrary, but realistic limit */
#endif

#define STI()   sti()

#endif /* _I386_IDE_H */
