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
#include <string.h>
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

/* The platform layer's socket addresses (posix.h passes them as opaque
pointers) have Winsock's and Linux's layout: a 16-bit family first. BSD's
has a length byte and an 8-bit family there. The calls that take or give one
convert it (an IPv4 address or a Unix one; the rest pass as they are). */
static inline void posix_darwin_address_in(const void *address, socklen_t length, struct sockaddr_storage *out,
	socklen_t *out_length)
{
	const unsigned char *bytes = address;

	memset(out, 0, sizeof(*out));
	if (length > (socklen_t)sizeof(*out))
		length = (socklen_t)sizeof(*out);
	memcpy(out, address, length);
	if (length >= 2)
	{
		((struct sockaddr *)out)->sa_family = (sa_family_t)(bytes[0] | (bytes[1] << 8));
		((struct sockaddr *)out)->sa_len = (unsigned char)length;
	}
	*out_length = length;
}

static inline void posix_darwin_address_out(const struct sockaddr_storage *in, socklen_t length, void *address,
	socklen_t capacity)
{
	unsigned char bytes[sizeof(struct sockaddr_storage)];
	unsigned short family = ((const struct sockaddr *)in)->sa_family;

	memcpy(bytes, in, sizeof(bytes));
	bytes[0] = (unsigned char)(family & 0xff);
	bytes[1] = (unsigned char)(family >> 8);
	memcpy(address, bytes, length < capacity ? length : capacity);
}

static inline int posix_darwin_bind(int socket, const void *address, socklen_t length)
{
	struct sockaddr_storage converted;
	socklen_t converted_length;

	posix_darwin_address_in(address, length, &converted, &converted_length);
	return bind(socket, (struct sockaddr *)&converted, converted_length);
}

static inline int posix_darwin_connect(int socket, const void *address, socklen_t length)
{
	struct sockaddr_storage converted;
	socklen_t converted_length;

	posix_darwin_address_in(address, length, &converted, &converted_length);
	return connect(socket, (struct sockaddr *)&converted, converted_length);
}

static inline ssize_t posix_darwin_sendto(int socket, const void *buffer, size_t size, int flags,
	const void *address, socklen_t length)
{
	struct sockaddr_storage converted;
	socklen_t converted_length;

	if (!address)
		return sendto(socket, buffer, size, flags, NULL, 0);
	posix_darwin_address_in(address, length, &converted, &converted_length);
	return sendto(socket, buffer, size, flags, (struct sockaddr *)&converted, converted_length);
}

static inline int posix_darwin_name(int (*get)(int, struct sockaddr *, socklen_t *), int socket, void *address,
	socklen_t *length)
{
	struct sockaddr_storage converted;
	socklen_t converted_length = sizeof(converted);
	int result = get(socket, (struct sockaddr *)&converted, &converted_length);

	if (result == 0 && address && length)
	{
		posix_darwin_address_out(&converted, converted_length, address, *length);
		*length = converted_length;
	}
	return result;
}

static inline int posix_darwin_getsockname(int socket, void *address, socklen_t *length)
{
	return posix_darwin_name(getsockname, socket, address, length);
}

static inline int posix_darwin_getpeername(int socket, void *address, socklen_t *length)
{
	return posix_darwin_name(getpeername, socket, address, length);
}

/* recvmsg's msg_name, converted back */
static inline ssize_t posix_darwin_recvmsg(int socket, struct msghdr *message, int flags)
{
	struct sockaddr_storage converted;
	void *address = message->msg_name;
	socklen_t capacity = message->msg_namelen;
	ssize_t result;

	if (address)
	{
		message->msg_name = &converted;
		message->msg_namelen = sizeof(converted);
	}
	result = recvmsg(socket, message, flags);
	if (address)
	{
		if (result >= 0)
			posix_darwin_address_out(&converted, message->msg_namelen, address, capacity);
		message->msg_name = address;
	}
	return result;
}

static inline int posix_darwin_accept4(int listener, void *address, socklen_t *length, int flags)
{
	struct sockaddr_storage converted;
	socklen_t converted_length = sizeof(converted);
	int descriptor = accept4(listener, (struct sockaddr *)&converted, &converted_length, flags);

	if (descriptor >= 0 && address && length)
	{
		posix_darwin_address_out(&converted, converted_length, address, *length);
		*length = converted_length;
	}
	return descriptor;
}

/* (posix_net.c's own addresses, its probe of the route and the like, are
BSD's: only the calls with the platform layer's address are these) */
#define POSIX_DARWIN_PLATFORM_ADDRESSES 1

static inline ssize_t getrandom(void *buffer, size_t size, unsigned int flags)
{
	(void)flags;
	if (size > 256)
		size = 256;
	return getentropy(buffer, size) == 0 ? (ssize_t)size : -1;
}

#endif

#endif
