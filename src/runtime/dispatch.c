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
#include <string.h>
#include <stdlib.h>

#include "lindbergh_rt.h"
#include "recomp_funcs_list.h"
#include "recomp_plt.h"

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

/* PLT stubs, address -> import. In a non-PIE ELF the address OF an imported
 * function IS its PLT stub, so `mov eax, offset memalign` puts 0x080727d0 in
 * a register and every later indirect call through that pointer arrives here
 * as a plain address rather than as a `call <stub>` the lifter could rewrite.
 * Function pointers, vtable slots, callback tables installed at startup - they
 * all land in this table instead of in the lifted one. */
typedef struct { uint32_t va; HleId id; } PltEntry;

#define PLT_ENT(a, id) { (a), (id) },
static const PltEntry g_plt[] = { HLE_PLT(PLT_ENT) };
#undef PLT_ENT

#define NPLT (sizeof g_plt / sizeof g_plt[0])

static int find_plt(uint32_t va, HleId *out)
{
    size_t lo = 0, hi = NPLT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_plt[mid].va == va) { *out = g_plt[mid].id; return 1; }
        if (g_plt[mid].va < va) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* Guest call depth, per thread.
 *
 * A guest call becomes a host call: dispatch() invokes a lifted C function
 * which calls dispatch() again. So guest recursion costs host stack, and a
 * runaway one exhausts it. That failure is worth catching deliberately,
 * because Windows reports stack exhaustion inconsistently - sometimes
 * STATUS_STACK_OVERFLOW, sometimes a cookie check that ends in __fastfail,
 * which no handler can see. Counting is cheaper than diagnosing afterwards. */
#ifdef _MSC_VER
static __declspec(thread) unsigned g_depth;
static __declspec(thread) unsigned g_depth_max;
#else
static __thread unsigned g_depth;
static __thread unsigned g_depth_max;
#endif

#define DEPTH_LIMIT 20000

unsigned guest_depth(void)     { return g_depth; }
unsigned guest_depth_max(void) { return g_depth_max; }

/* Did execution ever reach THIS function? Set LINDBERGH_WATCH to a comma-
 * separated list of hex addresses and each one reports the first time it is
 * dispatched. The question comes up constantly during bring-up - a branch went
 * the wrong way and something that should have run did not - and it is not
 * answerable from a backtrace, which only shows what DID run. */
static void watch_check(uint32_t va)
{
    static uint32_t list[16];
    static int n = -1;
    static unsigned char hit[16];
    if (n < 0) {
        n = 0;
        const char *v = getenv("LINDBERGH_WATCH");
        while (v && *v && n < 16) {
            list[n++] = (uint32_t)strtoul(v, (char **)&v, 16);
            while (*v == ',' || *v == ' ') v++;
        }
    }
    for (int i = 0; i < n; i++)
        if (list[i] == va && !hit[i]) {
            hit[i] = 1;
            fprintf(stderr, "[watch] reached %#010x\n", va);
            fflush(stderr);
        }
}

static int gl_token_call_entry(CPU *c, uint32_t va)
{
    uint32_t saved = c->esp;
    c->esp += 4;                       /* the return slot, as  would */
    if (gl_token_call(c, va)) return 1;
    c->esp = saved;
    return 0;
}

void dispatch(CPU *c, uint32_t va)
{
    guest_trace_dispatch(va);
    watch_check(va);

    if (++g_depth > g_depth_max) g_depth_max = g_depth;
    if (g_depth > DEPTH_LIMIT) {
        fprintf(stderr, "[dispatch] guest call depth %u at %#010x - runaway recursion\n",
                g_depth, va);
        guest_report_state("call depth limit");
        exit(43);
    }
    /* A GL entry point handed out by glXGetProcAddressARB. Same stack shape as
     * an indirect PLT hit: consume the return slot so the handler sees
     * argument 0 at esp+0. */
    if (gl_token_call_entry(c, va)) { g_depth--; return; }

    /* A host body standing in for a guest function. Checked first, so it wins
     * over the lifted version; the range test costs two compares. */
    HleHandler ov = guest_find_override(va);
    if (ov) { (void)pop32(c); ov(c); g_depth--; return; }

    void (*fn)(CPU *) = find(va);
    if (fn) { fn(c); g_depth--; return; }

    /* An import reached indirectly. The stack here is one slot off from a
     * `call <stub>` site: esp points at the return address with the arguments
     * above it, where a handler expects argument 0 at esp+0. Consuming the
     * return slot - which is all a lifted `ret` does - lines them up and
     * leaves esp where the caller's own `add esp, N` expects it. Getting this
     * wrong reads every argument shifted by four bytes. */
    HleId id;
    if (find_plt(va, &id)) {
        (void)pop32(c);
        hle_call(c, id);
        g_depth--;
        return;
    }

    /* Neither lifted nor an import: a function the bounds missed, or a jump
     * into the middle of one that did lift. Both are worth the address. */
    fprintf(stderr, "[dispatch] no lifted function at %#010x (called from esp=%#010x)\n",
            va, c->esp);
    abort();
}

/* Reverse of the PLT table: the stub address for a named import, or 0.
 *
 * This exists for glXGetProcAddressARB. The game asks the driver for an
 * extension entry point and then CALLS the pointer it gets back - as a guest
 * indirect call, through dispatch(). Handing it a real host function address
 * would be useless: dispatch would look that up in the lifted table, find
 * nothing, and abort.
 *
 * But the game also imports most of those extension functions through its own
 * PLT, so their stub addresses are already routable tokens with handlers
 * behind them. Returning the stub means the call goes back through the same
 * path as a direct one, with no extra machinery at all. */
uint32_t hle_plt_address(const char *name)
{
    for (size_t i = 0; i < NPLT; i++)
        if (strcmp(hle_name(g_plt[i].id), name) == 0)
            return g_plt[i].va;
    return 0;
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
