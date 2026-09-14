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

void guest_trace_dispatch(uint32_t va)
{
    g_trace[g_trace_i++ & (TRACE_N - 1)] = va;
}

static const char *region(uint32_t va)
{
    if (va >= 0x08048000u && va < 0x08d00000u) return "image";
    if (va >= 0xbf000000u)                     return "stack";
    if (!va)                                   return "NULL";
    if (va < 0x1000u)                          return "near NULL";
    return "";
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
