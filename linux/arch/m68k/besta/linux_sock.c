/*
 * linux_sock.c -- To emulate sysv-kludge-style socket calls throu Linux
 *		   native calls, in the same library.
 *		   It is mostly specific and not relative to kernel sources.
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

#include <errno.h>

/*  Be very careful...   */
#include <linux/socket.h>

/*  Comes from Linux:/usr/include/sys/socketcall.h,
   similar to Linux:/usr/src/linux/include/linux/net.h  */

#define SYS_SOCKET      1
#define SYS_BIND        2
#define SYS_CONNECT     3
#define SYS_LISTEN      4
#define SYS_ACCEPT      5
#define SYS_GETSOCKNAME 6
#define SYS_GETPEERNAME 7
#define SYS_SOCKETPAIR  8
#define SYS_SEND        9
#define SYS_RECV        10
#define SYS_SENDTO      11
#define SYS_RECVFROM    12
#define SYS_SHUTDOWN    13
#define SYS_SETSOCKOPT  14
#define SYS_GETSOCKOPT  15

/*  hardcoded in entry.S   */
#define __SVR_select     101
#define __SVR_socketcall 102

extern int syscall (int, ...);

static inline
int socketcall (int call, unsigned long *args) {

	return syscall (__SVR_socketcall, call, args);
}


int
_linux_accept(int sockfd, struct sockaddr *peer, int *paddrlen)
{
	unsigned long args[3];

	args[0] = sockfd;
	args[1] = (unsigned long)peer;
	args[2] = (unsigned long)paddrlen;
	return socketcall(SYS_ACCEPT, args);
}

int
_linux_bind(int sockfd, struct sockaddr *myaddr, int addrlen)
{
	unsigned long args[3];

	args[0] = sockfd;
	args[1] = (unsigned long)myaddr;
	args[2] = addrlen;
	return socketcall(SYS_BIND, args);
}

int
_linux_connect(int sockfd, struct sockaddr *saddr, int addrlen)
{
	unsigned long args[3];

	args[0] = sockfd;
	args[1] = (unsigned long)saddr;
	args[2] = addrlen;
	return socketcall(SYS_CONNECT, args);
}

int
_linux_getpeername(int sockfd, struct sockaddr *addr, int *paddrlen)
{
	unsigned long args[3];

	args[0] = sockfd;
	args[1] = (unsigned long)addr;
	args[2] = (unsigned long)paddrlen;
	return socketcall(SYS_GETPEERNAME, args);
}

int
_linux_getsockname(int sockfd, struct sockaddr *addr, int *paddrlen)
{
	unsigned long args[3];

	args[0] = sockfd;
	args[1] = (unsigned long)addr;
	args[2] = (unsigned long)paddrlen;
	return socketcall(SYS_GETSOCKNAME, args);
}

int
_linux_listen(int sockfd, int backlog)
{
	unsigned long args[2];

	args[0] = sockfd;
	args[1] = backlog;
	return socketcall(SYS_LISTEN, args);
}

/* recv, recvfrom added by bir7@leland.stanford.edu */

int
_linux_recv (int sockfd, void *buffer, int len, unsigned flags)
{
  unsigned long args[4];
  args[0] = sockfd;
  args[1] = (unsigned long) buffer;
  args[2] = len;
  args[3] = flags;
  return (socketcall (SYS_RECV, args));
}

/* recv, recvfrom added by bir7@leland.stanford.edu */

int
_linux_recvfrom (int sockfd, void *buffer, int len, unsigned flags,
	struct sockaddr *to, int *tolen)
{
  unsigned long args[6];
  args[0] = sockfd;
  args[1] = (unsigned long) buffer;
  args[2] = len;
  args[3] = flags;
  args[4] = (unsigned long) to;
  args[5] = (unsigned long) tolen;
  return (socketcall (SYS_RECVFROM, args));
}
/* send, sendto added by bir7@leland.stanford.edu */

int
_linux_send (int sockfd, const void *buffer, int len, unsigned flags)
{
  unsigned long args[4];
  args[0] = sockfd;
  args[1] = (unsigned long) buffer;
  args[2] = len;
  args[3] = flags;
  return (socketcall (SYS_SEND, args));
}
/* send, sendto added by bir7@leland.stanford.edu */

int
_linux_sendto (int sockfd, const void *buffer, int len, unsigned flags,
	const struct sockaddr *to, int tolen)
{
  unsigned long args[6];
  args[0] = sockfd;
  args[1] = (unsigned long) buffer;
  args[2] = len;
  args[3] = flags;
  args[4] = (unsigned long) to;
  args[5] = tolen;
  return (socketcall (SYS_SENDTO, args));
}

