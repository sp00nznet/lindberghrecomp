/*
 * hle_sega.c - the cabinet's own library, answered from here.
 *
 * A Lindbergh game does not talk to its hardware directly. It calls SEGA's
 * amLib: a C API, statically linked into this binary, that reaches a base
 * board over a serial link - the JVS I/O board with the coin slots, buttons
 * and guns on it, the EEPROM holding the bookkeeping, the security dongle.
 * None of that exists here, so every one of those calls fails and the game
 * stops on "Error 11 - JVS I/O board is not connected to main board" before
 * it ever reaches its attract mode.
 *
 * These are plain C symbols, so they override by name the same way the
 * XF86VidMode entry points do. What is answered here is only what the game
 * actually asks on the way to attract mode; the game narrates the rest
 * through _sDebug::putConsole (LINDBERGH_CONSOLE=1), which is how each of
 * these was found rather than guessed.
 */

#include <stdio.h>
#include <string.h>

#include "lindbergh_rt.h"

/* The library's own success code. Every am* function returns 0 for success
 * and a negative value for a failure the game then reports. */
#define AM_OK 0

static void h_ok(CPU *c)   { RET(AM_OK); }
static void h_true(CPU *c) { RET(1); }
static void h_one(CPU *c)  { RET(1); }

void hle_register_sega(void)
{
    int n = 0;

    /* The base board. amLibInit failing is what produces "SEGA BaseBD not
     * available", and everything else is gated behind it. */
    n += guest_override("amLibInit", h_ok);
    n += guest_override("amLibExit", h_ok);
    n += guest_override("amLibIsBasebdAvailable", h_true);

    /* The JVS link itself. amJvsCheckInit is a predicate, not a status code:
     * amJvsGetNodes reads it as a boolean and answers -5 when it is false,
     * which is how answering it with the library's success value of 0 turned
     * into "-5 JVS node(s) found". */
    n += guest_override("amJvsInit", h_ok);
    n += guest_override("amJvsExit", h_ok);
    n += guest_override("amJvsCheckInit", h_true);

    /* One I/O board on the link - the cabinet's own, with the buttons, the
     * coin slots and the guns. */
    n += guest_override("amJvsGetNodes", h_one);

    /* The security dongle. Nothing here reads a real one, and the game only
     * asks whether it is present before carrying on. */
    n += guest_override("amDongleInit", h_ok);
    n += guest_override("amDongleExit", h_ok);
    n += guest_override("amDongleIsAvailable", h_true);

    fprintf(stderr, "[sega] %d base board entry points answered\n", n);
}
