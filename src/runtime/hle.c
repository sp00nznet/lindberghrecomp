/*
 * hle.c - the shared libraries the board provided.
 *
 * A Lindbergh game is dynamically linked: its calls to libc, libGL, libSDL,
 * ALSA and SEGA's own libsegaapi/libposixdrv all leave through the PLT, and
 * the lifter turns each of those into hle_call(c, HLE_<name>). There is no
 * ld.so here and no .so to load, so this is where those calls end up.
 *
 * An id with no implementation aborts naming itself. That is deliberate: a
 * silent no-op turns a missing import into a graphical glitch three hours
 * later, and the name printed here is the next thing to write.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lindbergh_rt.h"

#define HLE_NAME(id, name) name,
static const char *const g_names[] = { HLE_IMPORTS(HLE_NAME) };
#undef HLE_NAME

const char *hle_name(HleId id)
{
    return (unsigned)id < HLE_COUNT ? g_names[id] : "<bad id>";
}

HleHandler g_hle_handlers[HLE_COUNT];

int hle_bind(const char *name, HleHandler fn)
{
    for (unsigned i = 0; i < HLE_COUNT; i++)
        if (strcmp(g_names[i], name) == 0) { g_hle_handlers[i] = fn; return 1; }
    return 0;   /* this game does not import it - nothing to bind, not a fault */
}

void hle_register_all(void)
{
    hle_register_libc();
    hle_register_libc2();
    hle_register_io();
    hle_register_libc3();
    hle_register_pthread();
    hle_register_window();
    hle_register_vidmode();
    hle_register_matrix();
    hle_register_console();
    hle_register_sega();
    hle_register_glut();
    hle_register_cxx();
    hle_register_gl();
    hle_register_cg();
}

/* Survey mode. Aborting on the first unimplemented import is the right default
 * - it stops at the first place the game went somewhere we cannot follow - but
 * during bring-up it means one rebuild per import, and a rebuild here is 80
 * translation units. With LINDBERGH_HLE_PERMISSIVE=1 an unbound import instead
 * reports itself once, returns 0, and lets the guest carry on, so a single run
 * enumerates everything the startup path reaches. The game is wrong from the
 * first such call - this is for finding the work, not for playing. */
static int permissive(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LINDBERGH_HLE_PERMISSIVE");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

static unsigned char *g_seen;

/* Name every import as it is entered. Heavier than the survey - one line per
 * call, not per distinct import - but it is the only way to see what was
 * running when the process dies in a way no handler can catch, which
 * __fastfail (STATUS_STACK_BUFFER_OVERRUN) does by design. */
static int tracing(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("LINDBERGH_HLE_TRACE");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

/* Stop deliberately at the Nth import call and report, because the failure
 * this was written for kills the process with __fastfail - which bypasses
 * every exception handler by design, so the crash reporter never runs. When a
 * failure is deterministic, arriving just before it is as good as catching it.
 *   LINDBERGH_HLE_BREAK=46211 */
static void maybe_break(CPU *c, HleId id)
{
    static long counter, at = -1;
    if (at < 0) {
        const char *v = getenv("LINDBERGH_HLE_BREAK");
        at = (v && *v) ? atol(v) : 0;
    }
    if (!at) return;
    if (++counter >= at) {
        fprintf(stderr, "[break] import call #%ld is %s\n", counter, hle_name(id));
        guest_report_state("deliberate break");
        exit(42);
    }
}

void hle_call(CPU *c, HleId id)
{
    maybe_break(c, id);
    if (tracing()) {
        fprintf(stderr, "[call] %s\n", hle_name(id));
        fflush(stderr);
    }
    if ((unsigned)id < HLE_COUNT && g_hle_handlers[id]) {
        g_hle_handlers[id](c);
        return;
    }
    if (permissive()) {
        if (!g_seen) g_seen = (unsigned char *)calloc(HLE_COUNT, 1);
        if (g_seen && !g_seen[id]) {
            g_seen[id] = 1;
            fprintf(stderr, "[hle] %s\n", hle_name(id));
            fflush(stderr);
        }
        c->eax = 0;
        return;
    }
    fprintf(stderr,
            "[hle] %s() is not implemented.\n"
            "      Bind it:  hle_bind(\"%s\", my_%s);\n"
            "      Or set LINDBERGH_HLE_PERMISSIVE=1 to list everything the\n"
            "      startup path wants in one run.\n",
            hle_name(id), hle_name(id), hle_name(id));
    abort();
}
