/*
 * overrides.c - replacing a guest function with a host one.
 *
 * Not everything a Lindbergh game calls its libraries for arrives through the
 * PLT. libXxf86vm is statically linked INTO this binary, so XF86VidMode* are
 * lifted code, not imports - and the lifted code talks the X protocol down a
 * socket that does not exist here.
 *
 * Every guest call already goes through dispatch(), so an override is just a
 * lookup there: the address of a guest function, and a host body to run
 * instead. The stack convention is the one dispatch already uses for an
 * indirect PLT hit - on entry esp points at the return address with arguments
 * above it, so consuming the return slot lines them up for a handler and
 * returning hands control back exactly as the guest's own `ret` would.
 *
 * The check is on the hot path, so it is a range test first: overrides cluster
 * in one small span of the image, and two comparisons reject every other
 * address before any scanning happens.
 */

#include <stdio.h>
#include <string.h>

#include "lindbergh_rt.h"
#include "recomp_symbols.h"

#define MAX_OVERRIDES 64

typedef struct { uint32_t va; HleHandler fn; const char *name; } Override;

static Override g_ov[MAX_OVERRIDES];
static int      g_ov_n;
static uint32_t g_ov_lo = 0xFFFFFFFFu, g_ov_hi;

uint32_t guest_symbol(const char *name)
{
#define SYM(n, a) if (strcmp(name, n) == 0) return a;
    GUEST_SYMBOLS(SYM)
#undef SYM
    return 0;
}

int guest_override(const char *name, HleHandler fn)
{
    uint32_t va = guest_symbol(name);
    if (!va) return 0;                       /* this game does not have it */
    if (g_ov_n >= MAX_OVERRIDES) {
        fprintf(stderr, "[override] table full, %s not installed\n", name);
        return 0;
    }
    g_ov[g_ov_n].va = va;
    g_ov[g_ov_n].fn = fn;
    g_ov[g_ov_n].name = name;
    g_ov_n++;
    if (va < g_ov_lo) g_ov_lo = va;
    if (va > g_ov_hi) g_ov_hi = va;
    return 1;
}

HleHandler guest_find_override(uint32_t va)
{
    if (va < g_ov_lo || va > g_ov_hi) return NULL;
    for (int i = 0; i < g_ov_n; i++)
        if (g_ov[i].va == va) return g_ov[i].fn;
    return NULL;
}

uint32_t guest_override_lo(void) { return g_ov_lo; }
uint32_t guest_override_hi(void) { return g_ov_hi; }
