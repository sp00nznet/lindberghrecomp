/*
 * hle_vidmode.c - XFree86-VidModeExtension, answered rather than spoken.
 *
 * Let's Go Jungle will not build its window without this. sWinProc_MakeWindow
 * probes for the extension, and when libXext reports it missing the whole of
 * _sScene::initializeGraph gives up - which skips _sScene::initialize, which
 * allocates the scene database, which is why a lookup in that database faulted
 * on a null map three layers further on. One missing extension, three layers
 * down, and nothing in between said a word.
 *
 * The extension exists to take over a real display: enumerate its modes,
 * switch to one, park the viewport. A cabinet needs that. A window does not -
 * ours is already the right size and nobody else is using the screen.
 *
 * These are overrides rather than imports because libXxf86vm is statically
 * linked into the game, so the XF86VidMode entry points are lifted code. Left
 * to run, they would marshal X protocol down a socket that does not exist.
 */

#include <stdio.h>
#include <string.h>

#include "lindbergh_rt.h"

/* XF86VidModeModeLine on i386: nine unsigned shorts, a gap the compiler
 * inserts to realign, then three 32-bit fields. 32 bytes. */
#define ML_hdisplay    0
#define ML_hsyncstart  2
#define ML_hsyncend    4
#define ML_htotal      6
#define ML_hskew       8
#define ML_vdisplay   10
#define ML_vsyncstart 12
#define ML_vsyncend   14
#define ML_vtotal     16
#define ML_flags      20
#define ML_privsize   24
#define ML_private    28

/* XF86VidModeModeInfo is the same with a dotclock in front. 36 bytes. */
#define MI_dotclock  0
#define MI_body      4
#define MI_flags    24
#define MI_privsize 28
#define MI_private  32

#define DOTCLOCK 85500        /* kHz, a real 1360x768 60 Hz pixel clock */

static void fill_modeline(uint32_t p, int w, int h, int base)
{
    /* Timings such a mode would really have. Nothing reads them back for
     * accuracy, but a mode whose total is smaller than its display is the kind
     * of nonsense a sanity check would catch. */
    wr16(p + base + ML_hdisplay,   (uint16_t)w);
    wr16(p + base + ML_hsyncstart, (uint16_t)(w + 64));
    wr16(p + base + ML_hsyncend,   (uint16_t)(w + 176));
    wr16(p + base + ML_htotal,     (uint16_t)(w + 256));
    wr16(p + base + ML_hskew,      0);
    wr16(p + base + ML_vdisplay,   (uint16_t)h);
    wr16(p + base + ML_vsyncstart, (uint16_t)(h + 3));
    wr16(p + base + ML_vsyncend,   (uint16_t)(h + 9));
    wr16(p + base + ML_vtotal,     (uint16_t)(h + 22));
}

static void h_QueryExtension(CPU *c)
{
    /* True, plus the event and error bases the server would have allocated.
     * The game stores them and compares arriving events against them; no
     * events ever arrive, so any consistent pair will do. */
    if (A32(1)) wr32(A32(1), 90);
    if (A32(2)) wr32(A32(2), 150);
    fprintf(stderr, "[vidmode] extension reported present\n");
    RET(1);
}

static void h_QueryVersion(CPU *c)
{
    if (A32(1)) wr32(A32(1), 2);
    if (A32(2)) wr32(A32(2), 2);
    RET(1);
}

static void h_GetModeLine(CPU *c)
{
    int w, h;
    lindbergh_window_size(&w, &h);
    if (A32(2)) wr32(A32(2), DOTCLOCK);
    if (A32(3)) {
        uint32_t p = A32(3);
        memset((void *)(uintptr_t)p, 0, 32);
        fill_modeline(p, w, h, 0);
    }
    RET(1);
}

/* One mode, and it is the window. What the game receives is a vector of
 * POINTERS to mode info, hence two objects: the mode, and a one-entry table
 * pointing at it. Both are static and outlive the process's interest, so the
 * XFree the game eventually calls on the vector does nothing - which is
 * exactly what this runtime's XFree already does. */
static unsigned char g_mode[36];
static uint32_t      g_mode_list[1];

static void h_GetAllModeLines(CPU *c)
{
    int w, h;
    lindbergh_window_size(&w, &h);

    memset(g_mode, 0, sizeof g_mode);
    uint32_t m = (uint32_t)(uintptr_t)g_mode;
    wr32(m + MI_dotclock, DOTCLOCK);
    fill_modeline(m, w, h, MI_body);

    g_mode_list[0] = m;
    if (A32(2)) wr32(A32(2), 1);
    if (A32(3)) wr32(A32(3), (uint32_t)(uintptr_t)g_mode_list);
    fprintf(stderr, "[vidmode] reporting one mode: %dx%d\n", w, h);
    RET(1);
}

static void h_GetViewPort(CPU *c)
{
    if (A32(2)) wr32(A32(2), 0);
    if (A32(3)) wr32(A32(3), 0);
    RET(1);
}

/* Switching modes, locking the switch and parking the viewport all describe
 * arrangements a window already has. Accept them. */
static void h_ok(CPU *c) { RET(1); }

void hle_register_vidmode(void)
{
    int n = 0;
    n += guest_override("XF86VidModeQueryExtension", h_QueryExtension);
    n += guest_override("XF86VidModeQueryVersion", h_QueryVersion);
    n += guest_override("XF86VidModeSetClientVersion", h_ok);
    n += guest_override("XF86VidModeGetModeLine", h_GetModeLine);
    n += guest_override("XF86VidModeGetAllModeLines", h_GetAllModeLines);
    n += guest_override("XF86VidModeSwitchToMode", h_ok);
    n += guest_override("XF86VidModeSwitchMode", h_ok);
    n += guest_override("XF86VidModeSetViewPort", h_ok);
    n += guest_override("XF86VidModeGetViewPort", h_GetViewPort);
    n += guest_override("XF86VidModeLockModeSwitch", h_ok);
    n += guest_override("XF86VidModeGetPermissions", h_ok);
    fprintf(stderr, "[vidmode] %d entry points overridden\n", n);
}
