/*
 * hle_window.c - the display, cut at the right seam.
 *
 * A Lindbergh game is an X11/GLX program: it opens a Display, picks a visual,
 * creates a Window, makes a GL context and swaps buffers. The temptation is to
 * implement Xlib. Do not. Reimplementing fifty X protocol calls on Windows so
 * that they can hand a GLX context to WGL is a great deal of work to arrive
 * back where you started, and every one of those calls would exist only to be
 * translated away again.
 *
 * The game never looks inside a Display, a Window or an XVisualInfo - it
 * receives them from Xlib and hands them back to Xlib. That makes them opaque
 * tokens, and opaque tokens can be anything. So: one real Win32 window with a
 * real WGL context, handed out behind whatever shapes the game expects. Fifty
 * X11 imports and eighteen GLX ones collapse into this file, and most of them
 * are no-ops because the thing they were arranging has already been arranged.
 *
 * What is NOT faked: the GL context is real, glXGetProcAddressARB resolves
 * real entry points out of the host's driver, and glXSwapBuffers really
 * presents. The game draws with actual OpenGL onto actual hardware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>

#include "lindbergh_rt.h"

/* Lindbergh Yellow ran Let's Go Jungle at 1360x768. Overridable because a
 * desktop is not a cabinet and the game does not care. */
#define DEFAULT_W 1360
#define DEFAULT_H 768

/* The tokens handed back to the guest. Any non-zero value would do; these are
 * distinctive on sight in a crash dump, which is the only requirement. */
#define TOK_DISPLAY 0x58440001u      /* "XD" */
#define TOK_WINDOW  0x58570001u      /* "XW" */
#define TOK_COLORMAP 0x58430001u     /* "XC" */

static struct {
    HWND   hwnd;
    HDC    hdc;
    HGLRC  hglrc;
    HMODULE gl;
    int    w, h;
    int    quit;
} g_win;

static int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : dflt;
}

/* The window's size, for anything that has to describe the display to the
 * game rather than draw on it - hle_vidmode reports exactly this as the one
 * available video mode. */
