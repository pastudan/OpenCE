/*
HOST_WASI.C

WASI preview 1, the calls wasi-libc makes, for the guest (the game's C
library's files, clocks, environment, sleeps and exit), on macOS's own.

The guest sees the whole file system through one preopened directory, "/"
(descriptor 3): wasi-libc resolves every path, relative ones from its own
current directory, to one under it, and path_open and the rest get it
relative to that root, which here is the real one. Descriptors are the
host's own (0 to 2 the standard streams), so they need no table.

Each call returns a WASI error number (__WASI_ERRNO_*), translated from the
host's errno. Structures are written into guest memory field by field, at
the offsets wasi/wasip1.h gives.
*/

#include "host.h"
#include "halo_w2c.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* the descriptor wasi-libc takes as the preopened "/" */
#define ROOT_DESCRIPTOR 3

#define WASI_ESUCCESS 0
#define WASI_EBADF 8
#define WASI_EINVAL 28
#define WASI_ENOTSUP 58

static uint16_t wasi_errno(int error)
{
	switch (error)
	{
	case 0: return 0;
	case E2BIG: return 1;
	case EACCES: return 2;
	case EADDRINUSE: return 3;
	case EADDRNOTAVAIL: return 4;
	case EAFNOSUPPORT: return 5;
	case EAGAIN: return 6;
	case EALREADY: return 7;
	case EBADF: return 8;
	case EBUSY: return 10;
	case ECANCELED: return 11;
	case ECHILD: return 12;
	case ECONNABORTED: return 13;
	case ECONNREFUSED: return 14;
	case ECONNRESET: return 15;
	case EDEADLK: return 16;
	case EDOM: return 18;
	case EEXIST: return 20;
	case EFAULT: return 21;
	case EFBIG: return 22;
	case EHOSTUNREACH: return 23;
	case EILSEQ: return 25;
	case EINPROGRESS: return 26;
	case EINTR: return 27;
	case EINVAL: return 28;
	case EIO: return 29;
	case EISCONN: return 30;
	case EISDIR: return 31;
	case ELOOP: return 32;
	case EMFILE: return 33;
	case EMLINK: return 34;
	case EMSGSIZE: return 35;
	case ENAMETOOLONG: return 37;
	case ENFILE: return 41;
	case ENOBUFS: return 42;
	case ENODEV: return 43;
	case ENOENT: return 44;
	case ENOMEM: return 48;
	case ENOSPC: return 51;
	case ENOSYS: return 52;
	case ENOTCONN: return 53;
	case ENOTDIR: return 54;
	case ENOTEMPTY: return 55;
	case ENOTSUP: return 58;
	case ENOTTY: return 59;
	case ENXIO: return 60;
	case EOVERFLOW: return 61;
	case EPERM: return 63;
	case EPIPE: return 64;
	case ERANGE: return 68;
	case EROFS: return 69;
	case ESPIPE: return 70;
	case ETIMEDOUT: return 73;
	case EXDEV: return 75;
	default: return 29;
	}
}

static uint32_t fail(void)
{
	return wasi_errno(errno);
}

/* ---------- guest memory access */

static void put8(uint32_t offset, uint8_t value) { host_memory_base[offset] = value; }
static void put16(uint32_t offset, uint16_t value) { memcpy(host_memory_base + offset, &value, 2); }
static void put32(uint32_t offset, uint32_t value) { memcpy(host_memory_base + offset, &value, 4); }
static void put64(uint32_t offset, uint64_t value) { memcpy(host_memory_base + offset, &value, 8); }
static uint32_t get32(uint32_t offset) { uint32_t value; memcpy(&value, host_memory_base + offset, 4); return value; }
static uint64_t get64(uint32_t offset) { uint64_t value; memcpy(&value, host_memory_base + offset, 8); return value; }
static uint16_t get16(uint32_t offset) { uint16_t value; memcpy(&value, host_memory_base + offset, 2); return value; }

