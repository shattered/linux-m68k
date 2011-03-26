#ifndef _M68K_PARAM_H
#define _M68K_PARAM_H

/*  There is no guarantee autoconf.h is included this moment (__besta__). */
#if 1	/*  This is true only for besta...  */
#undef HZ
#define HZ 60
#endif

#ifndef HZ
#define HZ 100
#endif

#define EXEC_PAGESIZE	4096

#ifndef NGROUPS
#define NGROUPS		32
#endif

#ifndef NOGROUP
#define NOGROUP		(-1)
#endif

#define MAXHOSTNAMELEN	64	/* max length of hostname */

#endif /* _M68K_PARAM_H */
