/*
 * sock_entries.c -- To emulate sysv-kludge-style socket calls by Linux
 *		     native calls, in the same library.
 *		     It is mostly specific and not relative to kernel sources.
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

#include <signal.h>
#include <errno.h>

static enum { UNKNOWN, LINUX, SVR3 } hosttype = UNKNOWN;

#define LIST2   int a, int b
#define LIST3   int a, int b, int c
#define LIST4   int a, int b, int c, int d
#define LIST5   int a, int b, int c, int d, int e
#define LIST6   int a, int b, int c, int d, int e, int f
#define ARGS2   a, b
#define ARGS3   a, b, c
#define ARGS4   a, b, c, d
#define ARGS5   a, b, c, d, e
#define ARGS6   a, b, c, d, e, f


#define BODY(NAME,N) \
int NAME (LIST ## N) {   \
	extern int _linux_ ## NAME (LIST ## N);     \
	extern int _svr3_lib_ ## NAME (LIST ## N);  \
				    \
	if (hosttype == UNKNOWN &&  \
	    set_host_type () < 0    \
	)  return -1;               \
				    \
	return  (hosttype == LINUX) ? _linux_ ## NAME (ARGS ## N)     \
				    : _svr3_lib_ ## NAME (ARGS ## N); \
}

static int set_host_type (void) {
	void (*handler) (int);
	extern void (*_signal (int, void (*) (int))) (int);

	handler = _signal (SIGSYS, SIG_DFL);

	if (((int) handler) == -1 && errno == 0) {
		/*  Linux says `SIGSYS is unimplemented' by setting `errno'
		   to `ENOSYS', which is unimplemented too, so, `errno == 0' ...
		*/

		hosttype = LINUX;
		return 0;
	}

	if (((int) handler) >= 0) {

	    _signal (SIGSYS, handler);       /*  go back accuracy...  */

	    hosttype = SVR3;
	    return 0;
	}

	return -1;
}


BODY (accept, 3)
BODY (bind, 3)
BODY (connect, 3)
BODY (getpeername, 3)
BODY (getsockname, 3)
BODY (listen, 2)
BODY (recv, 4)
BODY (recvfrom, 6)
BODY (send, 4)
BODY (sendto, 6)
BODY (shutdown, 2)
BODY (socket, 3)
BODY (socketpair, 4)
BODY (getsockopt, 5)
BODY (setsockopt, 5)
BODY (select, 5)

