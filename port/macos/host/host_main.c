/*
HOST_MAIN.C

The macOS build's program (port/macos/README.md): it makes the game's
memory, runs the game (the guest, translated by wasm2c: halo_w2c.h) on the
main thread, and starts a thread for each of the game's threads.

Memory. The guest's memory is 0xA0000000 bytes: the C heap below 0x80000000
(the link's initial memory ends there, so that __heap_end does), and the
Xbox's memory window above it, the desktop builds' 512 MB, 0x80000000 to
0xA0000000 (port/linux/src/platform.h; the web's and Android's are 128 MB),
which the desktop's texture and sound caches need. Its pages are backed only
once they are used. wasm2c's runtime reserves 8 GB of
address space for it, every byte past the end inaccessible, so that a 32-bit
offset plus a 32-bit displacement never needs a bounds check: an access
outside it is a fault, which the runtime reports as a trap.

Threads. Each of the game's threads (wasi-libc's pthread_create, through
wasi's thread-spawn) runs on a host thread with a wasm2c instance of its own
(its stack pointer and thread pointer are an instance's globals), every
instance sharing the one memory. The memory is compiled as unshared
(wasm2c --unshared-memory): plain loads and stores, as native code makes
them; the atomic operations stay atomic, and wait and notify are the host's
(os_sync_wait_on_address, below).

The main thread. AppKit wants the window and its events on the process's
main thread, and the game makes its window and pumps its events on the
thread that runs its main: so the game runs on the main thread, and nothing
else here needs it.
*/

#include "host.h"
#include "host_frame_log.h"
#include "halo_w2c.h"

#include <errno.h>
#include <mach/mach_time.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* the guest's memory: 0xA0000000 bytes (tools/macos_build.py links its
initial memory as 0x80000000, the heap's end, and its maximum as this) */
#define GUEST_MEMORY_PAGES (0xA0000000ull / WASM_DEFAULT_PAGE_SIZE)

struct w2c_env
{
	wasm_rt_memory_t memory;
};
struct w2c_wasi
{
	int unused;
};
struct w2c_wasi__snapshot__preview1
{
	int unused;
};

static struct w2c_env env;
static struct w2c_wasi wasi;
static struct w2c_wasi__snapshot__preview1 preview1;

uint8_t *host_memory_base;
uint64_t host_memory_size;

/* the thread's own instance (its globals: stack pointer, thread pointer) */
static _Thread_local w2c_halo *thread_instance;

wasm_rt_memory_t *w2c_env_memory(struct w2c_env *instance)
{
	return &instance->memory;
}

void host_wasi_set_arguments(int count, char **values);
void host_sdl_main_thread_set(void);

/* ---------- logging */

