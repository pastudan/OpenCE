/*
POSIX_DARWIN.H

What posix_*.c use of Linux that macOS spells differently, for the macOS
build's host (port/macos/README.md), which compiles them natively: included
by them on __APPLE__ only, so the Linux, Windows and Android builds see
nothing of it.

- struct stat's times are st_mtimespec and the like;
- sockets have no SOCK_CLOEXEC or SOCK_NONBLOCK flags (set afterwards with
  fcntl), no accept4, and no MSG_NOSIGNAL: SO_NOSIGPIPE is set on each socket
  instead, and SIGPIPE is ignored anyway (sdl_platform.c);
- getentropy stands in for getrandom (256 bytes at most a call).
*/

#ifndef __HALO_POSIX_DARWIN_H
#define __HALO_POSIX_DARWIN_H

#ifdef __APPLE__

#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define st_mtim st_mtimespec
#define st_atim st_atimespec
#define st_ctim st_ctimespec

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x10000000
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x20000000
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* socket() with Linux's type flags */
static inline int posix_darwin_socket(int family, int type, int protocol)
{
	int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
	int descriptor = socket(family, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol);
	int one = 1;

	if (descriptor < 0)
		return descriptor;
	if (flags & SOCK_CLOEXEC)
		fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	if (flags & SOCK_NONBLOCK)
		fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK);
	setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	return descriptor;
}
#define socket(family, type, protocol) posix_darwin_socket(family, type, protocol)

static inline int accept4(int listener, struct sockaddr *address, socklen_t *length, int flags)
{
	int descriptor = accept(listener, address, length);
	int one = 1;

	if (descriptor < 0)
		return descriptor;
	if (flags & SOCK_CLOEXEC)
		fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	if (flags & SOCK_NONBLOCK)
		fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK);
	setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	return descriptor;
}

static inline ssize_t getrandom(void *buffer, size_t size, unsigned int flags)
{
	(void)flags;
	if (size > 256)
		size = 256;
	return getentropy(buffer, size) == 0 ? (ssize_t)size : -1;
}

#endif

#endif
