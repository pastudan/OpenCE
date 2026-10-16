/*
GUEST_MAIN.C

The macOS build's guest entry point (port/macos/README.md), and what the
guest provides the host.

main runs on the process's main thread (port/macos/host/host_main.c). It
makes the data folder the current directory (wasi-libc starts at "/"; the
host names the folder in HALO_DATA_ROOT), then starts the game
(shell_xbox.c's main, renamed halo_game_main by tools/macos_build.py).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int halo_game_main(void);

/* ---------- allocations for the host (host_guest_malloc, host.h) */

__attribute__((export_name("halo_guest_malloc"))) void *halo_guest_malloc(unsigned long size)
{
	return malloc(size);
}

__attribute__((export_name("halo_guest_free"))) void halo_guest_free(void *memory)
{
	free(memory);
}

/* ---------- MSVC (scenario.c): a compiler barrier, an intrinsic clang knows
only on x86 and ARM */

void _ReadWriteBarrier(void)
{
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
}

/* ---------- start */

int main(int argc, char **argv)
{
	const char *root = getenv("HALO_DATA_ROOT");

	(void)argc;
	(void)argv;
	if (root && *root && chdir(root) != 0)
		fprintf(stderr, "cannot enter the data folder %s\n", root);
	return halo_game_main();
}