void lindbergh_window_size(int *w, int *h)
{
    if (w) *w = g_win.w ? g_win.w : env_int("LINDBERGH_WIDTH", DEFAULT_W);
    if (h) *h = g_win.h ? g_win.h : env_int("LINDBERGH_HEIGHT", DEFAULT_H);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        g_win.quit = 1;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    /* The game has its own idea of the cursor and its own aspect ratio; do not
     * let Windows repaint over it or resize it out from under it. */
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* Pump the host's message queue. Called from the places the game would have
 * talked to the X server - XPending, XSync, XFlush, glXSwapBuffers - because a
 * Win32 window that never pumps stops responding and Windows greys it out. */
static void pump(void)
{
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    if (g_win.quit) {
        fprintf(stderr, "[window] closed\n");
        exit(0);
    }
}

static int make_window(void)
{
    if (g_win.hwnd) return 1;

    g_win.w = env_int("LINDBERGH_WIDTH", DEFAULT_W);
    g_win.h = env_int("LINDBERGH_HEIGHT", DEFAULT_H);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.style         = CS_OWNDC;          /* the GL context keeps its own DC */
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "lindberghrecomp";
    if (!RegisterClassA(&wc)) return 0;

    RECT r = { 0, 0, g_win.w, g_win.h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_win.hwnd = CreateWindowExA(0, "lindberghrecomp", "Let's Go Jungle",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top,
                                 NULL, NULL, wc.hInstance, NULL);
    if (!g_win.hwnd) return 0;
    g_win.hdc = GetDC(g_win.hwnd);
    fprintf(stderr, "[window] %dx%d\n", g_win.w, g_win.h);
    return 1;
}

/* ---- GLX ----
 *
 * XVisualInfo, as the guest's Xlib headers lay it out on i386. The game reads
 * `depth` and passes the rest straight back, so only the size and that one
 * field have to be right. */
static struct {
    uint32_t visual;
    uint32_t visualid;
    int32_t  screen;
    int32_t  depth;
    int32_t  class_;
    uint32_t red_mask, green_mask, blue_mask;
    int32_t  colormap_size;
    int32_t  bits_per_rgb;
} g_visual = { 0, 0x21, 0, 24, 4, 0xFF0000, 0x00FF00, 0x0000FF, 256, 8 };

/* ---- the Display, which is not opaque ----
 *
 * The first version of this file handed the game a token for Display * on the
 * grounds that it only ever passes the thing back to Xlib. That is wrong, and
 * the game said so immediately: it faulted reading 0x58440085, which is the
 * token plus 0x84.
 *
 * struct _XDisplay is PUBLIC in Xlib.h. DefaultScreen(dpy), RootWindow(dpy,s),
 * DisplayWidth(dpy,s) and their friends are macros, compiled into the game as
 * direct loads, and offset 132 is default_screen - exactly the field that
 * faulted. Xlib's opaque types are Visual and GC; Display and Screen are laid
 * out in the header for those macros to read.
 *
 * So the shapes have to be real. These are the i386 layouts, and only the
 * fields something actually reads are filled in - the rest stay zero, which is
 * what a freshly connected display would look like anyway. */

#define DPY_SIZE 512          /* struct _XDisplay is ~180 bytes on i386 */
#define SCR_SIZE 128          /* Screen is 80 */
#define VIS_SIZE 64           /* Visual is 32 */

/* struct _XDisplay, byte offsets on i386 */
#define DPY_proto_major      16
#define DPY_proto_minor      20
#define DPY_vendor           24
#define DPY_byte_order       48
#define DPY_bitmap_unit      52
#define DPY_bitmap_pad       56
#define DPY_nformats         64
#define DPY_vnumber          72
#define DPY_release          76
#define DPY_qlen             88
#define DPY_max_request_size 116
#define DPY_display_name     128
#define DPY_default_screen   132
#define DPY_nscreens         136
#define DPY_screens          140

/* Screen, byte offsets on i386 */
#define SCR_display      4
#define SCR_root         8
#define SCR_width       12
#define SCR_height      16
#define SCR_mwidth      20
#define SCR_mheight     24
#define SCR_root_depth  36
#define SCR_root_visual 40
#define SCR_cmap        48
#define SCR_white_pixel 52
#define SCR_black_pixel 56

/* Visual, byte offsets on i386 */
#define VIS_visualid     4
#define VIS_class        8
#define VIS_red_mask    12
#define VIS_green_mask  16
#define VIS_blue_mask   20
#define VIS_bits_per_rgb 24
#define VIS_map_entries  28

static unsigned char g_dpy[DPY_SIZE];
static unsigned char g_scr[SCR_SIZE];
static unsigned char g_vis[VIS_SIZE];

static void put32(unsigned char *base, unsigned off, uint32_t v)
{
    memcpy(base + off, &v, 4);
}

static uint32_t build_display(void)
{
    static int done;
    if (done) return (uint32_t)(uintptr_t)g_dpy;
    done = 1;

    uint32_t dpy = (uint32_t)(uintptr_t)g_dpy;
    uint32_t scr = (uint32_t)(uintptr_t)g_scr;
    uint32_t vis = (uint32_t)(uintptr_t)g_vis;

    memset(g_dpy, 0, sizeof g_dpy);
    memset(g_scr, 0, sizeof g_scr);
    memset(g_vis, 0, sizeof g_vis);

    put32(g_vis, VIS_visualid, 0x21);
    put32(g_vis, VIS_class, 4);                  /* TrueColor */
    put32(g_vis, VIS_red_mask,   0x00FF0000);
    put32(g_vis, VIS_green_mask, 0x0000FF00);
    put32(g_vis, VIS_blue_mask,  0x000000FF);
    put32(g_vis, VIS_bits_per_rgb, 8);
    put32(g_vis, VIS_map_entries, 256);

    put32(g_scr, SCR_display, dpy);
    put32(g_scr, SCR_root, TOK_WINDOW);
    put32(g_scr, SCR_width,  (uint32_t)g_win.w);
    put32(g_scr, SCR_height, (uint32_t)g_win.h);
    put32(g_scr, SCR_mwidth,  (uint32_t)(g_win.w / 4));   /* millimetres, ~96 dpi */
    put32(g_scr, SCR_mheight, (uint32_t)(g_win.h / 4));
    put32(g_scr, SCR_root_depth, 24);
    put32(g_scr, SCR_root_visual, vis);
    put32(g_scr, SCR_cmap, TOK_COLORMAP);
    put32(g_scr, SCR_white_pixel, 0x00FFFFFF);
    put32(g_scr, SCR_black_pixel, 0);

    put32(g_dpy, DPY_proto_major, 11);
    put32(g_dpy, DPY_proto_minor, 0);
    put32(g_dpy, DPY_vendor, (uint32_t)(uintptr_t)"lindberghrecomp");
    put32(g_dpy, DPY_byte_order, 0);             /* LSBFirst */
    put32(g_dpy, DPY_bitmap_unit, 32);
    put32(g_dpy, DPY_bitmap_pad, 32);
    put32(g_dpy, DPY_nformats, 0);
    put32(g_dpy, DPY_vnumber, 11);
    put32(g_dpy, DPY_release, 0);
    put32(g_dpy, DPY_qlen, 0);
    put32(g_dpy, DPY_max_request_size, 65535);
    put32(g_dpy, DPY_display_name, (uint32_t)(uintptr_t)":0.0");
    put32(g_dpy, DPY_default_screen, 0);
    put32(g_dpy, DPY_nscreens, 1);
    put32(g_dpy, DPY_screens, scr);

    /* The visual the game gets from glXChooseVisual has to name the same
     * Visual the Screen does, or its colormap will not match its window. */
    g_visual.visual = vis;
    g_visual.visualid = 0x21;
    return dpy;
}


static void h_glXChooseVisual(CPU *c)
{
    if (A32(2)) {
        fprintf(stderr, "[glX] chooseVisual attribs:");
        for (int i = 0; i < 40; i++) {
            uint32_t v = rd32(A32(2) + 4u * i);
            if (v == 0) break;
            fprintf(stderr, " %u", v);
        }
        fprintf(stderr, "\n"); fflush(stderr);
    }
    /* The attribute list asks for double buffering, depth bits and so on. The
     * host pixel format is chosen to satisfy the usual set rather than parsed
     * from it: a 2006 game wants RGBA, double buffered, 24-bit depth and 8-bit
     * stencil, and that is what glXCreateContext below asks Windows for. */
    if (!make_window()) { RET(0); return; }
    RET(&g_visual);
}

static void h_glXCreateContext(CPU *c)
{
    if (!make_window()) { RET(0); return; }
    if (g_win.hglrc) { RET((uint32_t)(uintptr_t)g_win.hglrc); return; }

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize      = sizeof pfd;
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;

    int fmt = ChoosePixelFormat(g_win.hdc, &pfd);
    if (!fmt || !SetPixelFormat(g_win.hdc, fmt, &pfd)) {
        fprintf(stderr, "[glX] no usable pixel format\n");
        RET(0);
        return;
    }
    g_win.hglrc = wglCreateContext(g_win.hdc);
    if (!g_win.hglrc) { fprintf(stderr, "[glX] wglCreateContext failed\n"); RET(0); return; }
    fprintf(stderr, "[glX] context created (pixel format %d)\n", fmt);
    RET((uint32_t)(uintptr_t)g_win.hglrc);
}

static void h_glXMakeCurrent(CPU *c)
{
    HGLRC rc = (HGLRC)(uintptr_t)A32(2);
    if (!rc) { wglMakeCurrent(NULL, NULL); RET(1); return; }
    if (!wglMakeCurrent(g_win.hdc, rc)) { RET(0); return; }

    if (!g_win.gl) {
        g_win.gl = LoadLibraryA("opengl32.dll");
        const char *v = (const char *)glGetString(GL_VERSION);
        const char *r = (const char *)glGetString(GL_RENDERER);
        fprintf(stderr, "[glX] current: %s / %s\n", r ? r : "?", v ? v : "?");
    }
    ShowWindow(g_win.hwnd, SW_SHOW);
    RET(1);
}

/* ---- what is actually on the screen ----
 *
 * The call counts can look like rendering while the window stays black: a game
 * that draws into a framebuffer object and never blits it back produces just
 * as many glBegin and glBindTexture calls as one that draws to the screen.
 * So read the pixels rather than infer them.
 *
 * LINDBERGH_FBSTATS=1 reports how much of the presented buffer is non-black
 * for the first few frames. LINDBERGH_SHOT=<path> writes one frame out as a
 * BMP - the default framebuffer, exactly as presented, which is the honest
 * thing to put in a README.
 */
static unsigned g_frame;

static void write_bmp(const char *path, const unsigned char *bgr, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[shot] cannot write %s\n", path); return; }

    int stride = (w * 3 + 3) & ~3;           /* BMP rows are 4-byte aligned */
    uint32_t pix = (uint32_t)stride * (uint32_t)h;
    uint32_t off = 14 + 40;
    uint32_t size = off + pix;
    unsigned char hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    memcpy(hdr + 10, &off, 4);
    uint32_t hs = 40; memcpy(hdr + 14, &hs, 4);
    int32_t wi = w, hi = h;
    memcpy(hdr + 18, &wi, 4);
    memcpy(hdr + 22, &hi, 4);               /* positive: bottom-up, as GL gives */
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &pix, 4);
    fwrite(hdr, 1, sizeof hdr, f);

    unsigned char pad[3] = {0, 0, 0};
    for (int y = 0; y < h; y++) {
        fwrite(bgr + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        if (stride > w * 3) fwrite(pad, 1, (size_t)(stride - w * 3), f);
    }
    fclose(f);
    fprintf(stderr, "[shot] wrote %s (%dx%d)\n", path, w, h);
}

static void inspect_frame(void)
{
    const char *shot = getenv("LINDBERGH_SHOT");
    const char *stats = getenv("LINDBERGH_FBSTATS");
    unsigned shot_at = 0;
    if (shot) {
        const char *n = getenv("LINDBERGH_SHOT_FRAME");
        shot_at = (n && *n) ? (unsigned)atoi(n) : 300;
    }
    int want_stats = stats && *stats && *stats != '0' && g_frame < 8;

    /* Where is the frame actually going? A game that renders into a
     * framebuffer object and never brings it back leaves the default one
     * untouched - and the call counts look identical either way. */
    if (want_stats) {
        GLint fbo = 0, dbuf = 0, vp[4] = {0,0,0,0};
        glGetIntegerv(0x8CA6 /* GL_FRAMEBUFFER_BINDING_EXT */, &fbo);
        glGetIntegerv(GL_DRAW_BUFFER, &dbuf);
        glGetIntegerv(GL_VIEWPORT, vp);
        GLboolean cm[4] = {1,1,1,1};
        GLint dfunc = 0, afunc = 0;
        GLint sc[4] = {0,0,0,0};
        glGetIntegerv(GL_SCISSOR_BOX, sc);
        fprintf(stderr, "[fb] scissor %s box %d,%d %dx%d\n",
                glIsEnabled(GL_SCISSOR_TEST) ? "ON" : "off", sc[0], sc[1], sc[2], sc[3]);
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
        glGetIntegerv(GL_ALPHA_TEST_FUNC, &afunc);
        fprintf(stderr, "[fb] FBO %d dbuf 0x%X vp %dx%d err 0x%X | mask %d%d%d%d "
                        "depth(%d fn 0x%X) alpha(%d fn 0x%X) blend %d cull %d vp_arb %d fp_arb %d\n",
                (int)fbo, (unsigned)dbuf, vp[2], vp[3], glGetError(),
                cm[0], cm[1], cm[2], cm[3],
                glIsEnabled(GL_DEPTH_TEST), (unsigned)dfunc,
                glIsEnabled(GL_ALPHA_TEST), (unsigned)afunc,
                glIsEnabled(GL_BLEND), glIsEnabled(GL_CULL_FACE),
                glIsEnabled(0x8620 /* GL_VERTEX_PROGRAM_ARB */),
                glIsEnabled(0x8804 /* GL_FRAGMENT_PROGRAM_ARB */));
    }
    if (!want_stats && !(shot && g_frame == shot_at)) return;

    int w = g_win.w, h = g_win.h;
    unsigned char *buf = (unsigned char *)malloc((size_t)w * h * 3);
    if (!buf) return;

    /* Check the instrument before trusting it. On one frame, paint a colour
     * nothing else would produce and read it straight back: if that does not
     * come through, the readback or the drawable is wrong and every other
     * measurement here is meaningless. */
    if (want_stats && g_frame == 3) {
        glClearColor(0.0f, 0.25f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, buf);

    if (want_stats) {
        size_t lit = 0, n = (size_t)w * h;
        unsigned peak = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned v = buf[i*3] | buf[i*3+1] | buf[i*3+2];
            if (v > 8) lit++;
            if (v > peak) peak = v;
        }
        fprintf(stderr, "[fb] frame %u: %.1f%% non-black, peak channel %u\n",
                g_frame, 100.0 * (double)lit / (double)n, peak);
        fflush(stderr);
    }
    if (shot && g_frame == shot_at) write_bmp(shot, buf, w, h);
    free(buf);
}

static void h_glXSwapBuffers(CPU *c)
{
    inspect_frame();            /* before the swap: the back buffer is the frame */

    /* Who presents the frame? Working back from the swap names the loop that
     * should have composited into the default framebuffer just before it. */
    if (g_frame == 5) {
        const char *v = getenv("LINDBERGH_FBSTATS");
        if (v && *v && *v != '0') {
            fprintf(stderr, "[fb] present path:\n");
            guest_backtrace(c);
        }
    }
    g_frame++;
    SwapBuffers(g_win.hdc);
    pump();
}

static void h_glXGetProcAddressARB(CPU *c)
{
    /* Hand back the guest's own PLT stub for the name. See hle_plt_address:
     * the guest CALLS this pointer, so it has to be an address dispatch() can
     * route, and the stub already is one. A name the binary does not import
     * has no token to give, and 0 is what GLX returns for an entry point the
     * driver does not have - which the game already knows how to handle. */
    const char *name = ASTR(0);

    /* An imported name already has a routable token with a handler behind it,
     * so prefer the stub. Everything else - and init_extensions asks for 547
     * of them - gets a synthetic one that dispatch() forwards to the host
     * driver. Returning 0 for those left the engine feature flags clear, and
     * graphics init gave up right after its first glClear. */
    uint32_t va = hle_plt_address(name);
    if (!va) va = gl_token_for(name);
    RET(va);
}

static void h_glXDestroyContext(CPU *c)
{
    wglMakeCurrent(NULL, NULL);
    if (g_win.hglrc) { wglDeleteContext(g_win.hglrc); g_win.hglrc = NULL; }
}

static void h_glXGetCurrentContext(CPU *c)  { RET((uint32_t)(uintptr_t)wglGetCurrentContext()); }
static void h_glXGetCurrentDisplay(CPU *c)  { RET(build_display()); }
static void h_glXGetCurrentDrawable(CPU *c) { RET(TOK_WINDOW); }

/* The game checks these strings for extension names before using an entry
 * point. Reporting none is honest and safe: it falls back to the ARB path it
 * also ships, and glXGetProcAddressARB still resolves whatever it asks for. */
static void h_glXQueryExtensionsString(CPU *c) { RET(""); }
static void h_glXQueryServerString(CPU *c)     { RET("lindberghrecomp"); }
static void h_glXGetClientString(CPU *c)       { RET("lindberghrecomp"); }

/* SGIX pbuffers: an off-screen render target this game asks about and can do
 * without. Reporting no configs sends it down the path that does not use one. */
static void h_glXChooseFBConfigSGIX(CPU *c)   { if (A32(3)) wr32(A32(3), 0); RET(0); }
static void h_glXGetFBConfigAttribSGIX(CPU *c) { RET(-1); }
static void h_glXCreateContextWithConfigSGIX(CPU *c) { RET(0); }
static void h_glXCreateGLXPbufferSGIX(CPU *c) { RET(0); }
static void h_glXDestroyGLXPbufferSGIX(CPU *c) { (void)c; }

static void h_glXSwapIntervalSGI(CPU *c)
{
    /* vsync. The cabinet was locked to the display; a desktop should be too,
     * or the game runs its logic at whatever rate the GPU can manage. */
    typedef BOOL (WINAPI *PFNSWAPINTERVAL)(int);
    static PFNSWAPINTERVAL swap_interval;
    if (!swap_interval)
        swap_interval = (PFNSWAPINTERVAL)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval) swap_interval((int)A32(0));
    RET(0);
}

/* ---- Xlib ---- */
static void h_XOpenDisplay(CPU *c)
{
    if (!make_window()) { RET(0); return; }
    RET(build_display());
}

static void h_XCloseDisplay(CPU *c)   { (void)c; }
static void h_XInitThreads(CPU *c)    { RET(1); }
static void h_XLockDisplay(CPU *c)    { (void)c; }   /* one thread touches GL */
static void h_XUnlockDisplay(CPU *c)  { (void)c; }
static void h_XSetErrorHandler(CPU *c) { RET(0); }
static void h_XGetErrorText(CPU *c)
{
    if (A32(2) && A32(3) > 0) { ASTR(2)[0] = 0; }
    RET(0);
}

static void h_XCreateWindow(CPU *c)   { make_window(); RET(TOK_WINDOW); }
static void h_XCreateColormap(CPU *c) { RET(TOK_COLORMAP); }
static void h_XDestroyWindow(CPU *c)  { (void)c; }

static void h_XMapWindow(CPU *c)
{
    if (g_win.hwnd) {
        ShowWindow(g_win.hwnd, SW_SHOW);
        SetForegroundWindow(g_win.hwnd);
    }
    pump();
    RET(0);
}

static void h_XStoreName(CPU *c)
{
    if (g_win.hwnd && A32(2)) SetWindowTextA(g_win.hwnd, ASTR(2));
    RET(0);
}
static void h_XMoveWindow(CPU *c) { RET(0); }

/* Atoms are names the X server interns and hands back as ids. Nothing here
 * resolves them again, so a counter is enough - the game only ever compares
 * one it was given against another it was given. */
static void h_XInternAtom(CPU *c)
{
    static uint32_t next = 1000;
    RET(++next);
}

/* Events. Reporting an empty queue is not a stub for input - it is how this
 * seam works. The cabinet's controls arrive over JVS, not through X, so an X
 * event loop that never fires is the correct shape; what it must not do is
 * stop pumping the host's own queue, or Windows greys the window out. */
static void h_XPending(CPU *c)   { pump(); RET(0); }
static void h_XFlush(CPU *c)     { pump(); RET(0); }
static void h_XSync(CPU *c)      { pump(); RET(0); }
static void h_XNextEvent(CPU *c)
{
    pump();
    if (A32(1)) memset(APTR(1), 0, 96);      /* sizeof(XEvent) on i386 */
    RET(0);
}

/* XFree releases memory Xlib allocated. Everything this file hands out is
 * static or a token, so freeing would be a double free of something never
 * allocated. Doing nothing is correct here, not lazy. */
static void h_XFree(CPU *c) { RET(0); }

/* ---- pointer ----
 *
 * This is the light gun. Let's Go Jungle aims with a screen-position device,
 * and on a desktop that device is the mouse - so XQueryPointer wiring the real
 * cursor through is not a placeholder, it is the port. */
static void h_XQueryPointer(CPU *c)
{
    POINT p = { 0, 0 };
    GetCursorPos(&p);
    POINT w = p;
    if (g_win.hwnd) ScreenToClient(g_win.hwnd, &w);

    if (A32(2)) wr32(A32(2), TOK_WINDOW);         /* root */
    if (A32(3)) wr32(A32(3), TOK_WINDOW);         /* child */
    if (A32(4)) wr32(A32(4), (uint32_t)p.x);      /* root x */
    if (A32(5)) wr32(A32(5), (uint32_t)p.y);
    if (A32(6)) wr32(A32(6), (uint32_t)w.x);      /* window x */
    if (A32(7)) wr32(A32(7), (uint32_t)w.y);
    if (A32(8)) {
        uint32_t mask = 0;
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) mask |= 1u << 8;   /* Button1 */
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) mask |= 1u << 10;  /* Button3 */
        wr32(A32(8), mask);
    }
    RET(1);
}

