/*
 * hle_glut.c - the other way a Lindbergh game asks for a window.
 *
 * Lets Go Jungle and Virtua Tennis 3 drive GLX directly and run their own
 * loop. Most of the platform does not: Ghost Squad Evolution, After Burner
 * Climax and OutRun 2 SP SDX all use GLUT, which inverts control. The game
 * registers callbacks, calls glutMainLoop, and never gets the thread back.
 *
 * So glutMainLoop here does not return either. It becomes the loop: pump the
 * host message queue, hand any input to the callbacks the game registered,
 * run the idle callback, and redraw when a redisplay has been posted. Calling
 * back into lifted code is what guest_call already exists for.
 *
 * The window and the GL context are not ours - hle_window.c owns them, and
 * both seams borrow the same one. Two windows would be two too many.
 */

#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <GL/gl.h>

#include "lindbergh_rt.h"

extern unsigned g_frame;

/* Every callback a game can register, as guest addresses. A zero means the
 * game never asked for that one, and nothing is called. */
static struct {
    uint32_t display, reshape, idle, visibility, entry;
    uint32_t keyboard, keyboard_up, special, special_up;
    uint32_t mouse, motion, passive_motion, joystick;
} g_cb;

static int   g_redisplay = 1;   /* GLUT draws once before anything is posted */
static int   g_modifiers;
static DWORD g_start_ms;

/* GLUT numbering for the keys that are not characters. */
#define GLUT_KEY_F1     1
#define GLUT_KEY_LEFT   100
#define GLUT_KEY_UP     101
#define GLUT_KEY_RIGHT  102
#define GLUT_KEY_DOWN   103

static int glut_special(int vk)
{
    if (vk >= VK_F1 && vk <= VK_F12) return GLUT_KEY_F1 + (vk - VK_F1);
    if (vk == VK_LEFT)  return GLUT_KEY_LEFT;
    if (vk == VK_UP)    return GLUT_KEY_UP;
    if (vk == VK_RIGHT) return GLUT_KEY_RIGHT;
    if (vk == VK_DOWN)  return GLUT_KEY_DOWN;
    return 0;
}

static void call1(CPU *c, uint32_t fn, uint32_t a)
{
    if (fn) guest_call(c, fn, &a, 1);
}

static void call2(CPU *c, uint32_t fn, uint32_t a, uint32_t b)
{
    uint32_t v[2]; v[0] = a; v[1] = b;
    if (fn) guest_call(c, fn, v, 2);
}

static void call3(CPU *c, uint32_t fn, uint32_t a, uint32_t b, uint32_t d)
{
    uint32_t v[3]; v[0] = a; v[1] = b; v[2] = d;
    if (fn) guest_call(c, fn, v, 3);
}

