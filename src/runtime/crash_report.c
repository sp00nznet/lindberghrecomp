/*
 * crash_report.c - what the simulated machine was doing when it faulted.
 *
 * A debugger on a recompiled program shows you a C function named L_08411FF0
 * a million lines into a generated file, which is true and useless. What you
 * need is the guest's state: which lifted function was running, how it got
 * there, and what the registers held.
 *
 * pcrecomp has the equivalent for its global-register model in
 * runtime/recomp32/crash_report.c. That one reads g_eax and friends directly,
 * so it cannot serve the CPU-struct model - this is the same idea against a
 * `CPU *`. If a second CPU-struct target ever wants it, it should move up into
 * pcrecomp/runtime/recomp32_cpu/ beside cpu.h rather than be copied again.
 *
 * The handler reports and then declines the exception, so the process still
 * dies and a debugger still gets its turn. Nothing here allocates.
 */

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "lindbergh_rt.h"

/* The CPU currently executing guest code. Thread-local: every guest thread
 * runs on its own CPU struct, so a single global would report whichever thread
 * happened to start last rather than the one that faulted. */
#ifdef _MSC_VER
static __declspec(thread) CPU *g_current;
#else
static __thread CPU *g_current;
#endif

void guest_set_current_cpu(CPU *c) { g_current = c; }

/* Recent dispatches, newest last. Written on the hot path, so it is one store
 * and one masked increment and nothing else - no timestamps, no names. */
#define TRACE_N 64
static uint32_t g_trace[TRACE_N];
static unsigned g_trace_i;

/* Persist the ring to a file, because the failure being chased kills the
 * process with __fastfail and no handler runs - so an in-memory ring is lost
 * exactly when it is wanted. Written every RING_FLUSH dispatches, which costs
 * one small write per few thousand calls and nothing at all unless
 * LINDBERGH_TRACE_FILE names somewhere to put it. */
#define RING_FLUSH 2048

static FILE *g_ringf;
static unsigned g_since_flush;

static void ring_persist(void)
{
    if (!g_ringf) {
        const char *path = getenv("LINDBERGH_TRACE_FILE");
        if (!path || !*path) { g_since_flush = 0; return; }
        g_ringf = fopen(path, "wb");
        if (!g_ringf) return;
    }
    fseek(g_ringf, 0, SEEK_SET);
    fwrite(&g_trace_i, 4, 1, g_ringf);
    fwrite(g_trace, 4, TRACE_N, g_ringf);
    fflush(g_ringf);
}

void guest_trace_dispatch(uint32_t va)
{
    g_trace[g_trace_i++ & (TRACE_N - 1)] = va;
    if (++g_since_flush >= RING_FLUSH) {
        g_since_flush = 0;
        ring_persist();
    }
}

static const char *region(uint32_t va)
{
    if (va >= 0x08048000u && va < 0x08d00000u) return "image";
    if (va >= 0xbf000000u)                     return "stack";
    if (!va)                                   return "NULL";
    if (va < 0x1000u)                          return "near NULL";
    return "";
}

/* Walk the guest's frame pointers.
 *
 * The dispatch ring says what ran; it does not say who called whom, and for a
 * fault inside a leaf like _Rb_tree::find the caller is the entire question.
 * Lifted code keeps real frames - every function opens with push ebp / mov
 * ebp, esp - so the chain is the ordinary one: saved ebp at [ebp], return
 * address at [ebp+4].
 *
 * Addresses only, deliberately. The names live in the guest ELF's symbol
 * table, which this runtime has no reason to carry; tools/elf turns a column
 * of addresses into names in one pass, and that keeps the fault path free of
 * anything that could itself fail. */
static int readable(uint32_t va)
{
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof mbi)) {
        fprintf(stderr, "  (cannot query %#010x)\n", va);
        return 0;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        fprintf(stderr, "  (%#010x: state %#lx protect %#lx)\n",
                va, (unsigned long)mbi.State, (unsigned long)mbi.Protect);
        return 0;
    }
    return 1;
#else
    return va > 0x1000u;
#endif
}

void guest_backtrace(CPU *c)
{
    if (!c) return;
    fprintf(stderr, "guest backtrace (ebp chain):\n");

    uint32_t ebp = c->ebp;
    for (int depth = 0; depth < 32; depth++) {
        if (!readable(ebp) || !readable(ebp + 4)) break;
        uint32_t saved = rd32(ebp);
        uint32_t ret   = rd32(ebp + 4);
        if (!ret) break;
        fprintf(stderr, "  %2d  %#010x\n", depth, ret);
        if (saved <= ebp) break;          /* frames grow upward; anything else is junk */
        ebp = saved;
    }
    fflush(stderr);
}

void guest_report_state(const char *why)
{
    fprintf(stderr, "\n==== %s ====\n", why);

    if (g_current) {
        CPU *c = g_current;
        fprintf(stderr, "eax %08X  ecx %08X  edx %08X  ebx %08X\n",
                c->eax, c->ecx, c->edx, c->ebx);
        fprintf(stderr, "esp %08X  ebp %08X  esi %08X  edi %08X\n",
                c->esp, c->ebp, c->esi, c->edi);
        fprintf(stderr, "flags cf=%u zf=%u sf=%u of=%u pf=%u af=%u\n",
                c->cf, c->zf, c->sf, c->of, c->pf, c->af);
    } else {
        fprintf(stderr, "(no CPU registered)\n");
    }

    guest_backtrace(g_current);
    fprintf(stderr, "last %d dispatches, oldest first:\n", TRACE_N);
    unsigned n = g_trace_i < TRACE_N ? g_trace_i : TRACE_N;
    for (unsigned k = n; k > 0; k--) {
        uint32_t va = g_trace[(g_trace_i - k) & (TRACE_N - 1)];
        fprintf(stderr, "  %2u  %#010x %s\n", n - k, va, region(va));
    }
    fflush(stderr);
}

#ifdef _WIN32
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char why[160];

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uint32_t addr = (uint32_t)er->ExceptionInformation[1];
        snprintf(why, sizeof why, "access violation: %s %#010x %s",
                 er->ExceptionInformation[0] ? "write to" : "read from",
                 addr, region(addr));
    } else {
        snprintf(why, sizeof why, "exception %08lX", (unsigned long)er->ExceptionCode);
    }
    guest_report_state(why);
    return EXCEPTION_CONTINUE_SEARCH;      /* still crash; let a debugger see it */
}

void guest_install_crash_handler(void)
{
    AddVectoredExceptionHandler(1, on_exception);
}
#else
void guest_install_crash_handler(void) { }
#endif

/* An instruction the lifter could not translate, reached at run time.
 *
 * The lifter used to emit a bare abort() here, and that cost four wrong
 * diagnoses: MSVC's abort() calls __fastfail(FAST_FAIL_FATAL_APP_EXIT), which
 * raises 0xC0000409 - a status named STATUS_STACK_BUFFER_OVERRUN, past every
 * exception handler, with no message. So an unimplemented instruction looked
 * exactly like memory corruption, and the one thing it could not look like was
 * itself.
 *
 * Saying what it was, where it was, and how the program got there costs four
 * lines and removes the whole confusion. */
void lindbergh_todo(uint32_t va, const char *text)
{
    fprintf(stderr, "\n[recomp] unimplemented instruction at %#010x: %s\n", va, text);
    guest_report_state("unimplemented instruction");
    exit(44);
}