static void h_XWarpPointer(CPU *c)
{
    /* The game recentres the pointer. Honour it against the window's client
     * area, or aiming drifts every frame. */
    if (g_win.hwnd) {
        POINT p = { (LONG)(int32_t)A32(7), (LONG)(int32_t)A32(8) };
        ClientToScreen(g_win.hwnd, &p);
        SetCursorPos(p.x, p.y);
    }
    RET(0);
}

static void h_XTranslateCoordinates(CPU *c)
{
    POINT p = { (LONG)(int32_t)A32(3), (LONG)(int32_t)A32(4) };
    if (A32(5)) wr32(A32(5), (uint32_t)p.x);
    if (A32(6)) wr32(A32(6), (uint32_t)p.y);
    if (A32(7)) wr32(A32(7), TOK_WINDOW);
    RET(1);
}

/* Grabs, focus, cursor shape and window-manager hints. The window is ours
 * alone and already has the pointer over it; these arrange things that are
 * arranged. Zero is Success for the grabs. */
static void h_x_success(CPU *c) { RET(0); }
static void h_x_token(CPU *c)   { RET(TOK_WINDOW); }
static void h_x_void(CPU *c)    { (void)c; }

static void h_XMissingExtension(CPU *c)
{
    /* Xlib's "extension not present" path. Naming it says which one the game
     * wanted, which is the difference between guessing at the graphics seam
     * and knowing. */
    fprintf(stderr, "[X] missing extension: %s\n", A32(1) ? ASTR(1) : "(null)");
    fflush(stderr);
    RET(0);
}

