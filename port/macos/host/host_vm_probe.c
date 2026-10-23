/*
HOST_VM_PROBE.C

HALO_VM_PROBE=1: before the game starts, report what address-space
reservations this process may make, and whether an access to a reserved,
inaccessible page reaches a signal handler, then exit. A diagnostic for
the guest memory's layout (port/tvos/PERFORMANCE.md, "CPU (A12)"): wasm2c's
guard pages need the guest's 4 GB of addresses plus its largest static
offset reserved, where tvOS refused wasm2c's own 8 GB. Nothing here
commits memory but one page.
*/

#include "host.h"

#include <errno.h>
#include <mach/mach.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define GIB (1ull << 30)
#define MIB (1ull << 20)

static void *reserve(uint64_t size, int flags)
{
	void *address = mmap(NULL, (size_t)size, PROT_NONE, MAP_PRIVATE | MAP_ANON | flags, -1, 0);
	return address == MAP_FAILED ? NULL : address;
}

static void report_size(const char *what, uint64_t size, int flags)
{
	void *address = reserve(size, flags);
	int saved = errno;

	fprintf(stderr, "vm probe: %-28s %7.3f GiB: %s", what, (double)size / GIB, address ? "ok" : "FAILED");
	if (!address)
		fprintf(stderr, " (errno %d %s)", saved, strerror(saved));
	fputc('\n', stderr);
	if (address)
		munmap(address, (size_t)size);
}

/* the largest single reservation, to 64 MiB */
static uint64_t largest(void)
{
	uint64_t low = 0, high = 64 * GIB;

	while (high - low > 64 * MIB)
	{
		uint64_t middle = (low + high) / 2 & ~(64 * MIB - 1);
		void *address = reserve(middle, 0);

		if (address)
		{
			munmap(address, (size_t)middle);
			low = middle;
		}
		else
			high = middle;
	}
	return low;
}

/* everything reservable at once, in 256 MiB pieces */
static uint64_t total(void)
{
	enum { PIECES = 512 };
	static void *pieces[PIECES];
	uint64_t piece = 256 * MIB, sum = 0;
	int count = 0;

	while (count < PIECES && (pieces[count] = reserve(piece, 0)))
		count++, sum += piece;
	while (count--)
		munmap(pieces[count], (size_t)piece);
	return sum;
}

static sigjmp_buf probe_jump;
static volatile sig_atomic_t probe_signal, probe_code;
static void *volatile probe_address;

static void probe_handler(int signal_number, siginfo_t *info, void *context)
{
	(void)context;
	probe_signal = signal_number;
	probe_code = info->si_code;
	probe_address = info->si_addr;
	siglongjmp(probe_jump, 1);
}

/* the guard-page layout: 4 GiB + guard reserved, the guest's 2.5 GiB made
accessible, a page in the Xbox window written, then reads past the end */
static void guard_layout(uint64_t guard)
{
	uint64_t size = 4 * GIB + guard, accessible = 0xA0000000ull;
	uint8_t *base = reserve(size, 0);
	struct sigaction action, old_segv, old_bus;
	uint64_t offsets[] = { accessible, 4 * GIB - 1, 4 * GIB + guard - 16 };

	if (!base)
	{
		fprintf(stderr, "vm probe: guard layout 4 GiB + %llu MiB: reservation FAILED (errno %d)\n",
			(unsigned long long)(guard / MIB), errno);
		return;
	}
	if (mprotect(base, (size_t)accessible, PROT_READ | PROT_WRITE) != 0)
	{
		fprintf(stderr, "vm probe: guard layout: mprotect of 2.5 GiB FAILED (errno %d)\n", errno);
		munmap(base, (size_t)size);
		return;
	}
	base[0x80000000ull] = 0x42;
	fprintf(stderr, "vm probe: guard layout 4 GiB + %llu MiB at %p: reserved, 2.5 GiB accessible, Xbox page written\n",
		(unsigned long long)(guard / MIB), (void *)base);
	fprintf(stderr, "vm probe: with it held, largest further reservation %.2f GiB, all further %.2f GiB\n",
		(double)largest() / GIB, (double)total() / GIB);

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = probe_handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, &old_segv);
	sigaction(SIGBUS, &action, &old_bus);
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
	{
		probe_signal = 0;
		if (!sigsetjmp(probe_jump, 1))
		{
			volatile uint8_t value = *(volatile uint8_t *)(base + offsets[i]);
			(void)value;
			fprintf(stderr, "vm probe: read at +0x%llx: NO FAULT (guard pages do not work)\n",
				(unsigned long long)offsets[i]);
		}
		else
			fprintf(stderr, "vm probe: read at +0x%llx: signal %d code %d at %s address: caught\n",
				(unsigned long long)offsets[i], (int)probe_signal, (int)probe_code,
				probe_address == base + offsets[i] ? "the" : "ANOTHER");
	}
	sigaction(SIGSEGV, &old_segv, NULL);
	sigaction(SIGBUS, &old_bus, NULL);
	munmap(base, (size_t)size);
}

void host_vm_probe(void)
{
	vm_address_t address = 0;
	kern_return_t result;

	fprintf(stderr, "vm probe: start\n");
	report_size("mmap (wasm2c's guard)", 8 * GIB, 0);
	report_size("mmap", 6 * GIB, 0);
	report_size("mmap", 5 * GIB, 0);
	report_size("mmap", 4 * GIB + 256 * MIB, 0);
	report_size("mmap", 4 * GIB + 64 * MIB, 0);
	report_size("mmap", 4 * GIB, 0);
	report_size("mmap", 3 * GIB, 0);
	report_size("mmap (the TV's today)", 0xA0000000ull, 0);
	report_size("mmap MAP_NORESERVE", 8 * GIB, MAP_NORESERVE);
	report_size("mmap MAP_NORESERVE", 4 * GIB + 64 * MIB, MAP_NORESERVE);
	result = vm_allocate(mach_task_self(), &address, 8 * GIB, VM_FLAGS_ANYWHERE);
	fprintf(stderr, "vm probe: vm_allocate ANYWHERE 8 GiB: %s (%d)\n", result == KERN_SUCCESS ? "ok" : "FAILED", result);
	if (result == KERN_SUCCESS)
		vm_deallocate(mach_task_self(), address, 8 * GIB);
	address = 0;
	result = vm_allocate(mach_task_self(), &address, 4 * GIB + 64 * MIB, VM_FLAGS_ANYWHERE);
	fprintf(stderr, "vm probe: vm_allocate ANYWHERE 4 GiB + 64 MiB: %s (%d)\n",
		result == KERN_SUCCESS ? "ok" : "FAILED", result);
	if (result == KERN_SUCCESS)
		vm_deallocate(mach_task_self(), address, 4 * GIB + 64 * MIB);
	fprintf(stderr, "vm probe: largest single reservation %.2f GiB; all at once %.2f GiB\n",
		(double)largest() / GIB, (double)total() / GIB);
	guard_layout(64 * MIB);
	fprintf(stderr, "vm probe: done\n");
	fflush(stderr);
}