/* a guest path (not NUL-terminated) as a host path relative to the root:
"/" for the root itself */
static int host_path(uint32_t directory, uint32_t path, uint32_t length, char *out, size_t size)
{
	if (directory != ROOT_DESCRIPTOR)
	{
		errno = EBADF;
		return -1;
	}
	if (length + 2 > size)
	{
		errno = ENAMETOOLONG;
		return -1;
	}
	out[0] = '/';
	memcpy(out + 1, host_memory_base + path, length);
	out[length + 1] = '\0';
	return 0;
}

static uint64_t timespec_ns(struct timespec time)
{
	return (uint64_t)time.tv_sec * 1000000000ull + (uint64_t)time.tv_nsec;
}

static uint8_t file_type(mode_t mode)
{
	if (S_ISREG(mode)) return 4;
	if (S_ISDIR(mode)) return 3;
	if (S_ISLNK(mode)) return 7;
	if (S_ISCHR(mode)) return 2;
	if (S_ISBLK(mode)) return 1;
	if (S_ISSOCK(mode)) return 6;
	return 0;
}

/* __wasi_filestat_t: dev, ino, filetype, nlink, size, atim, mtim, ctim */
static void put_filestat(uint32_t out, const struct stat *st)
{
	memset(host_memory_base + out, 0, 64);
	put64(out + 0, (uint64_t)st->st_dev);
	put64(out + 8, (uint64_t)st->st_ino);
	put8(out + 16, file_type(st->st_mode));
	put64(out + 24, (uint64_t)st->st_nlink);
	put64(out + 32, (uint64_t)st->st_size);
	put64(out + 40, timespec_ns(st->st_atimespec));
	put64(out + 48, timespec_ns(st->st_mtimespec));
	put64(out + 56, timespec_ns(st->st_ctimespec));
}

/* ---------- arguments and environment */

static int argument_count;
static char **arguments;

void host_wasi_set_arguments(int count, char **values)
{
	argument_count = count;
	arguments = values;
}

extern char **environ;

static uint32_t strings_get(char **strings, int count, uint32_t pointers, uint32_t buffer)
{
	int index;

	for (index = 0; index < count; index++)
	{
		size_t length = strlen(strings[index]) + 1;

		put32(pointers + 4 * (uint32_t)index, buffer);
		memcpy(host_memory_base + buffer, strings[index], length);
		buffer += (uint32_t)length;
	}
	return 0;
}

static uint32_t strings_sizes(char **strings, int count, uint32_t count_out, uint32_t size_out)
{
	uint32_t size = 0;
	int index;

	for (index = 0; index < count; index++)
		size += (uint32_t)strlen(strings[index]) + 1;
	put32(count_out, (uint32_t)count);
	put32(size_out, size);
	return 0;
}

static int environment_count(void)
{
	int count = 0;

	while (environ[count])
		count++;
	return count;
}

u32 w2c_wasi__snapshot__preview1_args_get(struct w2c_wasi__snapshot__preview1 *instance, u32 pointers, u32 buffer)
{
	(void)instance;
	return strings_get(arguments, argument_count, pointers, buffer);
}

u32 w2c_wasi__snapshot__preview1_args_sizes_get(struct w2c_wasi__snapshot__preview1 *instance, u32 count, u32 size)
{
	(void)instance;
	return strings_sizes(arguments, argument_count, count, size);
}

u32 w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1 *instance, u32 pointers, u32 buffer)
{
	(void)instance;
	return strings_get(environ, environment_count(), pointers, buffer);
}

u32 w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1 *instance, u32 count,
	u32 size)
{
	(void)instance;
	return strings_sizes(environ, environment_count(), count, size);
}

/* ---------- clocks */

static uint64_t clock_now(uint32_t id)
{
	struct timespec time;

	switch (id)
	{
	case 0: clock_gettime(CLOCK_REALTIME, &time); break;
	case 2: clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time); break;
	case 3: clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time); break;
	default: return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	}
	return timespec_ns(time);
}