static void h_XDefineCursor(CPU *c)
{
    /* The game hides the hardware cursor and draws its own crosshair. */
    ShowCursor(FALSE);
    RET(0);
}

void hle_register_window(void)
{
    hle_bind("glXChooseVisual", h_glXChooseVisual);
    hle_bind("glXCreateContext", h_glXCreateContext);
    hle_bind("glXMakeCurrent", h_glXMakeCurrent);
    hle_bind("glXSwapBuffers", h_glXSwapBuffers);
    hle_bind("glXGetProcAddressARB", h_glXGetProcAddressARB);
    hle_bind("glXDestroyContext", h_glXDestroyContext);
    hle_bind("glXGetCurrentContext", h_glXGetCurrentContext);
    hle_bind("glXGetCurrentDisplay", h_glXGetCurrentDisplay);
    hle_bind("glXGetCurrentDrawable", h_glXGetCurrentDrawable);
    hle_bind("glXQueryExtensionsString", h_glXQueryExtensionsString);
    hle_bind("glXQueryServerString", h_glXQueryServerString);
    hle_bind("glXGetClientString", h_glXGetClientString);
    hle_bind("glXSwapIntervalSGI", h_glXSwapIntervalSGI);
    hle_bind("glXChooseFBConfigSGIX", h_glXChooseFBConfigSGIX);
    hle_bind("glXGetFBConfigAttribSGIX", h_glXGetFBConfigAttribSGIX);
    hle_bind("glXCreateContextWithConfigSGIX", h_glXCreateContextWithConfigSGIX);
    hle_bind("glXCreateGLXPbufferSGIX", h_glXCreateGLXPbufferSGIX);
    hle_bind("glXDestroyGLXPbufferSGIX", h_glXDestroyGLXPbufferSGIX);

    hle_bind("XOpenDisplay", h_XOpenDisplay);
    hle_bind("XCloseDisplay", h_XCloseDisplay);
    hle_bind("XInitThreads", h_XInitThreads);
    hle_bind("XLockDisplay", h_XLockDisplay);
    hle_bind("XUnlockDisplay", h_XUnlockDisplay);
    hle_bind("XSetErrorHandler", h_XSetErrorHandler);
    hle_bind("XGetErrorText", h_XGetErrorText);
    hle_bind("XCreateWindow", h_XCreateWindow);
    hle_bind("XCreateColormap", h_XCreateColormap);
    hle_bind("XDestroyWindow", h_XDestroyWindow);
    hle_bind("XMapWindow", h_XMapWindow);
    hle_bind("XStoreName", h_XStoreName);
    hle_bind("XMoveWindow", h_XMoveWindow);
    hle_bind("XInternAtom", h_XInternAtom);
    hle_bind("XPending", h_XPending);
    hle_bind("XFlush", h_XFlush);
    hle_bind("XSync", h_XSync);
    hle_bind("XNextEvent", h_XNextEvent);
    hle_bind("XFree", h_XFree);
    hle_bind("XQueryPointer", h_XQueryPointer);
    hle_bind("XWarpPointer", h_XWarpPointer);
    hle_bind("XTranslateCoordinates", h_XTranslateCoordinates);
    hle_bind("XDefineCursor", h_XDefineCursor);

    /* Arrangements that are already arranged: the window is ours alone, it
     * already has the pointer, and there is no window manager to hint at. */
    hle_bind("XGrabPointer", h_x_success);
    hle_bind("XGrabKeyboard", h_x_success);
    hle_bind("XUngrabPointer", h_x_success);
    hle_bind("XUngrabKeyboard", h_x_success);
    hle_bind("XSetInputFocus", h_x_success);
    hle_bind("XSetWMProperties", h_x_success);
    hle_bind("XSetWMProtocols", h_x_success);
    hle_bind("XStringListToTextProperty", h_x_success);
    hle_bind("XSetCloseDownMode", h_x_success);
    hle_bind("XAutoRepeatOn", h_x_success);
    hle_bind("XParseColor", h_x_success);
    hle_bind("XLookupString", h_x_success);
    hle_bind("XMissingExtension", h_XMissingExtension);
    hle_bind("XextAddDisplay", h_x_success);
    hle_bind("XextRemoveDisplay", h_x_success);
    hle_bind("XextFindDisplay", h_x_success);
    hle_bind("XextCreateExtension", h_x_success);

    hle_bind("XCreatePixmapCursor", h_x_token);
    hle_bind("XCreatePixmapFromBitmapData", h_x_token);
    hle_bind("XFreeCursor", h_x_void);
    hle_bind("XFreePixmap", h_x_void);
}

#else   /* !_WIN32 */
void hle_register_window(void) { }
#endif
