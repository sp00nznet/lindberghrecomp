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
