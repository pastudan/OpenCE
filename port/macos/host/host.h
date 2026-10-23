/*
HOST.H

The macOS build's host (port/macos/README.md): the native program that runs
the game. The game is WebAssembly (wasm32, the 32-bit pointers its data
needs) translated to C by wasm2c (build/macos/w2c/halo*.c) and compiled for
arm64 with everything else here.

The guest's memory is one block (guest_memory, made by host_main.c): a
guest pointer is an offset into it. HOST_POINTER makes a host pointer of a
guest one, and HOST_OFFSET the reverse.
*/

#ifndef __HALO_MACOS_HOST_H
#define __HALO_MACOS_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* the guest's memory: its first byte, and its size */
extern uint8_t *host_memory_base;
extern uint64_t host_memory_size;

#define HOST_POINTER(offset) ((offset) ? (void *)(host_memory_base + (uint32_t)(offset)) : NULL)
#define HOST_OFFSET(pointer) ((pointer) ? (uint32_t)((const uint8_t *)(pointer) - host_memory_base) : 0u)

/* a guest allocation (its malloc, called from the host: the game's
exported halo_guest_malloc), for what SDL hands back that the guest frees
with SDL_free (strings, lists); 0 if it cannot */
uint32_t host_guest_malloc(uint32_t size);
void host_guest_free(uint32_t offset);
/* a copy of a host string in guest memory, or 0 */
uint32_t host_guest_strdup(const char *text);

/* HALO_VM_PROBE=1: report the address-space reservations allowed, then exit
(host_vm_probe.c) */
void host_vm_probe(void);

/* logging (stderr, and the log file the game also writes beside its data) */
void host_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* the guest's errno after a host call (posix_*.c set the host's): the
generated wrappers copy it (host_imports.c) */
void host_errno_set_from_host(void);

#endif
