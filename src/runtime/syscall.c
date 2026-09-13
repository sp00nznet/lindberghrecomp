/*
 * syscall.c - the kernel end of a recompiled Lindbergh game.
 *
 * Linux/i386 convention: number in eax, arguments in ebx, ecx, edx, esi, edi,
 * ebp; result back in eax, and a failure is the negative errno rather than -1
 * plus a global. The game's own CRT does the errno translation, so returning
 * the raw negative value is what it expects.
 *
 * Implemented: what a process needs to start up and talk to stdout/the disc.
 * Everything else returns -GUEST_ENOSYS and says so once, which is the to-do list -
 * a game that wedges tells you the number it wanted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define open_  _open
#define read_  _read
#define write_ _write
#define close_ _close
#define lseek_ _lseek
#else
#include <fcntl.h>
#include <unistd.h>
#define open_  open
#define read_  read
#define write_ write
#define close_ close
#define lseek_ lseek
#endif

#include "lindbergh_rt.h"

/* Linux/i386 errno values, not the host's: the game's own CRT compares
 * against the numbers it was built with, and the UCRT does not agree (its
 * ENOSYS is 40). Named apart so the host headers cannot quietly win. */
#define GUEST_ENOSYS 38
#define GUEST_ENOMEM 12

/* Linux/i386 numbers. */
enum {
    SYS_exit = 1, SYS_read = 3, SYS_write = 4, SYS_open = 5, SYS_close = 6,
    SYS_lseek = 19, SYS_getpid = 20, SYS_brk = 45, SYS_ioctl = 54,
    SYS_munmap = 91, SYS_gettimeofday = 78, SYS_mmap2 = 192,
    SYS_exit_group = 252,
};

static void *gp(uint32_t va) { return (void *)(uintptr_t)va; }

/* The guest's O_* are Linux's, not the host's; only the low three bits and a
 * handful of flags ever appear, and a straight pass-through opens the wrong
 * file mode on Windows. */
static int xlate_open_flags(uint32_t f)
{
    int out = (int)(f & 3);                 /* O_RDONLY/WRONLY/RDWR agree */
#ifdef _WIN32
    out |= _O_BINARY;
    if (f & 0x40)  out |= _O_CREAT;
    if (f & 0x200) out |= _O_TRUNC;
    if (f & 0x400) out |= _O_APPEND;
#else
    if (f & 0x40)  out |= O_CREAT;
    if (f & 0x200) out |= O_TRUNC;
    if (f & 0x400) out |= O_APPEND;
#endif
    return out;
}

static void unimplemented(uint32_t nr)
{
    static unsigned char seen[512];
    if (nr < sizeof seen && seen[nr]++) return;
    fprintf(stderr, "[syscall] %u unimplemented -> -ENOSYS\n", nr);
}

void linux_syscall(CPU *c)
{
    uint32_t nr = c->eax, a = c->ebx, b = c->ecx, d = c->edx;
    int32_t r;

    switch (nr) {
    case SYS_exit:
    case SYS_exit_group:
        exit((int)a);

    case SYS_read:   r = (int32_t)read_((int)a, gp(b), (unsigned)d); break;
    case SYS_write:  r = (int32_t)write_((int)a, gp(b), (unsigned)d); break;
    case SYS_open:   r = (int32_t)open_((const char *)gp(a), xlate_open_flags(b), 0666); break;
    case SYS_close:  r = (int32_t)close_((int)a); break;
    case SYS_lseek:  r = (int32_t)lseek_((int)a, (long)b, (int)d); break;
    case SYS_getpid: r = 1; break;
    case SYS_brk:    r = (int32_t)guest_brk(a); break;

    case SYS_gettimeofday: {
        /* struct timeval { long tv_sec; long tv_usec; } - both 32-bit here. */
        if (a) {
            uint32_t *tv = (uint32_t *)gp(a);
            struct timespec ts;
            timespec_get(&ts, TIME_UTC);
            tv[0] = (uint32_t)ts.tv_sec;
            tv[1] = (uint32_t)(ts.tv_nsec / 1000);
        }
        r = 0;
        break;
    }

    /* mmap2(addr=ebx, len=ecx, prot=edx, flags=esi, fd=edi, pgoff=ebp).
     * Anonymous is all a starting process asks for, and it comes off the top
     * of the break. A file-backed map needs the loader we do not have, so it
     * refuses loudly rather than handing back memory with the wrong contents. */
    case SYS_mmap2: {
        if (!(c->esi & 0x20)) { unimplemented(nr); r = -GUEST_ENOSYS; break; }  /* MAP_ANONYMOUS */
        uint32_t want = (b + 0xFFFu) & ~0xFFFu;
        uint32_t base = guest_brk(0);
        r = (guest_brk(base + want) >= base + want) ? (int32_t)base : -GUEST_ENOMEM;
        break;
    }
    case SYS_munmap: r = 0; break;      /* the break never shrinks; harmless */

    case SYS_ioctl:  r = -GUEST_ENOSYS; break;   /* JVS I/O lands here - see docs */

    default:
        unimplemented(nr);
        r = -GUEST_ENOSYS;
        break;
    }
    c->eax = (uint32_t)r;
}
