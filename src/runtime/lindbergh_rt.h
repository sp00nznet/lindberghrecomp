/*
 * lindbergh_rt.h - runtime for statically recompiled Lindbergh games.
 *
 * The CPU model is pcrecomp's (pcrecomp/runtime/recomp32_cpu/cpu.h): an
 * explicit `CPU *c` threaded through every lifted function, flat memory where
 * a register holds a real 32-bit host address. That means the host must be a
 * 32-bit build - the game's segments are mapped at the virtual addresses it
 * was linked for, and on Linux/i386 those are down at 0x08048000.
 *
 * What this header adds is the Lindbergh side of the boundary: a Linux/i386
 * process to be, a kernel to call, and a set of shared libraries the board
 * provided that we have to provide instead.
 */
#ifndef LINDBERGH_RT_H
#define LINDBERGH_RT_H

/* Generated code calls abort() wherever an instruction did not lift, so the
 * declaration has to travel with the header the generated code includes. */
#include <stdlib.h>

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- guest process ---- */

/* Map an ELF's PT_LOADs at their original virtual addresses and build the
 * initial stack (argc/argv/envp/auxv) the way the kernel does, so the game's
 * _start finds what it expects. Returns 0, or -1 with the reason on stderr. */
int  guest_load(const char *elf_path, int argc, char **argv);

/* Entry point VA from the ELF header, valid after guest_load. */
uint32_t guest_entry(void);

/* Initialise a CPU to enter the guest: esp at the built stack, everything
 * else zero, exactly as Linux hands a fresh process to _start. */
void guest_init_cpu(CPU *c);

/* Guest break, for the brk/sbrk syscalls. */
uint32_t guest_brk(uint32_t newbrk);

/* ---- kernel ---- */

/* `int 0x80` and the vDSO's sysenter both land here. Number in eax, arguments
 * in ebx/ecx/edx/esi/edi/ebp, result back into eax (negative errno on
 * failure), per the Linux i386 calling convention. */
void linux_syscall(CPU *c);

/* ---- shared libraries ---- */

/* Every PLT import in the game gets an HLE_* id; recomp_imports.h is
 * generated from the ELF and lists them. */
#include "recomp_imports.h"

#define HLE_ENUM(id, name) id,
typedef enum { HLE_IMPORTS(HLE_ENUM) HLE_COUNT } HleId;
#undef HLE_ENUM

/* A library function's body. Reads its arguments off the guest stack and
 * leaves the return value in eax, the cdecl the ELF was built for. */
typedef void (*HleHandler)(CPU *c);

/* One slot per import. A game project fills in the ones it needs; the toolkit
 * ships the ones every title shares. A slot left null aborts naming itself,
 * which is the to-do list. */
extern HleHandler g_hle_handlers[];

void hle_call(CPU *c, HleId id);

/* Give a named import a body. Binding by name and not by HLE_* id is what
 * keeps a handler file title-agnostic: HLE_memcpy only exists as an enumerator
 * if *this* game imports memcpy, so a file that said `g_hle_handlers[HLE_memcpy]`
 * would fail to compile against a game that does not. Returns 0 when the game
 * does not import that name, which is not an error - most do not import most
 * of them. */
int hle_bind(const char *name, HleHandler fn);

/* Bind every handler the toolkit ships. A game project calls this once, then
 * binds its own on top. */
void hle_register_all(void);
void hle_register_libc(void);

/* ---- the guest side of a call ----
 *
 * The lifter turns `call <plt stub>` into hle_call() and pushes no return
 * address, so on entry esp points straight at argument 0. cdecl, so the
 * handler must not move esp - the caller's own `add esp, N` does that.
 */
#define A32(n)   rd32(c->esp + 4u * (unsigned)(n))
#define APTR(n)  ((void *)(uintptr_t)A32(n))
#define ASTR(n)  ((char *)(uintptr_t)A32(n))
#define AI32(n)  ((int32_t)A32(n))
#define RET(v)   (c->eax = (uint32_t)(uintptr_t)(v))

/* A function returning float or double returns it in st(0) on i386 SysV, not
 * in an XMM register - the caller does `fstp` to take it. Every math import
 * below goes back this way. */
#define RETF(v)  fpush(c, (double)(v))

/* Name for an id, for diagnostics. */
const char *hle_name(HleId id);

/* ---- lifted code ---- */

/* Address -> lifted function. Generated: recomp_funcs_list.h is the X-macro
 * of every VA that lifted, and recomp_dispatch.c turns it into a table. */
void dispatch(CPU *c, uint32_t va);
void dispatch_jmp(CPU *c, uint32_t va);

#ifdef __cplusplus
}
#endif
#endif /* LINDBERGH_RT_H */