static void call4(CPU *c, uint32_t fn, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{
    uint32_t v[4]; v[0] = a; v[1] = b; v[2] = d; v[3] = e;
    if (fn) guest_call(c, fn, v, 4);
}

/* Drain what the window procedure collected and give it to the game.
 *
 * Motion and passive motion are one event to Windows; GLUT splits them on
 * whether a button is held, which is what the queue records beside the
 * position. */
static void deliver_input(CPU *c)
{
    HostInput e;
    while (host_input_pop(&e)) {
        if (e.kind == HOST_IN_MOTION) {
            if (e.c) call2(c, g_cb.motion, (uint32_t)e.a, (uint32_t)e.b);
            else     call2(c, g_cb.passive_motion, (uint32_t)e.a, (uint32_t)e.b);
        } else if (e.kind == HOST_IN_MOUSE) {
            call4(c, g_cb.mouse, (uint32_t)e.a, (uint32_t)e.b,
                  (uint32_t)e.c, (uint32_t)e.d);
        } else if (e.kind == HOST_IN_KEY) {
            call3(c, g_cb.keyboard, (uint32_t)(e.a & 0xFF),
                  (uint32_t)e.c, (uint32_t)e.d);
        } else if (e.kind == HOST_IN_KEYUP) {
            int sp = glut_special(e.a);
            if (sp) call3(c, g_cb.special_up, (uint32_t)sp,
                          (uint32_t)e.c, (uint32_t)e.d);
            else    call3(c, g_cb.keyboard_up, (uint32_t)(e.a & 0xFF),
                          (uint32_t)e.c, (uint32_t)e.d);
        } else if (e.kind == HOST_IN_SPECIAL) {
            int sp = glut_special(e.a);
            if (sp) call3(c, g_cb.special, (uint32_t)sp,
                          (uint32_t)e.c, (uint32_t)e.d);
        }
    }
}

/* ---- set up ---- */

static void h_glutInit(CPU *c)
{
    g_start_ms = GetTickCount();
    fprintf(stderr, "[glut] init\n");
    RET(0);
}

static void h_noop(CPU *c) { RET(0); }

static void h_glutInitWindowSize(CPU *c)
{
    host_window_hint((int)A32(0), (int)A32(1));
    RET(0);
}

/* A game mode string is "640x480:32\n60" and every part after the resolution
 * is about a display mode we are not going to set. The size is the part that
 * matters, because the game will draw at it regardless. */
static void h_glutGameModeString(CPU *c)
{
    const char *sp = A32(0) ? (const char *)(uintptr_t)A32(0) : NULL;
    int w = 0, h = 0;
    if (sp && sscanf(sp, "%dx%d", &w, &h) == 2) {
        fprintf(stderr, "[glut] game mode string \"%s\" -> %dx%d\n", sp, w, h);
        host_window_hint(w, h);
    }
    RET(0);
}

/* glutCreateWindow and glutEnterGameMode both mean "give me somewhere to
 * draw". Game mode is fullscreen on the cabinet; here it is the same window,
 * because a recompiled game that takes over the display and then stops is
 * worse than one you can alt-tab away from. */
static void h_glutCreateWindow(CPU *c)
{
    const char *title = A32(0) ? (const char *)(uintptr_t)A32(0) : "lindberghrecomp";
    if (!host_window_open(title) || !host_gl_context()) { RET(0); return; }
    RET(1);
}

static void h_glutEnterGameMode(CPU *c)
{
    if (!host_window_open("lindberghrecomp") || !host_gl_context()) { RET(0); return; }
    fprintf(stderr, "[glut] game mode\n");
    RET(1);
}

/* ---- the callbacks ----
 *
 * Written out rather than macro-generated: thirteen plain lines read better
 * than a macro that hides which field each one sets. */

static void h_set_display(CPU *c)        { g_cb.display = A32(0); RET(0); }
static void h_set_reshape(CPU *c)        { g_cb.reshape = A32(0); RET(0); }
static void h_set_idle(CPU *c)           { g_cb.idle = A32(0); RET(0); }
static void h_set_visibility(CPU *c)     { g_cb.visibility = A32(0); RET(0); }
static void h_set_entry(CPU *c)          { g_cb.entry = A32(0); RET(0); }
static void h_set_keyboard(CPU *c)       { g_cb.keyboard = A32(0); RET(0); }
static void h_set_keyboard_up(CPU *c)    { g_cb.keyboard_up = A32(0); RET(0); }
static void h_set_special(CPU *c)        { g_cb.special = A32(0); RET(0); }
static void h_set_special_up(CPU *c)     { g_cb.special_up = A32(0); RET(0); }
static void h_set_mouse(CPU *c)          { g_cb.mouse = A32(0); RET(0); }
static void h_set_motion(CPU *c)         { g_cb.motion = A32(0); RET(0); }
static void h_set_passive_motion(CPU *c) { g_cb.passive_motion = A32(0); RET(0); }
static void h_set_joystick(CPU *c)       { g_cb.joystick = A32(0); RET(0); }

/* ---- during the frame ---- */

static void h_glutSwapBuffers(CPU *c)   { host_swap(); RET(0); }
static void h_glutPostRedisplay(CPU *c) { g_redisplay = 1; RET(0); }
static void h_glutGetModifiers(CPU *c)  { RET((uint32_t)g_modifiers); }

static void h_glutGet(CPU *c)
{
    int w, h;
    lindbergh_window_size(&w, &h);
    switch (A32(0)) {
    case 100: RET(0); return;                       /* window x */
    case 101: RET(0); return;                       /* window y */
    case 102: RET((uint32_t)w); return;             /* window width */
    case 103: RET((uint32_t)h); return;             /* window height */
    case 115: RET(1); return;                       /* double buffered */
    case 200: RET((uint32_t)w); return;             /* screen width */
    case 201: RET((uint32_t)h); return;             /* screen height */
    case 700: RET((uint32_t)(GetTickCount() - g_start_ms)); return;
    }
    RET(0);
}

/* The game asks before it uses an extension; the honest answer is whatever
 * the driver underneath actually reports. */
static void h_glutExtensionSupported(CPU *c)
{
    const char *want = (const char *)(uintptr_t)A32(0);
    const char *have = (const char *)glGetString(GL_EXTENSIONS);
    RET(want && have && strstr(have, want) ? 1 : 0);
}

/* ---- the loop ---- */

static void h_glutMainLoop(CPU *c)
{
    int w, h;
    lindbergh_window_size(&w, &h);
    fprintf(stderr, "[glut] entering the game loop\n");

    /* GLUT reshapes once before the first frame, and a game that sizes its
     * projection in that callback draws nothing until it arrives. */
    call2(c, g_cb.reshape, (uint32_t)w, (uint32_t)h);
    call1(c, g_cb.visibility, 1);

    unsigned long spins = 0;
    for (;;) {
        host_pump();
        deliver_input(c);

        /* If this ever falls out, say so - an infinite loop that ends is the
         * kind of thing that looks like a clean exit from the outside. */
        if (++spins % 2000 == 0)
            fprintf(stderr, "[glut] loop alive, %lu iterations, frame %u\n",
                    spins, g_frame);

        if (g_cb.idle) guest_call(c, g_cb.idle, NULL, 0);

        if (g_redisplay && g_cb.display) {
            g_redisplay = 0;
            guest_call(c, g_cb.display, NULL, 0);
        } else if (!g_cb.idle) {
            Sleep(1);                 /* nothing to do; do not spin a core */
        }
    }
}

void hle_register_glut(void)
{
    int n = 0;
    n += hle_bind("glutInit", h_glutInit);
    n += hle_bind("glutInitDisplayMode", h_noop);
    n += hle_bind("glutInitWindowSize", h_glutInitWindowSize);
    n += hle_bind("glutInitWindowPosition", h_noop);
    n += hle_bind("glutGameModeString", h_glutGameModeString);
    n += hle_bind("glutSetCursor", h_noop);
    n += hle_bind("glutLeaveGameMode", h_noop);
    n += hle_bind("glutCreateWindow", h_glutCreateWindow);
    n += hle_bind("glutEnterGameMode", h_glutEnterGameMode);

    n += hle_bind("glutDisplayFunc", h_set_display);
    n += hle_bind("glutReshapeFunc", h_set_reshape);
    n += hle_bind("glutIdleFunc", h_set_idle);
    n += hle_bind("glutVisibilityFunc", h_set_visibility);
    n += hle_bind("glutEntryFunc", h_set_entry);
    n += hle_bind("glutKeyboardFunc", h_set_keyboard);
    n += hle_bind("glutKeyboardUpFunc", h_set_keyboard_up);
    n += hle_bind("glutSpecialFunc", h_set_special);
    n += hle_bind("glutSpecialUpFunc", h_set_special_up);
    n += hle_bind("glutMouseFunc", h_set_mouse);
    n += hle_bind("glutMotionFunc", h_set_motion);
    n += hle_bind("glutPassiveMotionFunc", h_set_passive_motion);
    n += hle_bind("glutJoystickFunc", h_set_joystick);

    n += hle_bind("glutSwapBuffers", h_glutSwapBuffers);
    n += hle_bind("glutPostRedisplay", h_glutPostRedisplay);
    n += hle_bind("glutGetModifiers", h_glutGetModifiers);
    n += hle_bind("glutGet", h_glutGet);
    n += hle_bind("glutExtensionSupported", h_glutExtensionSupported);
    n += hle_bind("glutMainLoop", h_glutMainLoop);

    /* The teapot and friends. A game links them because GLUT defines them,
     * not because it draws them; answering is cheaper than aborting. */
    n += hle_bind("glutBitmapCharacter", h_noop);
    n += hle_bind("glutSolidCube", h_noop);
    n += hle_bind("glutWireCube", h_noop);
    n += hle_bind("glutSolidCone", h_noop);
    n += hle_bind("glutWireCone", h_noop);
    n += hle_bind("glutSolidSphere", h_noop);
    n += hle_bind("glutWireSphere", h_noop);
    n += hle_bind("glutSolidTeapot", h_noop);

    fprintf(stderr, "[glut] %d entry points bound\n", n);
}
