/*
 * dispatch.c - original address -> lifted function.
 *
 * Direct calls could have been emitted as plain C calls, but indirect ones
 * cannot: the game computes a target at run time (vtables, callback tables,
 * the task dispatchers arcade code is full of) and hands over an address from
 * the original image. Every call goes through here so both kinds land the
 * same way.
 *
 * The table is generated - recomp_funcs_list.h is the X-macro of every VA
 * that lifted, emitted in ascending order, which is what lets this binary
 * search instead of scanning a few thousand entries per indirect call.
 */

#include <stdio.h>
#include <stdlib.h>

#include "lindbergh_rt.h"
#include "recomp_funcs_list.h"

#define DECL(a) void L_##a(CPU *c);
LIFTED_FUNCS(DECL)
#undef DECL

typedef struct { uint32_t va; void (*fn)(CPU *); } Entry;

#define ENT(a) { 0x##a##u, L_##a },
static const Entry g_table[] = { LIFTED_FUNCS(ENT) };
#undef ENT

#define N (sizeof g_table / sizeof g_table[0])

static void (*find(uint32_t va))(CPU *)
{
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_table[mid].va == va) return g_table[mid].fn;
        if (g_table[mid].va < va) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

void dispatch(CPU *c, uint32_t va)
{
    void (*fn)(CPU *) = find(va);
    if (fn) { fn(c); return; }
    /* Not lifted. Almost always one of: a function the bounds file missed, a
     * jump into the middle of one that did lift, or a PLT stub the ELF's
     * .rel.plt did not name. All three are worth seeing the address of. */
    fprintf(stderr, "[dispatch] no lifted function at %#010x (called from esp=%#010x)\n",
            va, c->esp);
    abort();
}

void dispatch_jmp(CPU *c, uint32_t va)
{
    /* A tail call: the caller already popped its frame, so the callee returns
     * straight to our caller's caller. Same table, no return address pushed. */
    dispatch(c, va);
}

/* ---- calling back into guest code ----
 *
 * Needed whenever the host has to run something the game owns: the CRT's
 * constructors, a thread's start routine, a qsort comparator, an atexit
 * handler, a GL callback. The shape is the one a lifted function expects on
 * entry - arguments pushed right to left, then a return slot for its own `ret`
 * to pop - and cdecl says the caller cleans up, which is what restoring esp
 * afterwards does.
 *
 * The return address pushed is deliberately a value no code maps to. A lifted
 * `ret` only does `esp += 4; return;` and never jumps to it, so it is never
 * read; if it ever shows up in a diagnostic, something has gone wrong in a way
 * worth noticing.
 */
uint32_t guest_call(CPU *c, uint32_t fn, const uint32_t *args, int nargs)
{
    uint32_t saved = c->esp;
    for (int i = nargs - 1; i >= 0; i--)
        push32(c, args[i]);
    push32(c, 0xDEADBEEFu);
    dispatch(c, fn);
    c->esp = saved;
    return c->eax;
}