u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1 *instance, u32 id,
	u64 precision, u32 out)
{
	(void)instance;
	(void)precision;
	if (id > 3)
		return WASI_EINVAL;
	put64(out, clock_now(id));
	return 0;
}

u32 w2c_wasi__snapshot__preview1_clock_res_get(struct w2c_wasi__snapshot__preview1 *instance, u32 id, u32 out)
{
	(void)instance;
	(void)id;
	put64(out, 1);
	return 0;
}

/* ---------- descriptors */

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *instance, u32 fd)
{
	(void)instance;
	if (fd <= 2 || fd == ROOT_DESCRIPTOR)
		return 0;
	return close((int)fd) == 0 ? 0 : fail();
}

/* __wasi_fdstat_t: filetype, flags, rights base, rights inheriting */
u32 w2c_wasi__snapshot__preview1_fd_fdstat_get(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 out)
{
	struct stat st;
	int flags;
	uint16_t wasi_flags = 0;

	(void)instance;
	memset(host_memory_base + out, 0, 24);
	if (fd == ROOT_DESCRIPTOR)
	{
		put8(out, 3);
		put64(out + 8, ~0ull);
		put64(out + 16, ~0ull);
		return 0;
	}
	if (fstat((int)fd, &st) != 0)
		return fail();
	flags = fcntl((int)fd, F_GETFL);
	if (flags >= 0)
	{
		if (flags & O_APPEND) wasi_flags |= 1;
		if (flags & O_NONBLOCK) wasi_flags |= 4;
	}
	put8(out, fd <= 2 && isatty((int)fd) ? 2 : file_type(st.st_mode));
	put16(out + 2, wasi_flags);
	put64(out + 8, ~0ull);
	put64(out + 16, ~0ull);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_set_flags(struct w2c_wasi__snapshot__preview1 *instance, u32 fd,
	u32 wasi_flags)
{
	int flags;

	(void)instance;
	flags = fcntl((int)fd, F_GETFL);
	if (flags < 0)
		return fail();
	flags &= ~(O_APPEND | O_NONBLOCK);
	if (wasi_flags & 1) flags |= O_APPEND;
	if (wasi_flags & 4) flags |= O_NONBLOCK;
	return fcntl((int)fd, F_SETFL, flags) == 0 ? 0 : fail();
}

u32 w2c_wasi__snapshot__preview1_fd_filestat_get(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 out)
{
	struct stat st;

	(void)instance;
	if (fd == ROOT_DESCRIPTOR)
	{
		if (stat("/", &st) != 0)
			return fail();
	}
	else if (fstat((int)fd, &st) != 0)
	{
		return fail();
	}
	put_filestat(out, &st);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_filestat_set_size(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u64 size)
{
	(void)instance;
	return ftruncate((int)fd, (off_t)size) == 0 ? 0 : fail();
}

static void times_from(u64 time, u32 flags, int now_bit, int set_bit, struct timespec *out)
{
	if (flags & (u32)now_bit)
	{
		out->tv_sec = 0;
		out->tv_nsec = UTIME_NOW;
	}
	else if (flags & (u32)set_bit)
	{
		out->tv_sec = (time_t)(time / 1000000000ull);
		out->tv_nsec = (long)(time % 1000000000ull);
	}
	else
	{
		out->tv_sec = 0;
		out->tv_nsec = UTIME_OMIT;
	}
}

u32 w2c_wasi__snapshot__preview1_fd_filestat_set_times(struct w2c_wasi__snapshot__preview1 *instance, u32 fd,
	u64 access, u64 modification, u32 flags)
{
	struct timespec times[2];

	(void)instance;
	times_from(access, flags, 2, 1, &times[0]);
	times_from(modification, flags, 8, 4, &times[1]);
	return futimens((int)fd, times) == 0 ? 0 : fail();
}

/* the iovecs (pointer, length pairs) of guest memory */
static int iovecs(uint32_t list, uint32_t count, struct iovec *vectors, int capacity)
{
	uint32_t index;

	if (count > (uint32_t)capacity)
		count = (uint32_t)capacity;
	for (index = 0; index < count; index++)
	{
		vectors[index].iov_base = host_memory_base + get32(list + 8 * index);
		vectors[index].iov_len = get32(list + 8 * index + 4);
	}
	return (int)count;
}

u32 w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 list, u32 count,
	u32 out)
{
	struct iovec vectors[64];
	ssize_t result;

	(void)instance;
	result = readv((int)fd, vectors, iovecs(list, count, vectors, 64));
	if (result < 0)
		return fail();
	put32(out, (uint32_t)result);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_pread(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 list, u32 count,
	u64 offset, u32 out)
{
	struct iovec vectors[64];
	ssize_t result;

	(void)instance;
	result = preadv((int)fd, vectors, iovecs(list, count, vectors, 64), (off_t)offset);
	if (result < 0)
		return fail();
	put32(out, (uint32_t)result);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 list, u32 count,
	u32 out)
{
	struct iovec vectors[64];
	ssize_t result;

	(void)instance;
	result = writev((int)fd, vectors, iovecs(list, count, vectors, 64));
	if (result < 0)
		return fail();
	put32(out, (uint32_t)result);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_pwrite(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 list, u32 count,
	u64 offset, u32 out)
{
	struct iovec vectors[64];
	ssize_t result;

	(void)instance;
	result = pwritev((int)fd, vectors, iovecs(list, count, vectors, 64), (off_t)offset);
	if (result < 0)
		return fail();
	put32(out, (uint32_t)result);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u64 offset,
	u32 whence, u32 out)
{
	off_t result;

	(void)instance;
	result = lseek((int)fd, (off_t)(int64_t)offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
	if (result < 0)
		return fail();
	put64(out, (uint64_t)result);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_tell(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 out)
{
	return w2c_wasi__snapshot__preview1_fd_seek(instance, fd, 0, 1, out);
}

u32 w2c_wasi__snapshot__preview1_fd_sync(struct w2c_wasi__snapshot__preview1 *instance, u32 fd)
{
	(void)instance;
	return fsync((int)fd) == 0 ? 0 : fail();
}

u32 w2c_wasi__snapshot__preview1_fd_datasync(struct w2c_wasi__snapshot__preview1 *instance, u32 fd)
{
	return w2c_wasi__snapshot__preview1_fd_sync(instance, fd);
}

u32 w2c_wasi__snapshot__preview1_fd_advise(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u64 offset,
	u64 length, u32 advice)
{
	(void)instance; (void)fd; (void)offset; (void)length; (void)advice;
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_allocate(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u64 offset,
	u64 length)
{
	struct stat st;

	(void)instance;
	if (fstat((int)fd, &st) != 0)
		return fail();
	if ((uint64_t)st.st_size < offset + length && ftruncate((int)fd, (off_t)(offset + length)) != 0)
		return fail();
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_renumber(struct w2c_wasi__snapshot__preview1 *instance, u32 from, u32 to)
{
	(void)instance;
	if (dup2((int)from, (int)to) < 0)
		return fail();
	close((int)from);
	return 0;
}

/* the one preopen: "/" */
u32 w2c_wasi__snapshot__preview1_fd_prestat_get(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 out)
{
	(void)instance;
	if (fd != ROOT_DESCRIPTOR)
		return WASI_EBADF;
	put8(out, 0);
	put32(out + 4, 1);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_prestat_dir_name(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 path,
	u32 length)
{
	(void)instance;
	if (fd != ROOT_DESCRIPTOR || length < 1)
		return WASI_EBADF;
	host_memory_base[path] = '/';
	return 0;
}

/* a directory's entries from cookie on, as __wasi_dirent_t (next cookie,
inode, name length, type) each followed by its name, into the buffer as
far as it goes (wasi-libc asks again with a larger one when an entry is
cut short) */
u32 w2c_wasi__snapshot__preview1_fd_readdir(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 buffer,
	u32 size, u64 cookie, u32 out)
{
	DIR *directory;
	int copy;
	uint32_t used = 0;
	uint64_t index = 0;
	struct dirent *entry;

	(void)instance;
	copy = dup((int)fd);
	if (copy < 0)
		return fail();
	directory = fdopendir(copy);
	if (!directory)
	{
		close(copy);
		return fail();
	}
	rewinddir(directory);
	while ((entry = readdir(directory)) != NULL)
	{
		unsigned char header[24];
		uint32_t name_length = (uint32_t)strlen(entry->d_name);
		uint64_t next = index + 1;
		uint64_t inode = entry->d_ino;
		uint8_t type = entry->d_type == DT_DIR ? 3 : entry->d_type == DT_REG ? 4 : entry->d_type == DT_LNK ? 7 : 0;
		uint32_t take;

		if (index++ < cookie)
			continue;
		memset(header, 0, sizeof(header));
		memcpy(header, &next, 8);
		memcpy(header + 8, &inode, 8);
		memcpy(header + 16, &name_length, 4);
		header[20] = type;
		take = size - used < 24 ? size - used : 24;
		memcpy(host_memory_base + buffer + used, header, take);
		used += take;
		take = size - used < name_length ? size - used : name_length;
		memcpy(host_memory_base + buffer + used, entry->d_name, take);
		used += take;
		if (used >= size)
			break;
	}
	closedir(directory);
	put32(out, used);
	return 0;
}

/* ---------- paths */

u32 w2c_wasi__snapshot__preview1_path_open(struct w2c_wasi__snapshot__preview1 *instance, u32 directory,
	u32 lookup_flags, u32 path, u32 length, u32 open_flags, u64 rights, u64 inheriting, u32 fd_flags, u32 out)
{
	char host[4096];
	int flags = O_CLOEXEC, descriptor;

	(void)instance;
	(void)inheriting;
	if (host_path(directory, path, length, host, sizeof(host)) != 0)
		return fail();
	/* rights: read (bit 1) and write (bit 6) */
	if ((rights & ((1ull << 1) | (1ull << 6))) == ((1ull << 1) | (1ull << 6)))
		flags |= O_RDWR;
	else if (rights & (1ull << 6))
		flags |= O_WRONLY;
	else
		flags |= O_RDONLY;
	if (open_flags & 1) flags |= O_CREAT;
	if (open_flags & 2) flags |= O_DIRECTORY;
	if (open_flags & 4) flags |= O_EXCL;
	if (open_flags & 8) flags |= O_TRUNC;
	if (fd_flags & 1) flags |= O_APPEND;
	if (fd_flags & 4) flags |= O_NONBLOCK;
	if (!(lookup_flags & 1)) flags |= O_NOFOLLOW;
	descriptor = open(host, flags, 0666);
	/* (a directory asked for writing: opened for reading, as wasi-libc
	opens directories to list them) */
	if (descriptor < 0 && errno == EISDIR)
		descriptor = open(host, (flags & ~(O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) | O_RDONLY, 0666);
	if (descriptor < 0)
		return fail();
	put32(out, (uint32_t)descriptor);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_path_filestat_get(struct w2c_wasi__snapshot__preview1 *instance, u32 directory,
	u32 lookup_flags, u32 path, u32 length, u32 out)
{
	char host[4096];
	struct stat st;

	(void)instance;
	if (host_path(directory, path, length, host, sizeof(host)) != 0)
		return fail();
	if ((lookup_flags & 1 ? stat(host, &st) : lstat(host, &st)) != 0)
		return fail();
	put_filestat(out, &st);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_path_filestat_set_times(struct w2c_wasi__snapshot__preview1 *instance,
	u32 directory, u32 lookup_flags, u32 path, u32 length, u64 access, u64 modification, u32 flags)
{
	char host[4096];
	struct timespec times[2];

	(void)instance;
	if (host_path(directory, path, length, host, sizeof(host)) != 0)
		return fail();
	times_from(access, flags, 2, 1, &times[0]);
	times_from(modification, flags, 8, 4, &times[1]);
	return utimensat(AT_FDCWD, host, times, lookup_flags & 1 ? 0 : AT_SYMLINK_NOFOLLOW) == 0 ? 0 : fail();
}

u32 w2c_wasi__snapshot__preview1_path_create_directory(struct w2c_wasi__snapshot__preview1 *instance, u32 directory,
	u32 path, u32 length)
{
	char host[4096];

	(void)instance;
	if (host_path(directory, path, length, host, sizeof(host)) != 0)
		return fail();
	return mkdir(host, 0777) == 0 ? 0 : fail();
}

u32 w2c_wasi__snapshot__preview1_path_remove_directory(struct w2c_wasi__snapshot__preview1 *instance, u32 directory,
	u32 path, u32 length)
{
	char host[4096];

	(void)instance;
	if (host_path(directory, path, length, host, sizeof(host)) != 0)
		return fail();
	return rmdir(host) == 0 ? 0 : fail();
}

u32 w2c_wasi__snapshot__preview1_path_unlink_file(struct w2c_wasi__snapshot__preview1 *instance, u32 directory,
	u32 path, u32 length)
{
	char host[4096];

	(void)instance;
	if (host_path(directory, path, length, host, sizeof(host)) != 0)
		return fail();
	return unlink(host) == 0 ? 0 : fail();
}

u32 w2c_wasi__snapshot__preview1_path_rename(struct w2c_wasi__snapshot__preview1 *instance, u32 from_directory,
	u32 from, u32 from_length, u32 to_directory, u32 to, u32 to_length)
{
	char host_from[4096], host_to[4096];

	(void)instance;
	if (host_path(from_directory, from, from_length, host_from, sizeof(host_from)) != 0 ||
		host_path(to_directory, to, to_length, host_to, sizeof(host_to)) != 0)
	{
		return fail();
	}
	return rename(host_from, host_to) == 0 ? 0 : fail();
}

u32 w2c_wasi__snapshot__preview1_path_readlink(struct w2c_wasi__snapshot__preview1 *instance, u32 directory,
	u32 path, u32 length, u32 buffer, u32 size, u32 out)
{
	char host[4096];
	ssize_t result;

	(void)instance;
	if (host_path(directory, path, length, host, sizeof(host)) != 0)
		return fail();
	result = readlink(host, (char *)host_memory_base + buffer, size);
	if (result < 0)
		return fail();
	put32(out, (uint32_t)result);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_path_symlink(struct w2c_wasi__snapshot__preview1 *instance, u32 target,
	u32 target_length, u32 directory, u32 path, u32 length)
{
	char host[4096], link_target[4096];

	(void)instance;
	if (host_path(directory, path, length, host, sizeof(host)) != 0 || target_length >= sizeof(link_target))
		return fail();
	memcpy(link_target, host_memory_base + target, target_length);
	link_target[target_length] = '\0';
	return symlink(link_target, host) == 0 ? 0 : fail();
}

u32 w2c_wasi__snapshot__preview1_path_link(struct w2c_wasi__snapshot__preview1 *instance, u32 from_directory,
	u32 lookup_flags, u32 from, u32 from_length, u32 to_directory, u32 to, u32 to_length)
{
	char host_from[4096], host_to[4096];

	(void)instance;
	(void)lookup_flags;
	if (host_path(from_directory, from, from_length, host_from, sizeof(host_from)) != 0 ||
		host_path(to_directory, to, to_length, host_to, sizeof(host_to)) != 0)
	{
		return fail();
	}
	return link(host_from, host_to) == 0 ? 0 : fail();
}

/* ---------- waiting */

/* clock subscriptions only (the sleeps: nanosleep and the like); a
descriptor's is answered at once as ready */
u32 w2c_wasi__snapshot__preview1_poll_oneoff(struct w2c_wasi__snapshot__preview1 *instance, u32 subscriptions,
	u32 events, u32 count, u32 out)
{
	uint64_t now, wake = ~0ull;
	uint32_t index, ready = 0;

	(void)instance;
	if (!count)
		return WASI_EINVAL;
	for (index = 0; index < count; index++)
	{
		uint32_t subscription = subscriptions + 48 * index;
		uint8_t type = host_memory_base[subscription + 8];

		if (type == 0)
		{
			uint32_t id = get32(subscription + 16);
			uint64_t timeout = get64(subscription + 24);
			uint16_t flags = get16(subscription + 40);
			uint64_t at = flags & 1 ? timeout : clock_now(id) + timeout;

			if (id == 0 && !(flags & 1))
				at = clock_now(1) + timeout;
			else if (id == 0)
				at = clock_now(1) + (timeout > clock_now(0) ? timeout - clock_now(0) : 0);
			if (at < wake)
				wake = at;
		}
		else
		{
			ready++;
		}
	}
	now = clock_now(1);
	if (!ready && wake != ~0ull && wake > now)
	{
		struct timespec duration;
		uint64_t left = wake - now;

		duration.tv_sec = (time_t)(left / 1000000000ull);
		duration.tv_nsec = (long)(left % 1000000000ull);
		while (nanosleep(&duration, &duration) != 0 && errno == EINTR)
			;
	}
	/* every subscription's event: the clocks have passed, and descriptors
	are taken as ready */
	for (index = 0; index < count; index++)
	{
		uint32_t subscription = subscriptions + 48 * index;
		uint32_t event = events + 32 * index;

		memset(host_memory_base + event, 0, 32);
		put64(event, get64(subscription));
		put8(event + 10, host_memory_base[subscription + 8]);
	}
	put32(out, count);
	return 0;
}

u32 w2c_wasi__snapshot__preview1_sched_yield(struct w2c_wasi__snapshot__preview1 *instance)
{
	(void)instance;
	sched_yield();
	return 0;
}

u32 w2c_wasi__snapshot__preview1_random_get(struct w2c_wasi__snapshot__preview1 *instance, u32 buffer, u32 size)
{
	(void)instance;
	while (size)
	{
		uint32_t take = size > 256 ? 256 : size;

		if (getentropy(host_memory_base + buffer, take) != 0)
			return fail();
		buffer += take;
		size -= take;
	}
	return 0;
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *instance, u32 code)
{
	(void)instance;
	fflush(stdout);
	fflush(stderr);
	exit((int)code);
}

u32 w2c_wasi__snapshot__preview1_proc_raise(struct w2c_wasi__snapshot__preview1 *instance, u32 signal_number)
{
	(void)instance;
	host_fatal("the game raised signal %u", signal_number);
}

/* the guest's sockets are the host's, through posix_net.c */
u32 w2c_wasi__snapshot__preview1_sock_accept(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 flags, u32 out)
{
	(void)instance; (void)fd; (void)flags; (void)out;
	return WASI_ENOTSUP;
}

u32 w2c_wasi__snapshot__preview1_sock_recv(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 list, u32 count,
	u32 flags, u32 out_size, u32 out_flags)
{
	(void)instance; (void)fd; (void)list; (void)count; (void)flags; (void)out_size; (void)out_flags;
	return WASI_ENOTSUP;
}

u32 w2c_wasi__snapshot__preview1_sock_send(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 list, u32 count,
	u32 flags, u32 out)
{
	(void)instance; (void)fd; (void)list; (void)count; (void)flags; (void)out;
	return WASI_ENOTSUP;
}

u32 w2c_wasi__snapshot__preview1_sock_shutdown(struct w2c_wasi__snapshot__preview1 *instance, u32 fd, u32 how)
{
	(void)instance; (void)fd; (void)how;
	return WASI_ENOTSUP;
}