void host_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	fputs("host: ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

void host_fatal(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	fputs("host: fatal: ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
	fflush(stderr);
	abort();
}

/* ---------- wait and notify (memory.atomic.wait32/64, notify) */

/* nanoseconds to Mach absolute time: 1 to 1 on Apple silicon (its timebase
is 125/3 on Intel Macs, which this build does not run on) */
static uint64_t mach_from_ns(int64_t nanoseconds)
{
	static mach_timebase_info_data_t timebase;

	if (!timebase.denom)
		mach_timebase_info(&timebase);
	return (uint64_t)nanoseconds * timebase.denom / timebase.numer;
}

u32 wasm_rt_atomic_wait32(void *address, u32 expected, s64 timeout)
{
	int result;
	uint64_t waited;

	if (__atomic_load_n((u32 *)address, __ATOMIC_SEQ_CST) != expected)
		return 1;
	/* (the main thread's waits, the game's frame pacing among them, are
	not its work: HALO_FRAME_LOG) */
	waited = host_frame_log_detail() ? host_frame_log_now() : 0;
	if (timeout < 0)
		result = os_sync_wait_on_address(address, expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
	else
		result = os_sync_wait_on_address_with_timeout(address, expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE,
			OS_CLOCK_MACH_ABSOLUTE_TIME, mach_from_ns(timeout));
	if (waited)
		host_frame_acc.sleep_ns += host_frame_log_now() - waited;
	return result < 0 && errno == ETIMEDOUT ? 2 : 0;
}

u32 wasm_rt_atomic_wait64(void *address, u64 expected, s64 timeout)
{
	int result;
	uint64_t waited;

	if (__atomic_load_n((u64 *)address, __ATOMIC_SEQ_CST) != expected)
		return 1;
	waited = host_frame_log_detail() ? host_frame_log_now() : 0;
	if (timeout < 0)
		result = os_sync_wait_on_address(address, expected, 8, OS_SYNC_WAIT_ON_ADDRESS_NONE);
	else
		result = os_sync_wait_on_address_with_timeout(address, expected, 8, OS_SYNC_WAIT_ON_ADDRESS_NONE,
			OS_CLOCK_MACH_ABSOLUTE_TIME, mach_from_ns(timeout));
	if (waited)
		host_frame_acc.sleep_ns += host_frame_log_now() - waited;
	return result < 0 && errno == ETIMEDOUT ? 2 : 0;
}

u32 wasm_rt_atomic_notify(void *address, u32 count)
{
	u32 woken = 0;

	if (!count)
		return 0;
	if (count == 0xffffffffu)
	{
		os_sync_wake_by_address_all(address, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
		return 1;
	}
	while (woken < count && os_sync_wake_by_address_any(address, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE) == 0)
		woken++;
	return woken;
}

/* ---------- traps */

/* a trap ends the game: the guest has no handler to unwind to */
void host_trap(wasm_rt_trap_t code)
{
	host_fatal("the game trapped: %s", wasm_rt_strerror(code));
}

/* ---------- threads (wasi's thread-spawn) */

struct thread_start
{
	u32 tid;
	u32 argument;
};

static atomic_uint next_tid = 1;

static void *thread_main(void *context)
{
	struct thread_start start = *(struct thread_start *)context;
	w2c_halo *instance;

	free(context);
	wasm_rt_init_thread();
	instance = calloc(1, sizeof(*instance));
	if (!instance)
		host_fatal("out of memory for a thread");
	/* (instantiating again runs no guest code but the memory's own set-up,
	which wasi-libc's __wasm_init_memory does once for every instance) */
	wasm2c_halo_instantiate(instance, &env, &wasi, &preview1);
	thread_instance = instance;
	w2c_halo_wasi_thread_start(instance, start.tid, start.argument);
	thread_instance = NULL;
	wasm2c_halo_free(instance);
	free(instance);
	wasm_rt_free_thread();
	return NULL;
}

u32 w2c_wasi_thread0x2Dspawn(struct w2c_wasi *instance, u32 argument)
{
	struct thread_start *start = malloc(sizeof(*start));
	pthread_attr_t attributes;
	pthread_t thread;
	int result;

	(void)instance;
	if (!start)
		return (u32)-6;
	start->tid = atomic_fetch_add(&next_tid, 1) & 0x1fffffff;
	start->argument = argument;
	pthread_attr_init(&attributes);
	/* (the native stack runs the translated code: deep, as the game's
	recursions are, and more than its guest stack holds) */
	pthread_attr_setstacksize(&attributes, 16 << 20);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	result = pthread_create(&thread, &attributes, thread_main, start);
	pthread_attr_destroy(&attributes);
	if (result != 0)
	{
		free(start);
		return (u32)-result;
	}
	return start->tid;
}

/* ---------- guest allocations (host.h) */

uint32_t host_guest_malloc(uint32_t size)
{
	if (!thread_instance)
		return 0;
	return w2c_halo_halo_guest_malloc(thread_instance, size);
}

void host_guest_free(uint32_t offset)
{
	if (thread_instance && offset)
		w2c_halo_halo_guest_free(thread_instance, offset);
}

uint32_t host_guest_strdup(const char *text)
{
	size_t length;
	uint32_t copy;

	if (!text)
		return 0;
	length = strlen(text) + 1;
	copy = host_guest_malloc((uint32_t)length);
	if (copy)
		memcpy(host_memory_base + copy, text, length);
	return copy;
}

/* ---------- the guest's errno, after a host call */

static _Thread_local int host_errno_value;

void host_errno_set_from_host(void)
{
	host_errno_value = errno;
}

/* errno as the guest numbers it (wasi-libc's, WASI's) */
u32 w2c_env_host_errno(struct w2c_env *instance)
{
	(void)instance;
	switch (host_errno_value)
	{
	case 0: return 0;
	case EACCES: return 2;
	case EAGAIN: return 6;
	case EBADF: return 8;
	case EEXIST: return 20;
	case EINTR: return 27;
	case EINVAL: return 28;
	case EIO: return 29;
	case EISDIR: return 31;
	case EMFILE: return 33;
	case ENAMETOOLONG: return 37;
	case ENOENT: return 44;
	case ENOMEM: return 48;
	case ENOSPC: return 51;
	case ENOTDIR: return 54;
	case ENOTEMPTY: return 55;
	case EPERM: return 63;
	case EROFS: return 69;
	default: return 29;
	}
}

/* ---------- the guest's memory */

/* HALO_GUEST_RESERVE_BYTES (the TV build: port/tvos/game/CMakeLists.txt):
the memory with guard pages in a reservation of this size instead of
wasm2c's 8 GB, which tvOS refuses. Guard pages need every 32-bit index plus
every load's and store's static offset (and its size) to land in the
reservation; tools/tvos_build.py checks the translated code's largest
offset against it. The layout is wasm2c's own (wasm-rt-mem-impl.c): the
memory at the reservation's start, the rest inaccessible. */
#ifdef HALO_GUEST_RESERVE_BYTES
#include <sys/mman.h>

#if !WASM_RT_MEMCHECK_GUARD_PAGES
#error "HALO_GUEST_RESERVE_BYTES is for WASM_RT_MEMCHECK_GUARD_PAGES"
#endif
_Static_assert((uint64_t)HALO_GUEST_RESERVE_BYTES > 0x100000000ull, "the reservation must cover 4 GB and a guard");

static void guest_memory_allocate(void)
{
	uint64_t size = GUEST_MEMORY_PAGES * WASM_DEFAULT_PAGE_SIZE;
	uint8_t *base = mmap(NULL, (size_t)HALO_GUEST_RESERVE_BYTES, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);

	if (base == MAP_FAILED)
		host_fatal("cannot reserve %llu bytes for the game's memory (errno %d)",
			(unsigned long long)HALO_GUEST_RESERVE_BYTES, errno);
	if (mprotect(base, (size_t)size, PROT_READ | PROT_WRITE) != 0)
		host_fatal("cannot make the game's memory accessible (errno %d)", errno);
	env.memory.data = base;
	env.memory.data_end = base + HALO_GUEST_RESERVE_BYTES;
	env.memory.page_size = WASM_DEFAULT_PAGE_SIZE;
	env.memory.pages = GUEST_MEMORY_PAGES;
	env.memory.max_pages = GUEST_MEMORY_PAGES;
	env.memory.size = size;
	env.memory.is64 = false;
	host_log("game memory: %llu MB, guard pages to %llu MB", (unsigned long long)(size >> 20),
		(unsigned long long)((uint64_t)HALO_GUEST_RESERVE_BYTES >> 20));
}
#else
static void guest_memory_allocate(void)
{
	wasm_rt_allocate_memory(&env.memory, (uint32_t)GUEST_MEMORY_PAGES, (uint32_t)GUEST_MEMORY_PAGES, false,
		WASM_DEFAULT_PAGE_SIZE);
}
#endif

#if WASM_RT_MEMCHECK_GUARD_PAGES
/* With guard pages a guest access out of bounds is a fault in the
reservation. wasm2c's handler calls only SEGV_ACCERR out of bounds and
everything else stack exhaustion, and macOS raises SIGBUS there: so this
one, installed after it, decides by the address. A fault anywhere else is
the host's own: the default action, a crash report naming it. */
static void guest_fault(int signal_number, siginfo_t *info, void *context)
{
	uint8_t *address = info->si_addr;

	(void)context;
	if (address >= (uint8_t *)env.memory.data && address < (uint8_t *)env.memory.data_end)
		wasm_rt_trap(WASM_RT_TRAP_OOB);
	signal(signal_number, SIG_DFL);
}

static void guest_fault_handler_install(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = guest_fault;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
}
#else
static void guest_fault_handler_install(void)
{
}
#endif

/* ---------- start */

int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message);

/* the data folder (the one holding maps): HALO_DATA_ROOT, or else
~/Games/Halo, so that the app starts from Finder, where there is no
environment to name it; the guest enters it (port/macos/guest/guest_main.c) */
#ifndef HALO_TVOS
static void data_root_choose(void)
{
	static char path[1024];
	const char *home = getenv("HOME");
	char maps[1100];

	if (getenv("HALO_DATA_ROOT") && *getenv("HALO_DATA_ROOT"))
		return;
	snprintf(path, sizeof(path), "%s/Games/Halo", home && *home ? home : ".");
	setenv("HALO_DATA_ROOT", path, 1);
	snprintf(maps, sizeof(maps), "%s/maps", path);
	if (access(maps, F_OK) != 0)
	{
		char message[1400];

		snprintf(message, sizeof(message),
			"Halo's maps were not found.\n\nPut the game's maps folder in\n%s\n"
			"(so that %s/maps/ui.map exists), or set HALO_DATA_ROOT to the folder that holds it.",
			path, path);
		fprintf(stderr, "%s\n", message);
		host_sdl_show_simple_message_box(0x10 /* SDL_MESSAGEBOX_ERROR */, "OpenCE", message);
		exit(EXIT_FAILURE);
	}
}

#endif

#ifdef HALO_TVOS
/* The TV entry supplies cache paths and enters UIKit through SDL_RunApp. */
int opence_guest_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	static w2c_halo instance;
	struct rlimit limit;

	/* (sockets of a closed connection fail rather than end the game) */
	signal(SIGPIPE, SIG_IGN);
	/* (the game opens many files at once: its maps, their caches) */
	if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < 4096)
	{
		limit.rlim_cur = limit.rlim_max < 4096 ? limit.rlim_max : 4096;
		setrlimit(RLIMIT_NOFILE, &limit);
	}
	host_wasi_set_arguments(argc, argv);
	if (getenv("HALO_VM_PROBE") && !strcmp(getenv("HALO_VM_PROBE"), "1"))
	{
		host_vm_probe();
		exit(0);
	}
#ifndef HALO_TVOS
	data_root_choose();
#endif
	host_sdl_main_thread_set();
	wasm_rt_init();
	guest_memory_allocate();
	guest_fault_handler_install();
	host_memory_base = (uint8_t *)env.memory.data;
	host_memory_size = env.memory.size;
	wasm2c_halo_instantiate(&instance, &env, &wasi, &preview1);
	thread_instance = &instance;
	w2c_halo_0x5Fstart(&instance);
	/* (_start returns only when main does; exit is proc_exit) */
	fflush(stdout);
	return 0;
}
