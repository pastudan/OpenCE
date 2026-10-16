/*
HALO_W2C.H

The translated game, as the host sees it: wasm2c's header
(build/macos/w2c/halo.h), with its runtime's (wasm-rt.h).
*/

#ifndef __HALO_MACOS_W2C_H
#define __HALO_MACOS_W2C_H

#include "halo.h"

/* (host_main.c: the runtime calls it for a trap, WASM_RT_TRAP_HANDLER) */
void host_trap(wasm_rt_trap_t code);

/* memory.atomic.wait32/64 and notify (port/macos/tools' wasm2c patch) */
u32 wasm_rt_atomic_wait32(void *address, u32 expected, s64 timeout);
u32 wasm_rt_atomic_wait64(void *address, u64 expected, s64 timeout);
u32 wasm_rt_atomic_notify(void *address, u32 count);

#endif