/* shutdown by bir7@leland.stanford.edu */
int
_linux_shutdown (int sockfd, int how)
{
  unsigned long args[2];
  args[0] = sockfd;
  args[1] = how;
  return (socketcall (SYS_SHUTDOWN, args));
}

int
_linux_socket(int family, int type, int protocol)
{
	unsigned long args[3];

	/*  svr3:/usr/include/sys/io/socket.h is compatible with Linux,
	 but  svr3:/usr/include/sys/socket.h  is not.
	      I hope, the last is useful.
	*/
	switch (type) {
	    case 2:  type = SOCK_STREAM;  break;
	    case 1:  type = SOCK_DGRAM;  break;
	    case 4:  type = SOCK_RAW;  break;
	    case 5:  type = SOCK_RDM;  break;
	    case 6:  type = SOCK_SEQPACKET;  break;
	}

	args[0] = family;
	args[1] = type;
	args[2] = protocol;
	return socketcall(SYS_SOCKET, args);
}

int
_linux_socketpair(int family, int type, int protocol, int sockvec[2])
{
	unsigned long args[4];

	/*  svr3:/usr/include/sys/io/socket.h is compatible with Linux,
	 but  svr3:/usr/include/sys/socket.h  is not.
	      I hope, the last is useful.
	*/
	switch (type) {
	    case 2:  type = SOCK_STREAM;  break;
	    case 1:  type = SOCK_DGRAM;  break;
	    case 4:  type = SOCK_RAW;  break;
	    case 5:  type = SOCK_RDM;  break;
	    case 6:  type = SOCK_SEQPACKET;  break;
	}

	args[0] = family;
	args[1] = type;
	args[2] = protocol;
	args[3] = (unsigned long)sockvec;
	return socketcall(SYS_SOCKETPAIR, args);
}

int
_linux_getsockopt (int fd, int level, int optname, void *optval, int *optlen)
{
	unsigned long args[5];

	if (level == 0xffff)  level = 1;
	else {
	    errno = ENOPKG;
	    return -1;
	}

	switch (optname) {
	    case 0x0001: optname =  1; break;     /*  SO_DEBUG   */
	    case 0x0004: optname =  2; break;     /*  SO_REUSEADDR   */
	    case 0x0008: optname =  9; break;     /*  SO_KEEPALIVE   */
	    case 0x0010: optname =  5; break;     /*  SO_DONTROUTE   */
	    case 0x0020: optname =  6; break;     /*  SO_BROADCAST   */
	    case 0x0080: optname = 13; break;     /*  SO_LINGER   */
	    case 0x0100: optname = 10; break;     /*  SO_OOBINLINE   */
	    case 0x1001: optname =  7; break;     /*  SO_SNDBUF   */
	    case 0x1002: optname =  8; break;     /*  SO_RCVBUF   */
	    case 0x1007: optname =  4; break;     /*  SO_ERROR   */
	    case 0x1008: optname =  3; break;     /*  SO_TYPE   */
	    default: return -1;
	}


	args[0]=fd;
	args[1]=level;
	args[2]=optname;
	args[3]=(unsigned long)optval;
	args[4]=(unsigned long)optlen;
	return (socketcall (SYS_GETSOCKOPT, args));
}

/* [sg]etsockoptions by bir7@leland.stanford.edu */
int
_linux_setsockopt (int fd, int level, int optname, const void *optval,
	int optlen)
{
	unsigned long args[5];

	if (level == 0xffff)  level = 1;
	else {
	    errno = ENOPKG;
	    return -1;
	}

	switch (optname) {
	    case 0x0001: optname =  1; break;     /*  SO_DEBUG   */
	    case 0x0004: optname =  2; break;     /*  SO_REUSEADDR   */
	    case 0x0008: optname =  9; break;     /*  SO_KEEPALIVE   */
	    case 0x0010: optname =  5; break;     /*  SO_DONTROUTE   */
	    case 0x0020: optname =  6; break;     /*  SO_BROADCAST   */
	    case 0x0080: optname = 13; break;     /*  SO_LINGER   */
	    case 0x0100: optname = 10; break;     /*  SO_OOBINLINE   */
	    case 0x1001: optname =  7; break;     /*  SO_SNDBUF   */
	    case 0x1002: optname =  8; break;     /*  SO_RCVBUF   */
	    case 0x1007: optname =  4; break;     /*  SO_ERROR   */
	    case 0x1008: optname =  3; break;     /*  SO_TYPE   */
	    default: return -1;
	}

	args[0]=fd;
	args[1]=level;
	args[2]=optname;
	args[3]=(unsigned long)optval;
	args[4]=optlen;
	return (socketcall (SYS_SETSOCKOPT, args));
}

int _linux_select (int one, int two, int three, int four, int five) {

	return syscall (__SVR_select, &one);

}

