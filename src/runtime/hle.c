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

#include "lindbergh_rt.h"

#define HLE_NAME(id, name) name,
static const char *const g_names[] = { HLE_IMPORTS(HLE_NAME) };
#undef HLE_NAME

const char *hle_name(HleId id)
{
    return (unsigned)id < HLE_COUNT ? g_names[id] : "<bad id>";
}

HleHandler g_hle_handlers[HLE_COUNT];

void hle_call(CPU *c, HleId id)
{
    if ((unsigned)id < HLE_COUNT && g_hle_handlers[id]) {
        g_hle_handlers[id](c);
        return;
    }
    fprintf(stderr,
            "[hle] %s() is not implemented.\n"
            "      Give it a body:  g_hle_handlers[%s] = my_%s;\n",
            hle_name(id), hle_name(id), hle_name(id));
    abort();
}
