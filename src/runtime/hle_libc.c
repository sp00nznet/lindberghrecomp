/*
 * hle_libc.c - the C library the board provided.
 *
 * A Lindbergh game links glibc 2.3.x, and almost every one of those imports is
 * a function the host CRT already has. So these are thunks, not
 * implementations: read the arguments off the guest stack, call the host, put
 * the result back where the guest expects it.
 *
 * Two things about the guest ABI matter while reading:
 *
 *   - It is cdecl, and the lifter pushed no return address, so argument 0 is
 *     at esp+0. A handler must not move esp; the caller's own `add esp, N`
 *     does that.
 *   - A function returning float or double returns it in st(0), not in an XMM
 *     register. That is the i386 SysV convention, and it is why every math
 *     thunk here ends in RETF rather than RET.
 *
 * Pointers need no translation: the memory model is flat and the image is
 * mapped where it was linked, so a guest pointer IS a host pointer.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#else
#include <unistd.h>
#endif

#include "lindbergh_rt.h"

/* ---- memory ----
 *
 * The guest's allocator is built here on plain malloc rather than on the
 * host's aligned one, and that is the second time this seam has bitten.
 *
 * First it was free(): the game memaligns its SSE buffers and releases them
 * with plain free(), exactly as glibc allows, but _aligned_malloc memory only
 * _aligned_free understands.
 *
 * Routing everything through _aligned_malloc fixed that and introduced the
 * next one, because _aligned_realloc requires the SAME alignment the block was
 * allocated with - and a block the game got from memalign(64, n) reallocated
 * at 16 is undefined. MSVC notices and calls __fastfail, which raises
 * STATUS_STACK_BUFFER_OVERRUN past every exception handler by design, so the
 * process vanishes with no report at all.
 *
 * Owning the layout ends the whole category. A header immediately below the
 * returned pointer carries the original block and the requested size, so
 * free() and realloc() work on anything malloc, calloc, memalign or strdup
 * returned, at any alignment, with no rules to remember. The magic word is
 * also a check at a real trust boundary: a guest pointer that never came from
 * here is refused rather than passed to free().
 */
#define GUEST_ALIGN 16
#define ALLOC_MAGIC 0xA110C8EDu

typedef struct {
    void    *base;
    size_t   size;
    uint32_t magic;
} AllocHdr;

/* glibc hands any request at or above its mmap threshold - 128 KiB by default
 * - straight to mmap, so a large block always comes back page aligned. Guest
 * code never asks for that and never checks it, which is exactly why it can
 * depend on it without anyone noticing.
 *
 * Ghost Squad Evolution builds its own allocator on top of malloc and rounds
 * the first free block up to a 32-byte boundary. Given a page-aligned heap the
 * rounding is a no-op. Given a merely 16-aligned one it shifts the block by
 * 16 bytes, and the split-off block then overlaps the header of the block it
 * was split from - so the allocator reads a "next" pointer out of memory it
 * has just zeroed, and writes through the null it finds. The fault lands two
 * functions away from the cause, with nothing pointing back at malloc. */
#define GUEST_MMAP_THRESHOLD (128u * 1024u)
#define GUEST_PAGE           4096u

static void *gmalloc(size_t n, size_t align)
{
    if (align < GUEST_ALIGN) align = GUEST_ALIGN;
    if (n >= GUEST_MMAP_THRESHOLD && align < GUEST_PAGE) align = GUEST_PAGE;
    size_t hdr = sizeof(AllocHdr);

    /* LINDBERGH_HEAP_SLACK=N leaves N spare bytes past every allocation.
     *
     * A guest that writes past the end of its own block lands in the host
     * heap's own bookkeeping, and the CRT answers that by calling __fastfail -
     * no handler, no message, exit code 0xC0000409. Slack does not fix an
     * overrun, it moves the damage somewhere harmless so the overrun can be
     * seen rather than being the last thing that ever happens. */
    static size_t slack = (size_t)-1;
    if (slack == (size_t)-1) {
        const char *v = getenv("LINDBERGH_HEAP_SLACK");
        slack = (v && *v) ? (size_t)strtoul(v, NULL, 0) : 0;
    }
    void *base = malloc(n + align + hdr + slack);
    if (!base) {
        /* The guest has no way to report this and will dereference the null it
         * gets back, several frames later, as a fault with nothing to say what
         * happened. Say it here instead. */
        static size_t total;
        fprintf(stderr, "[libc] out of memory: %zu bytes (align %zu), %zu MB handed out so far\n",
                n, align, total >> 20);
        fflush(stderr);
        return NULL;
    }
    {
        static size_t total_ok;
        total_ok += n;
    }

    uintptr_t raw = (uintptr_t)base + hdr;
    uintptr_t aligned = (raw + align - 1) & ~(uintptr_t)(align - 1);

    AllocHdr *h = (AllocHdr *)(aligned - hdr);
    h->base  = base;
    h->size  = n;
    h->magic = ALLOC_MAGIC;
    return (void *)aligned;
}

static AllocHdr *hdr_of(void *p)
{
    AllocHdr *h = (AllocHdr *)((uintptr_t)p - sizeof(AllocHdr));
    return h->magic == ALLOC_MAGIC ? h : NULL;
}

static void gfree(void *p)
{
    if (!p) return;
    AllocHdr *h = hdr_of(p);
    if (!h) {
        fprintf(stderr, "[libc] free(%p): not from this allocator, ignored\n", p);
        return;
    }
    h->magic = 0;                       /* catch a double free as a bad pointer */
    free(h->base);
}

static void *grealloc(void *p, size_t n)
{
    if (!p) return gmalloc(n, GUEST_ALIGN);
    AllocHdr *h = hdr_of(p);
    if (!h) {
        fprintf(stderr, "[libc] realloc(%p): not from this allocator\n", p);
        return NULL;
    }
    size_t old = h->size;
    void *q = gmalloc(n, GUEST_ALIGN);
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    gfree(p);
    return q;
}

/* LINDBERGH_ALLOC=1 reports every allocation and what came back.
 *
 * A game that builds its own heap manager on top of malloc asks for a handful
 * of large blocks at start up and then never calls it again. If one of those
 * comes back null the game does not say so - it carries the null into its own
 * allocator and faults somewhere else entirely, with a backtrace that points
 * at the heap code rather than at the request that failed. */
static void alloc_log(const char *who, uint32_t n, uint32_t got)
{
    static int shown;
    if (shown >= 40 || !getenv("LINDBERGH_ALLOC")) return;
    shown++;
    fprintf(stderr, "[alloc] %-9s %8u bytes -> 0x%08X%s\n",
            who, n, got, got ? "" : "   *** NULL ***");
    fflush(stderr);
}

static void h_malloc(CPU *c)
{
    uint32_t n = A32(0), got = (uint32_t)(uintptr_t)gmalloc(n, GUEST_ALIGN);
    alloc_log("malloc", n, got);
    RET(got);
}
static void h_realloc (CPU *c) { RET(grealloc(APTR(0), A32(1))); }
static void h_free    (CPU *c) { gfree(APTR(0)); }

/* ---- C++ allocation ----
 *
 * operator new and its friends, which a C++ game reaches for constantly and
 * which are not libc. Let's Go Jungle imports none of these - its engine
 * allocates through its own pools - so nothing here needed them until Ghost
 * Squad Evolution, which imports all four. An unbound operator new returns
 * zero under permissive mode, every construction writes through a null, and
 * the game dies before it reaches its own main loop with nothing to say why.
 *
 * new never returns null in C++ - it throws - so a failure here is not
 * something the game will check for, and there is nothing useful to do about
 * it other than say so. */
static void h_op_new(CPU *c)
{
    void *p = gmalloc(A32(0), GUEST_ALIGN);
    if (!p) fprintf(stderr, "[libc] operator new(%u) failed\n", A32(0));
    RET(p);
}

static void h_op_delete(CPU *c) { gfree(APTR(0)); RET(0); }

/* isprint and friends get asked about bytes above 127, where the host's
 * locale-aware version is free to disagree with the guest's. The C locale
 * answer is the one a 2007 arcade binary was built against. */
/* The game's own assertions. Unbound, one is a silent exit; bound, it names
 * the file, the line and the expression that failed, which is the game
 * telling you exactly what it does not like about its environment. */
static void h_assert_fail(CPU *c)
{
    fprintf(stderr, "[assert] %s failed at %s:%u in %s\n",
            A32(0) ? ASTR(0) : "(null)",
            A32(1) ? ASTR(1) : "(null)",
            A32(2),
            A32(3) ? ASTR(3) : "(null)");
    fflush(stderr);
    guest_report_state("assertion failed");
    exit(44);
}

/* Defined with the maths handlers further down; these need them earlier. */
static double guest_arg_d(CPU *c, unsigned i);
static float  guest_arg_f(CPU *c, unsigned i);

static void h_atan2(CPU *c)  { RETF(atan2(guest_arg_d(c, 0), guest_arg_d(c, 2))); }
static void h_atan2f(CPU *c) { RETF(atan2f(guest_arg_f(c, 0), guest_arg_f(c, 1))); }
static void h_fmodf(CPU *c)  { RETF(fmodf(guest_arg_f(c, 0), guest_arg_f(c, 1))); }
static void h_sqrt(CPU *c)   { RETF(sqrt(guest_arg_d(c, 0))); }
static void h_sqrtf(CPU *c)  { RETF(sqrtf(guest_arg_f(c, 0))); }
static void h_srand(CPU *c)  { srand(A32(0)); RET(0); }
/* Guest pointers are checked before they reach a CRT function. A null here
 * is not a crash in the guest's world - it is on Windows, and a silent one. */
static void h_clearerr(CPU *c) { if (APTR(0)) clearerr((FILE *)APTR(0)); RET(0); }
static void h_sysconf(CPU *c)  { RET(A32(0) == 30 ? 4096u : 1u); }  /* _SC_PAGESIZE */
static void h_sleep(CPU *c)    { Sleep(A32(0) * 1000u); RET(0); }
static void h_strcasecmp(CPU *c)
{
    const char *a = A32(0) ? ASTR(0) : "", *b = A32(1) ? ASTR(1) : "";
    RET((uint32_t)_stricmp(a, b));
}
static void h_strncasecmp(CPU *c)
{
    const char *a = A32(0) ? ASTR(0) : "", *b = A32(1) ? ASTR(1) : "";
    RET((uint32_t)_strnicmp(a, b, A32(2)));
}

/* qsort, with the comparison living in lifted code.
 *
 * The callback is the whole difficulty: it is a guest function, so every
 * comparison is a call back across the seam. An insertion sort keeps that
 * simple and needs no scratch beyond one element - these are short arrays
 * that a game sorts once, not a hot path, and a subtle quicksort that calls
 * into the guest from inside its own recursion is not worth the risk. */
static void h_qsort(CPU *c)
{
    uint32_t base = A32(0), n = A32(1), sz = A32(2), cmp = A32(3);
    /* Every one of these comes from the guest, and a wrong one does not fail
     * politely: the sort walks off the end of what it was given and writes
     * over the host's own memory, which ends as a fail-fast with no handler
     * and nothing printed. Bound them all. */
    if (getenv("LINDBERGH_ALLOC"))
        fprintf(stderr, "[libc] qsort base 0x%08X n %u size %u cmp 0x%08X\n",
                base, n, sz, cmp);
    if (!base || !cmp || sz == 0 || n < 2) { RET(0); return; }
    if (n > (1u << 20) || (unsigned long long)n * sz > (64ull << 20)) {
        fprintf(stderr, "[libc] qsort refused: %u elements of %u bytes\n", n, sz);
        RET(0);
        return;
    }
    if (sz > 4096) { fprintf(stderr, "[libc] qsort element %u bytes, refusing\n", sz);
                     RET(0); return; }

    unsigned char *tmp = (unsigned char *)malloc(sz);
    if (!tmp) { RET(0); return; }
    unsigned char *a = (unsigned char *)(uintptr_t)base;

    for (uint32_t i = 1; i < n; i++) {
        memcpy(tmp, a + (size_t)i * sz, sz);
        uint32_t j = i;
        while (j > 0) {
            uint32_t args[2];
            args[0] = base + (uint32_t)((j - 1) * sz);
            args[1] = (uint32_t)(uintptr_t)tmp;
            if ((int32_t)guest_call(c, cmp, args, 2) <= 0) break;
            memcpy(a + (size_t)j * sz, a + (size_t)(j - 1) * sz, sz);
            j--;
        }
        memcpy(a + (size_t)j * sz, tmp, sz);
    }
    free(tmp);
    RET(0);
}

static void h_atanf(CPU *c) { RETF(atanf(guest_arg_f(c, 0))); }
static void h_powf(CPU *c)  { RETF(powf(guest_arg_f(c, 0), guest_arg_f(c, 1))); }
static void h_rewind(CPU *c) { if (APTR(0)) rewind((FILE *)APTR(0)); RET(0); }
static void h_isdigit(CPU *c){ uint32_t ch = A32(0); RET(ch >= '0' && ch <= '9'); }

static void h_isprint(CPU *c) { uint32_t ch = A32(0); RET(ch >= 0x20 && ch < 0x7F); }

/* nanosleep takes a timespec; the guest lays it out as two longs. */
static void h_nanosleep(CPU *c)
{
    uint32_t req = A32(0);
    if (req) {
        uint32_t sec = rd32(req), nsec = rd32(req + 4);
        DWORD ms = sec * 1000u + nsec / 1000000u;
        Sleep(ms ? ms : 1);
    }
    RET(0);
}
static void h_memalign(CPU *c) { RET(gmalloc(A32(1), A32(0))); }

static void h_calloc(CPU *c)
{
    size_t n = (size_t)A32(0) * A32(1);
    void *p = gmalloc(n, GUEST_ALIGN);
    if (p) memset(p, 0, n);
    RET(p);
}

/* A pointer the guest supplied, checked before the host writes through it.
 *
 * This is a trust boundary: every address here was computed by recompiled
 * code, and a single mis-lifted instruction upstream turns memcpy into a tool
 * for scribbling over the host's own stacks and heap. The failure then lands
 * somewhere else entirely, long afterwards, as a corrupted cookie or a
 * corrupted arena, with nothing left to say where it came from.
 *
 * The guest's allocations come from the host heap and so have no tidy range to
 * check against. What is checkable is the shape of a plainly broken pointer:
 * the bottom of the address space is never mapped, and no call in this game
 * moves a quarter of a gigabyte. */
#define GUEST_LOW  0x10000u
#define GUEST_HUGE (256u << 20)

static int sane(const char *who, uint32_t va, uint32_t len)
{
    if (va < GUEST_LOW || len > GUEST_HUGE) {
        fprintf(stderr, "[libc] %s(%#010x, %u bytes) refused - bad pointer or size\n",
                who, va, len);
        fflush(stderr);
        return 0;
    }
    return 1;
}

static void h_memcpy(CPU *c)
{
    if (!sane("memcpy dst", A32(0), A32(2)) || !sane("memcpy src", A32(1), A32(2))) { RET(A32(0)); return; }
    RET(memcpy(APTR(0), APTR(1), A32(2)));
}
static void h_memmove(CPU *c)
{
    if (!sane("memmove dst", A32(0), A32(2)) || !sane("memmove src", A32(1), A32(2))) { RET(A32(0)); return; }
    RET(memmove(APTR(0), APTR(1), A32(2)));
}
static void h_memset(CPU *c)
{
    if (!sane("memset", A32(0), A32(2))) { RET(A32(0)); return; }
    RET(memset(APTR(0), AI32(1), A32(2)));
}
static void h_memchr (CPU *c) { RET(memchr(APTR(0), AI32(1), A32(2))); }

/* ---- strings ---- */
static void h_strlen  (CPU *c) { RET(strlen(ASTR(0))); }
static void h_strcpy  (CPU *c) { RET(strcpy(ASTR(0), ASTR(1))); }
static void h_strncpy (CPU *c) { RET(strncpy(ASTR(0), ASTR(1), A32(2))); }
static void h_strcat  (CPU *c) { RET(strcat(ASTR(0), ASTR(1))); }
static void h_strncat (CPU *c) { RET(strncat(ASTR(0), ASTR(1), A32(2))); }
static void h_strcmp  (CPU *c) { RET(strcmp(ASTR(0), ASTR(1))); }
static void h_strncmp (CPU *c) { RET(strncmp(ASTR(0), ASTR(1), A32(2))); }
static void h_strchr  (CPU *c) { RET(strchr(ASTR(0), AI32(1))); }
static void h_strrchr (CPU *c) { RET(strrchr(ASTR(0), AI32(1))); }
static void h_strstr  (CPU *c) { RET(strstr(ASTR(0), ASTR(1))); }
static void h_strerror(CPU *c) { RET(strerror(AI32(0))); }

static void h_strdup(CPU *c)
{
    /* Not the host strdup: the guest hands the result to its own free(), which
     * is this file's free(), so it must come from this file's malloc(). */
    const char *s = ASTR(0);
    size_t n = strlen(s) + 1;
    char *p = (char *)gmalloc(n, GUEST_ALIGN);
    if (p) memcpy(p, s, n);
    RET(p);
}

/* glibc exports plain index/rindex as aliases of strchr/strrchr. */
static void h_index(CPU *c) { RET(strchr(ASTR(0), AI32(1))); }

/* ---- formatted output ----
 *
 * printf cannot be forwarded to the host, because the arguments are on the
 * GUEST stack and there is no portable way to build a host va_list from them.
 * So the format string is walked here and each directive handed to the host
 * snprintf on its own, with the one argument it consumes pulled off the guest
 * stack by hand. That keeps the host's formatting for the fiddly parts - field
 * width, precision, %g - without ever needing a real va_list.
 */
static int guest_format(char *out, size_t cap, const char *fmt, uint32_t argbase)
{
    unsigned argi = 0;      /* index into the argument list at argbase */
    size_t n = 0;
    char spec[64];

#define PUT(ch)     do { if (n + 1 < cap) out[n] = (char)(ch); n++; } while (0)
#define APPEND(str) do { const char *_p = (str); while (*_p) PUT(*_p++); } while (0)

    while (*fmt) {
        if (*fmt != 37) { PUT(*fmt++); continue; }        /* 37 is '%' */

        const char *start = fmt++;
        if (*fmt == 37) { PUT(37); fmt++; continue; }

        int stars = 0, longlong = 0;
        while (*fmt && strchr("-+ #0", *fmt)) fmt++;                 /* flags */
        while (*fmt == 42 || isdigit((unsigned char)*fmt)) {         /* width */
            if (*fmt == 42) stars++;
            fmt++;
        }
        if (*fmt == 46) {                                            /* precision */
            fmt++;
            while (*fmt == 42 || isdigit((unsigned char)*fmt)) {
                if (*fmt == 42) stars++;
                fmt++;
            }
        }
        while (*fmt && strchr("hlLqjzt", *fmt)) {                    /* length */
            if ((*fmt == 108 && fmt[1] == 108) || *fmt == 113 || *fmt == 76)
                longlong = 1;                                        /* ll, q, L */
            fmt++;
        }
        char conv = *fmt ? *fmt++ : 0;

        size_t len = (size_t)(fmt - start);
        if (!conv || len >= sizeof spec - 4) { APPEND("<badfmt>"); continue; }
        memcpy(spec, start, len);
        spec[len] = 0;

        /* A star takes its width from the argument list, ahead of the value.
         * Rather than reproduce that plumbing, fold each one into the spec as
         * a literal so the host only ever sees a fixed width. */
        for (int i = 0; i < stars; i++) {
            char fixed[96];
            char *st = strchr(spec, 42);
            if (!st) break;
            snprintf(fixed, sizeof fixed, "%.*s%d%s",
                     (int)(st - spec), spec, (int)rd32(argbase + 4u * argi++), st + 1);
            snprintf(spec, sizeof spec, "%s", fixed);
        }

        char piece[512];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c':
            if (longlong) {
                unsigned long long v = (unsigned long long)rd32(argbase + 4u * argi) |
                                       ((unsigned long long)rd32(argbase + 4u * (argi + 1)) << 32);
                argi += 2;
                char *q = strchr(spec, 113);     /* glibc %q -> %l, MSVC knows %ll */
                if (q) *q = 108;
                snprintf(piece, sizeof piece, spec, v);
            } else {
                snprintf(piece, sizeof piece, spec, (int)rd32(argbase + 4u * argi++));
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            /* A double is 8 bytes on the guest stack whether the call site
             * wrote a float or not - C promotes it in a variadic call. */
            uint64_t bits = (uint64_t)rd32(argbase + 4u * argi) | ((uint64_t)rd32(argbase + 4u * (argi + 1)) << 32);
            double d;
            argi += 2;
            memcpy(&d, &bits, 8);
            snprintf(piece, sizeof piece, spec, d);
            break;
        }
        case 's': {
            const char *sv = (const char *)(uintptr_t)rd32(argbase + 4u * argi++);
            snprintf(piece, sizeof piece, spec, sv ? sv : "(null)");
            break;
        }
        case 'p':
            snprintf(piece, sizeof piece, "0x%08X", rd32(argbase + 4u * argi++));
            break;
        case 'n':
            argi++;                          /* refuse, but still eat the pointer */
            piece[0] = 0;
            break;
        default:
            snprintf(piece, sizeof piece, "<bad %c>", conv);
            break;
        }
        APPEND(piece);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
#undef PUT
#undef APPEND
    return (int)n;
}

/* One scratch buffer. The guest is single-threaded through these, and a 16 KB
 * automatic in a deep mechanically-translated call chain is a stack overflow
 * waiting to happen.
 * ponytail: make it thread-local if two guest threads ever format at once. */
static char g_fmtbuf[16384];

static void h_printf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(0), c->esp + 4u);
    fputs(g_fmtbuf, stdout);
    RET(n);
}
static void h_fprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), c->esp + 8u);
    fputs(g_fmtbuf, (FILE *)APTR(0));
    RET(n);
}
static void h_sprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), c->esp + 8u);
    /* guest_format reports the length the format WOULD have produced, which is
     * what sprintf returns - but only what fit is in the buffer, so copying n
     * bytes would read past it. */
    size_t have = strlen(g_fmtbuf);
    memcpy(ASTR(0), g_fmtbuf, have + 1);
    RET(n);
}
static void h_snprintf(CPU *c)
{
    uint32_t cap = A32(1);
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(2), c->esp + 12u);
    if (cap) {
        size_t take = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
        memcpy(ASTR(0), g_fmtbuf, take);
        ASTR(0)[take] = 0;
    }
    RET(n);                              /* the length it WOULD have been */
}

/* ---- stdio ---- */
static void h_fopen (CPU *c) { RET(fopen(ASTR(0), ASTR(1))); }
static void h_fclose(CPU *c) { RET(fclose((FILE *)APTR(0))); }
static void h_fread (CPU *c) { RET(fread(APTR(0), A32(1), A32(2), (FILE *)APTR(3))); }
static void h_fwrite(CPU *c) { RET(fwrite(APTR(0), A32(1), A32(2), (FILE *)APTR(3))); }
static void h_fseek (CPU *c) { RET(fseek((FILE *)APTR(0), AI32(1), AI32(2))); }
static void h_ftell (CPU *c) { RET(ftell((FILE *)APTR(0))); }
static void h_fflush(CPU *c) { RET(fflush(APTR(0) ? (FILE *)APTR(0) : NULL)); }
static void h_feof  (CPU *c) { RET(feof((FILE *)APTR(0))); }
static void h_ferror(CPU *c) { RET(ferror((FILE *)APTR(0))); }
static void h_fgets (CPU *c) { RET(fgets(ASTR(0), AI32(1), (FILE *)APTR(2))); }
static void h_fputs (CPU *c) { RET(fputs(ASTR(0), (FILE *)APTR(1))); }
static void h_fputc (CPU *c) { RET(fputc(AI32(0), (FILE *)APTR(1))); }
static void h_puts  (CPU *c) { RET(puts(ASTR(0))); }

/* ---- process and environment ---- */
static void h_exit(CPU *c)
{
    /* Worth announcing. A game that decides to leave looks exactly like a
     * game that crashed, unless it says so. */
    fprintf(stderr, "[guest] exit(%d)\n", AI32(0));
    fflush(stderr);
    exit(AI32(0));
}
static void h_getenv(CPU *c)
{
    const char *k = ASTR(0);
    const char *v = getenv(k);
    fprintf(stderr, "[env] %s = %s\n", k, v ? v : "(unset)");
    fflush(stderr);
    RET(v);
}
static void h_getpid(CPU *c) { (void)c; RET(1); }
static void h_rand  (CPU *c) { (void)c; RET(rand()); }

static void h_abort(CPU *c)
{
    (void)c;
    fprintf(stderr, "[guest] called abort()\n");
    abort();
}
static void h_time(CPU *c)
{
    uint32_t t = (uint32_t)time(NULL);
    if (A32(0)) wr32(A32(0), t);
    RET(t);
}
static void h_gettimeofday(CPU *c)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    if (A32(0)) {
        wr32(A32(0), (uint32_t)ts.tv_sec);
        wr32(A32(0) + 4, (uint32_t)(ts.tv_nsec / 1000));
    }
    RET(0);
}
static void h_usleep(CPU *c)
{
#ifdef _WIN32
    Sleep(A32(0) / 1000);
#else
    usleep(A32(0));
#endif
    RET(0);
}

/* ---- math ----
 *
 * Every one of these returns in st(0). The single-precision variants compute
 * in float and widen only at the end: the guest called sinf and expects a
 * float's worth of precision, and a double-precision sine differs from it. */
static double guest_arg_d(CPU *c, unsigned i)
{
    uint64_t bits = (uint64_t)A32(i) | ((uint64_t)A32(i + 1) << 32);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
static float guest_arg_f(CPU *c, unsigned i)
{
    uint32_t bits = A32(i);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

#define MATH1(nm, fn)  static void h_##nm(CPU *c) { RETF(fn(guest_arg_d(c, 0))); }
#define MATH1F(nm, fn) static void h_##nm(CPU *c) { RETF(fn(guest_arg_f(c, 0))); }

MATH1(acos, acos)    MATH1F(acosf, acosf)
MATH1(asin, asin)    MATH1F(asinf, asinf)
MATH1(cos,  cos)     MATH1F(cosf,  cosf)
MATH1(sin,  sin)     MATH1F(sinf,  sinf)
MATH1(tan,  tan)     MATH1F(tanf,  tanf)
MATH1(cosh, cosh)    MATH1(sinh, sinh)    MATH1(tanh, tanh)
MATH1(exp,  exp)     MATH1F(expf,  expf)
MATH1(log,  log)     MATH1F(logf,  logf)
MATH1(log10, log10)

static void h_pow(CPU *c) { RETF(pow(guest_arg_d(c, 0), guest_arg_d(c, 2))); }
static void h_finitef(CPU *c) { RET(isfinite(guest_arg_f(c, 0)) ? 1 : 0); }

static void h_frexp(CPU *c)
{
    int e = 0;
    double m = frexp(guest_arg_d(c, 0), &e);
    if (A32(2)) wr32(A32(2), (uint32_t)e);
    RETF(m);
}
static void h_modf(CPU *c)
{
    double ip = 0.0;
    double fp = modf(guest_arg_d(c, 0), &ip);
    if (A32(2)) {
        uint64_t b;
        memcpy(&b, &ip, 8);
        wr32(A32(2), (uint32_t)b);
        wr32(A32(2) + 4, (uint32_t)(b >> 32));
    }
    RETF(fp);
}

/* ---- glibc internals a compiled binary calls directly ----
 *
 * __ctype_b_loc() returns a pointer TO a pointer into the middle of a
 * 384-entry table indexable from -128 to 255. isalpha() and friends are macros
 * in the game's own headers that index straight out of it, so the double
 * indirection and the negative half both have to be there or the game reads
 * garbage and every character classification is wrong. */
static unsigned short g_ctype_b[384];
static int32_t g_ctype_lower[384], g_ctype_upper[384];
static const unsigned short *g_ctype_b_p     = g_ctype_b + 128;
static const int32_t        *g_ctype_lower_p = g_ctype_lower + 128;
static const int32_t        *g_ctype_upper_p = g_ctype_upper + 128;

static void ctype_init(void)
{
    /* glibc's own bit assignments, from its <ctype.h>. */
    enum { ISupper = 1 << 8, ISlower = 1 << 9, ISalpha = 1 << 10,
           ISdigit = 1 << 11, ISxdigit = 1 << 12, ISspace = 1 << 13,
           ISprint = 1 << 14, ISgraph = 1 << 15, ISblank = 1 << 0,
           IScntrl = 1 << 1, ISpunct = 1 << 2, ISalnum = 1 << 3 };
    for (int i = -128; i < 256; i++) {
        int ch = i & 0xFF;
        unsigned short f = 0;
        if (isupper(ch))  f |= ISupper;
        if (islower(ch))  f |= ISlower;
        if (isalpha(ch))  f |= ISalpha;
        if (isdigit(ch))  f |= ISdigit;
        if (isxdigit(ch)) f |= ISxdigit;
        if (isspace(ch))  f |= ISspace;
        if (isprint(ch))  f |= ISprint;
        if (isgraph(ch))  f |= ISgraph;
        if (iscntrl(ch))  f |= IScntrl;
        if (ispunct(ch))  f |= ISpunct;
        if (isalnum(ch))  f |= ISalnum;
        if (ch == 32 || ch == 9) f |= ISblank;
        g_ctype_b[i + 128]     = (i < 0) ? 0 : f;
        g_ctype_lower[i + 128] = (i < 0) ? i : tolower(ch);
        g_ctype_upper[i + 128] = (i < 0) ? i : toupper(ch);
    }
}

static void h_ctype_b_loc      (CPU *c) { RET(&g_ctype_b_p); }
static void h_ctype_tolower_loc(CPU *c) { RET(&g_ctype_lower_p); }
static void h_ctype_toupper_loc(CPU *c) { RET(&g_ctype_upper_p); }

static int32_t g_guest_errno;
static void h_errno_location(CPU *c) { RET(&g_guest_errno); }

/* libgcc's 64-bit division helpers. A 32-bit compiler emits calls to these
 * for any long long divide, so they turn up in ordinary game code. */
static void h_divdi3(CPU *c)
{
    int64_t a = (int64_t)((uint64_t)A32(0) | ((uint64_t)A32(1) << 32));
    int64_t b = (int64_t)((uint64_t)A32(2) | ((uint64_t)A32(3) << 32));
    int64_t r = b ? a / b : 0;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)((uint64_t)r >> 32);
}
static void h_udivdi3(CPU *c)
{
    uint64_t a = (uint64_t)A32(0) | ((uint64_t)A32(1) << 32);
    uint64_t b = (uint64_t)A32(2) | ((uint64_t)A32(3) << 32);
    uint64_t r = b ? a / b : 0;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)(r >> 32);
}
static void h_umoddi3(CPU *c)
{
    uint64_t a = (uint64_t)A32(0) | ((uint64_t)A32(1) << 32);
    uint64_t b = (uint64_t)A32(2) | ((uint64_t)A32(3) << 32);
    uint64_t r = b ? a % b : 0;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)(r >> 32);
}

/* ---- the CRT entry point ----
 *
 * glibc's i386 _start pushes seven arguments and calls this:
 *
 *   __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
 *
 * and it never returns - the process ends by this calling exit(). `init` is
 * __libc_csu_init, which walks .init_array and runs every C++ static
 * constructor in the binary; for a game built on Xerces and Cg that is where
 * most of the work before main() happens, and getting it wrong means main()
 * runs against uninitialised globals rather than crashing honestly.
 */
static void h_libc_start_main(CPU *c)
{
    uint32_t main_fn = A32(0), argc = A32(1), argv = A32(2);
    uint32_t init = A32(3), fini = A32(4);

    /* envp follows argv's NULL terminator on the stack the kernel built. */
    uint32_t envp = argv + (argc + 1) * 4;
    uint32_t args[3] = { argc, argv, envp };

    if (init) {
        fprintf(stderr, "[crt] __libc_csu_init at %#010x\n", init);
        guest_call(c, init, args, 3);
    }

    fprintf(stderr, "[crt] main at %#010x (argc=%u)\n", main_fn, argc);
    uint32_t rc = guest_call(c, main_fn, args, 3);
    fprintf(stderr, "[crt] main returned %d\n", (int)rc);

    if (fini)
        guest_call(c, fini, NULL, 0);

    exit((int)rc);
}

/* ---- libgcc's exception-frame registry ----
 *
 * __register_frame_info_bases(begin, ob, tbase, dbase) hands libgcc the
 * .eh_frame section so a later throw can walk it. Nothing here unwinds DWARF -
 * the _Unwind_* family is unimplemented on purpose - so recording the pointer
 * would serve nobody, and a no-op is the honest translation rather than a
 * shortcut. If this game ever throws, it will stop in _Unwind_RaiseException
 * with its own name on it, which is the right place to find out.
 *
 * The deregister form returns the object it was given; the CRT stores that
 * result, so returning 0 would be a lie it might notice. */
static void h_register_frame_info_bases(CPU *c) { RET(0); }
static void h_deregister_frame_info_bases(CPU *c) { RET(A32(0)); }

/* Called from exit() to release glibc's own caches. There are none. */
static void h_libc_freeres(CPU *c) { (void)c; }

/* The game only ever asks for the C locale, and that is the one we are in. */
static void h_setlocale(CPU *c) { RET("C"); }

/* ---- where the game thinks it is ----
 *
 * The game calls these to work out its own install directory and then builds
 * every data path from the result, so a wrong answer here is a wrong answer
 * for every file it opens afterwards. Both return forward-slash-free Windows
 * paths, which the guest never inspects - it only concatenates - so no
 * translation is needed. */
static void h_getcwd(CPU *c)
{
#ifdef _WIN32
    RET(_getcwd(ASTR(0), AI32(1)));
#else
    RET(getcwd(ASTR(0), A32(1)));
#endif
}

static void h_realpath(CPU *c)
{
#ifdef _WIN32
    /* _fullpath allocates when handed NULL, same as realpath does; when the
     * guest supplies a buffer it must be PATH_MAX, which is what it passes. */
    RET(_fullpath(A32(1) ? ASTR(1) : NULL, ASTR(0), 260));
#else
    RET(realpath(ASTR(0), A32(1) ? ASTR(1) : NULL));
#endif
}

void hle_register_libc(void)
{
    ctype_init();

    hle_bind("malloc", h_malloc);        hle_bind("calloc", h_calloc);
    hle_bind("realloc", h_realloc);      hle_bind("free", h_free);

    /* C++ allocation. Mangled, but plain C-linkage symbols as far as the PLT
     * is concerned, so they bind by name like anything else. */
    hle_bind("_Znwj", h_op_new);         hle_bind("_Znaj", h_op_new);
    hle_bind("_ZdlPv", h_op_delete);     hle_bind("_ZdaPv", h_op_delete);
    hle_bind("isprint", h_isprint);      hle_bind("nanosleep", h_nanosleep);
    hle_bind("__assert_fail", h_assert_fail);
    hle_bind("atanf", h_atanf);          hle_bind("powf", h_powf);
    hle_bind("rewind", h_rewind);        hle_bind("isdigit", h_isdigit);
    hle_bind("atan2", h_atan2);          hle_bind("atan2f", h_atan2f);
    hle_bind("fmodf", h_fmodf);          hle_bind("qsort", h_qsort);
    hle_bind("sqrt", h_sqrt);            hle_bind("sqrtf", h_sqrtf);
    hle_bind("srand", h_srand);          hle_bind("clearerr", h_clearerr);
    hle_bind("sysconf", h_sysconf);      hle_bind("sleep", h_sleep);
    hle_bind("strcasecmp", h_strcasecmp);
    hle_bind("strncasecmp", h_strncasecmp);
    hle_bind("memcpy", h_memcpy);        hle_bind("memmove", h_memmove);
    hle_bind("memset", h_memset);        hle_bind("memchr", h_memchr);
    hle_bind("memalign", h_memalign);

    hle_bind("strlen", h_strlen);        hle_bind("strcpy", h_strcpy);
    hle_bind("strncpy", h_strncpy);      hle_bind("strcat", h_strcat);
    hle_bind("strncat", h_strncat);      hle_bind("strcmp", h_strcmp);
    hle_bind("strncmp", h_strncmp);      hle_bind("strchr", h_strchr);
    hle_bind("strrchr", h_strrchr);      hle_bind("strstr", h_strstr);
    hle_bind("strdup", h_strdup);        hle_bind("strerror", h_strerror);
    hle_bind("index", h_index);

    hle_bind("printf", h_printf);        hle_bind("fprintf", h_fprintf);
    hle_bind("sprintf", h_sprintf);      hle_bind("snprintf", h_snprintf);

    hle_bind("fopen", h_fopen);          hle_bind("fclose", h_fclose);
    hle_bind("fread", h_fread);          hle_bind("fwrite", h_fwrite);
    hle_bind("fseek", h_fseek);          hle_bind("ftell", h_ftell);
    hle_bind("fflush", h_fflush);        hle_bind("feof", h_feof);
    hle_bind("ferror", h_ferror);        hle_bind("fgets", h_fgets);
    hle_bind("fputs", h_fputs);          hle_bind("fputc", h_fputc);
    hle_bind("puts", h_puts);

    hle_bind("exit", h_exit);            hle_bind("_exit", h_exit);
    hle_bind("abort", h_abort);          hle_bind("getenv", h_getenv);
    hle_bind("getpid", h_getpid);        hle_bind("rand", h_rand);
    hle_bind("time", h_time);            hle_bind("usleep", h_usleep);
    hle_bind("gettimeofday", h_gettimeofday);

    hle_bind("acos", h_acos);    hle_bind("acosf", h_acosf);
    hle_bind("asin", h_asin);    hle_bind("asinf", h_asinf);
    hle_bind("cos", h_cos);      hle_bind("cosf", h_cosf);
    hle_bind("sin", h_sin);      hle_bind("sinf", h_sinf);
    hle_bind("tan", h_tan);      hle_bind("tanf", h_tanf);
    hle_bind("cosh", h_cosh);    hle_bind("sinh", h_sinh);
    hle_bind("tanh", h_tanh);    hle_bind("exp", h_exp);
    hle_bind("expf", h_expf);    hle_bind("log", h_log);
    hle_bind("logf", h_logf);    hle_bind("log10", h_log10);
    hle_bind("pow", h_pow);      hle_bind("finitef", h_finitef);
    hle_bind("frexp", h_frexp);  hle_bind("modf", h_modf);

    hle_bind("__ctype_b_loc", h_ctype_b_loc);
    hle_bind("__ctype_tolower_loc", h_ctype_tolower_loc);
    hle_bind("__ctype_toupper_loc", h_ctype_toupper_loc);
    hle_bind("__errno_location", h_errno_location);
    hle_bind("__libc_start_main", h_libc_start_main);
    hle_bind("__register_frame_info_bases", h_register_frame_info_bases);
    hle_bind("__deregister_frame_info_bases", h_deregister_frame_info_bases);
    hle_bind("__libc_freeres", h_libc_freeres);
    hle_bind("setlocale", h_setlocale);
    hle_bind("getcwd", h_getcwd);
    hle_bind("realpath", h_realpath);
    hle_bind("__divdi3", h_divdi3);
    hle_bind("__udivdi3", h_udivdi3);
    hle_bind("__umoddi3", h_umoddi3);
}

/* ---- files, wide characters, and glibc's versioned internals ----
 *
 * The game reaches these once it starts loading data: stat to find files,
 * strtol to parse its configuration, and the wide-character set because its
 * text handling is locale-aware. */

/* glibc never exported `stat` itself - the header turns it into __xstat with a
 * struct-version argument, so that is what a binary imports. The struct is the
 * kernel's stat64 layout, and the game reads st_mode and st_size from it. */
#define GST_mode 16
#define GST_size 44

static void xstat_common(CPU *c, const char *path, uint32_t out)
{
    struct _stat64 st;
    if (!path || _stat64(path, &st) != 0) { RET(-1); return; }
    if (out) {
        memset((void *)(uintptr_t)out, 0, 88);          /* sizeof(struct stat64) */
        wr32(out + GST_mode, (uint32_t)st.st_mode);
        wr32(out + GST_size, (uint32_t)st.st_size);
    }
    RET(0);
}

/* __xstat(version, path, buf) - the version is glibc's, and nothing here
 * varies by it. */
static void h_xstat(CPU *c)  { xstat_common(c, ASTR(1), A32(2)); }
static void h_lxstat(CPU *c) { xstat_common(c, ASTR(1), A32(2)); }

static void h_access(CPU *c) { RET(_access(ASTR(0), AI32(1))); }

/* strtol and strtoul are exported under these names with a `group` argument
 * that only affects locale-specific digit grouping, which no game uses. */
static void h_strtol_internal(CPU *c)
{
    RET(strtol(ASTR(0), (char **)(uintptr_t)A32(1), AI32(2)));
}
static void h_strtoul_internal(CPU *c)
{
    RET(strtoul(ASTR(0), (char **)(uintptr_t)A32(1), AI32(2)));
}
static void h_strtod_internal(CPU *c)
{
    RETF(strtod(ASTR(0), (char **)(uintptr_t)A32(1)));
}

/* Wide characters. The game is in the C locale, where the conversions are the
 * identity over ASCII and undefined above it - so these are as complete as
 * they need to be. */
static void h_wcslen(CPU *c)
{
    const uint32_t *w = (const uint32_t *)APTR(0);
    size_t n = 0;
    while (w && w[n]) n++;
    RET(n);
}
static void h_btowc(CPU *c) { int ch = AI32(0); RET(ch < 0 || ch > 0x7F ? 0xFFFFFFFFu : (uint32_t)ch); }
static void h_wctob(CPU *c) { uint32_t w = A32(0); RET(w > 0x7F ? 0xFFFFFFFFu : w); }

/* wctype("alpha") and friends return an opaque handle that iswctype then
 * interprets. The handle only has to be distinct and non-zero per class. */
static void h_wctype(CPU *c)
{
    static const char *const classes[] = {
        "alnum","alpha","blank","cntrl","digit","graph",
        "lower","print","punct","space","upper","xdigit"
    };
    const char *want = ASTR(0);
    for (unsigned i = 0; i < sizeof classes / sizeof classes[0]; i++)
        if (strcmp(want, classes[i]) == 0) { RET(i + 1); return; }
    RET(0);
}

static void h_iswctype(CPU *c)
{
    int ch = (int)A32(0);
    unsigned cls = A32(1);
    if (ch > 0x7F) { RET(0); return; }
    switch (cls) {
    case 1:  RET(isalnum(ch)); return;   case 2:  RET(isalpha(ch)); return;
    case 3:  RET(ch == ' ' || ch == '\t'); return;
    case 4:  RET(iscntrl(ch)); return;   case 5:  RET(isdigit(ch)); return;
    case 6:  RET(isgraph(ch)); return;   case 7:  RET(islower(ch)); return;
    case 8:  RET(isprint(ch)); return;   case 9:  RET(ispunct(ch)); return;
    case 10: RET(isspace(ch)); return;   case 11: RET(isupper(ch)); return;
    case 12: RET(isxdigit(ch)); return;
    default: RET(0); return;
    }
}

static void h_towlower(CPU *c) { uint32_t w = A32(0); RET(w <= 0x7F ? (uint32_t)tolower((int)w) : w); }
static void h_towupper(CPU *c) { uint32_t w = A32(0); RET(w <= 0x7F ? (uint32_t)toupper((int)w) : w); }

void hle_register_libc2(void)
{
    hle_bind("__xstat", h_xstat);
    hle_bind("__lxstat", h_lxstat);
    hle_bind("access", h_access);
    hle_bind("__strtol_internal", h_strtol_internal);
    hle_bind("__strtoul_internal", h_strtoul_internal);
    hle_bind("__strtod_internal", h_strtod_internal);
    hle_bind("wcslen", h_wcslen);
    hle_bind("btowc", h_btowc);
    hle_bind("wctob", h_wctob);
    hle_bind("wctype", h_wctype);
    hle_bind("iswctype", h_iswctype);
    hle_bind("towlower", h_towlower);
    hle_bind("towupper", h_towupper);
}

/* ---- descriptors and devices ----
 *
 * The game opens the JVS I/O board as a device and drives it with ioctl. There
 * is no such device here, so open fails and the game is told so - which is the
 * truth, and better than handing back a descriptor that answers nothing.
 */
/* The guest O_* constants are Linux's. Only the access mode and a few flags
 * ever appear, and passing them through unchanged opens the wrong mode. */
static int guest_open_flags(uint32_t f)
{
    int out = (int)(f & 3) | _O_BINARY;
    if (f & 0x40)  out |= _O_CREAT;
    if (f & 0x200) out |= _O_TRUNC;
    if (f & 0x400) out |= _O_APPEND;
    return out;
}

static void h_open(CPU *c)
{
    int fd = _open(ASTR(0), guest_open_flags(A32(1)), 0666);
    /* The cabinet's devices are the ones that matter here: a JVS I/O board
     * hangs off a serial port, and a game that cannot open it stops at an
     * error screen instead of running its attract mode. */
    if (getenv("LINDBERGH_OPENS"))
        fprintf(stderr, "[open] %s -> %d\n", ASTR(0), fd);
    RET(fd);
}
static void h_close(CPU *c) { RET(_close(AI32(0))); }
static void h_read(CPU *c)  { RET(_read(AI32(0), APTR(1), A32(2))); }
static void h_write(CPU *c) { RET(_write(AI32(0), APTR(1), A32(2))); }
static void h_lseek(CPU *c) { RET(_lseek(AI32(0), (long)AI32(1), AI32(2))); }
static void h_ioctl(CPU *c) { RET(-1); }
static void h_unlink(CPU *c) { RET(_unlink(ASTR(0))); }
static void h_mkdir(CPU *c)  { RET(_mkdir(ASTR(0))); }

/* iopl(3) asks the kernel for permission to execute IN and OUT directly.
 * Granting it would be a lie with consequences: the game would then run port
 * I/O instructions, which do not lift and cannot mean anything on a host that
 * is not the cabinet. Refusing is both true and the answer that sends it down
 * whatever path it has for not being on real hardware. */
static void h_iopl(CPU *c) { RET(-1); }

/* The engine's own console.
 *
 * _sDebug::putConsole is where this game narrates what it is doing - which
 * hardware it looked for, what answered, and why it gave up. On the cabinet
 * it goes to a debug console that is not here, so every one of those messages
 * has been thrown away. It is a printf, and a static one, so the format
 * string is the first argument and the rest follow it on the stack.
 *
 * LINDBERGH_CONSOLE=1 turns it on. Reading what the game says beats guessing
 * what it wants. */
static void h_putConsole(CPU *c)
{
    static char buf[4096];
    guest_format(buf, sizeof buf, ASTR(0), c->esp + 4u);
    fputs("[game] ", stderr);
    fputs(buf, stderr);
    if (!*buf || buf[strlen(buf) - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);
    RET(0);
}

void hle_register_console(void)
{
    if (!getenv("LINDBERGH_CONSOLE")) return;
    if (guest_override("_ZN7_sDebug10putConsoleEPKcz", h_putConsole))
        fprintf(stderr, "[console] engine console messages enabled\n");
}

void hle_register_io(void)
{
    hle_bind("open", h_open);       hle_bind("close", h_close);
    hle_bind("read", h_read);       hle_bind("write", h_write);
    hle_bind("lseek", h_lseek);     hle_bind("ioctl", h_ioctl);
    hle_bind("unlink", h_unlink);   hle_bind("mkdir", h_mkdir);
    hle_bind("iopl", h_iopl);
}

/* ---- the v* formatters ----
 *
 * A va_list on i386 is nothing but a pointer into the caller's stack, so these
 * are the same formatter with the argument base taken from the guest rather
 * than computed from esp. That is the whole reason guest_format takes an
 * address. */
static void h_vsprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), A32(2));
    size_t have = strlen(g_fmtbuf);
    memcpy(ASTR(0), g_fmtbuf, have + 1);
    RET(n);
}
static void h_vsnprintf(CPU *c)
{
    uint32_t cap = A32(1);
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(2), A32(3));
    if (cap) {
        size_t have = strlen(g_fmtbuf);
        size_t take = have < cap - 1 ? have : cap - 1;
        memcpy(ASTR(0), g_fmtbuf, take);
        ASTR(0)[take] = 0;
    }
    RET(n);
}
static void h_vfprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), A32(2));
    fputs(g_fmtbuf, (FILE *)APTR(0));
    RET(n);
}

/* ---- dynamic loading ----
 *
 * The game dlopens the Sega sound library and looks its entry points up by
 * name. It also imports those same names through its PLT, so the lookup can be
 * answered the same way glXGetProcAddressARB is: hand back the stub, which is
 * already an address dispatch() routes to a handler. The handle itself only
 * has to be non-null and recognisable. */
#define DL_HANDLE 0x444C0001u

static void h_dlopen(CPU *c)
{
    fprintf(stderr, "[dl] dlopen(%s)\n", A32(0) ? ASTR(0) : "(self)");
    RET(DL_HANDLE);
}
/* ---- symbols that live in a shared object we do not have ----
 *
 * Ghost Squad Evolution dlopens its audio library and dlsyms every ADX entry
 * point out of it. Those are not PLT imports of the game binary, so there is
 * nothing to hand back - and a game that gets a null from dlsym does not
 * check it. It calls it, and dispatch lands on address zero.
 *
 * A stub address is a better answer than a null one. These tokens live above
 * the image where nothing else can be, dispatch routes them here, and calling
 * one does nothing and returns zero - which for an audio library means a game
 * that runs in silence rather than one that does not run.
 */
#define DL_TOKEN_BASE 0xF1000000u
#define DL_TOKEN_MAX  256

static struct { const char *name; int moaned; } g_dl_tok[DL_TOKEN_MAX];
static int g_dl_tok_n;

static uint32_t dl_stub_for(const char *name)
{
    for (int i = 0; i < g_dl_tok_n; i++)
        if (strcmp(g_dl_tok[i].name, name) == 0)
            return DL_TOKEN_BASE + (uint32_t)i;      /* same name, same address */
    if (g_dl_tok_n >= DL_TOKEN_MAX) return 0;
    g_dl_tok[g_dl_tok_n].name = _strdup(name);
    return DL_TOKEN_BASE + (uint32_t)(g_dl_tok_n++);
}

/* What a stubbed library function should hand back.
 *
 * Zero is the wrong answer for the half of an API that creates things. Ghost
 * Squad Evolution asks its sound library for an ADXT handle, gets a null, and
 * does not survive using it - and a null is not what the real library would
 * ever return. A pointer to a page of zeroes is: reads see nothing set,
 * writes go somewhere harmless, and a handle check passes.
 *
 * Every stub shares the one page, so two objects from a stubbed library are
 * the same object. That is fine for a library doing nothing, and would not be
 * for one doing something - which is the point at which the answer is to lift
 * the library rather than to stub it. */
/* One of these per stubbed library function, not one shared by all of them,
 * and each big enough to be a context object rather than a page.
 *
 * A game that asks a stubbed library to create something writes into what it
 * gets back. Handing every caller the same four kilobytes means two objects
 * are one object, and a context larger than the buffer runs off the end into
 * whatever the runtime keeps next door - which is host memory, so it ends as
 * a fail-fast with no handler and nothing printed. */
#define STUB_OBJECT_SIZE (256u * 1024u)
static unsigned char *g_stub_objects[DL_TOKEN_MAX];

int hle_stub_call(CPU *c, uint32_t va)
{
    if (va < DL_TOKEN_BASE || va >= DL_TOKEN_BASE + (uint32_t)g_dl_tok_n) return 0;
    int i = (int)(va - DL_TOKEN_BASE);
    if (!g_dl_tok[i].moaned) {
        g_dl_tok[i].moaned = 1;
        fprintf(stderr, "[dl] %s() is a stub - that library is not loaded\n",
                g_dl_tok[i].name);
    }
    if (!g_stub_objects[i]) g_stub_objects[i] = (unsigned char *)calloc(1, STUB_OBJECT_SIZE);
    RET((uint32_t)(uintptr_t)g_stub_objects[i]);
    return 1;
}

static void h_dlsym(CPU *c)
{
    const char *name = ASTR(1);
    uint32_t va = hle_plt_address(name);
    if (!va) {
        va = dl_stub_for(name);
        static int shown;
        if (shown < 12) {
            shown++;
            fprintf(stderr, "[dl] dlsym(%s) -> stub at %#010x\n", name, va);
        }
    }
    RET(va);
}
static void h_dlclose(CPU *c) { RET(0); }
static void h_dlerror(CPU *c) { (void)c; RET(0); }

/* ---- time ----
 *
 * struct tm on i386 glibc: nine ints, then tm_gmtoff and tm_zone. The game
 * reads the date fields to stamp its logs. */
static int32_t g_tm[11];

static void h_localtime(CPU *c)
{
    time_t t = (time_t)(A32(0) ? rd32(A32(0)) : 0);
    struct tm lt;
    if (localtime_s(&lt, &t) != 0) { RET(0); return; }
    g_tm[0] = lt.tm_sec;   g_tm[1] = lt.tm_min;   g_tm[2] = lt.tm_hour;
    g_tm[3] = lt.tm_mday;  g_tm[4] = lt.tm_mon;   g_tm[5] = lt.tm_year;
    g_tm[6] = lt.tm_wday;  g_tm[7] = lt.tm_yday;  g_tm[8] = lt.tm_isdst;
    g_tm[9] = 0; g_tm[10] = 0;
    RET(g_tm);
}

static void h_fscanf(CPU *c);   /* defined below, bound here */

void hle_register_libc3(void)
{
    hle_bind("vsprintf", h_vsprintf);
    hle_bind("vsnprintf", h_vsnprintf);
    hle_bind("vfprintf", h_vfprintf);
    hle_bind("dlopen", h_dlopen);
    hle_bind("dlsym", h_dlsym);
    hle_bind("dlclose", h_dlclose);
    hle_bind("dlerror", h_dlerror);
    hle_bind("localtime", h_localtime);
    hle_bind("fscanf", h_fscanf);
}

/* ---- scanf ----
 *
 * The same problem as printf and a worse failure mode. A guest va_list cannot
 * be handed to the host, so the format is walked here and each directive given
 * to the host's own scanner on its own, with the single guest pointer it
 * fills. Literal and whitespace runs are passed through too, because they
 * consume input the conversions rely on.
 *
 * Leaving this unbound is not a no-op: the game reads its configuration with
 * `while (!feof(f)) fscanf(...)`, and a scanf that returns without consuming
 * anything turns that into an infinite loop. It span a hundred thousand times
 * a second and drew nothing.
 */
static int guest_scan(FILE *f, const char *fmt, CPU *c, uint32_t argbase)
{
    unsigned argi = 0;
    int filled = 0;
    char spec[64];

    while (*fmt) {
        if (*fmt != 37) {                      /* not '%': literal or space */
            const char *start = fmt;
            while (*fmt && *fmt != 37) fmt++;
            size_t len = (size_t)(fmt - start);
            if (len < sizeof spec - 1) {
                memcpy(spec, start, len);
                spec[len] = 0;
                fscanf(f, spec);               /* consumes, assigns nothing */
            }
            continue;
        }

        const char *start = fmt++;
        if (*fmt == 37) { fscanf(f, "%%"); fmt++; continue; }

        int suppress = 0;
        if (*fmt == 42) { suppress = 1; fmt++; }            /* %* */
        while (isdigit((unsigned char)*fmt)) fmt++;         /* width */
        while (*fmt && strchr("hlLqjzt", *fmt)) fmt++;      /* length */
        char conv = *fmt ? *fmt++ : 0;
        if (!conv) break;

        size_t len = (size_t)(fmt - start);
        if (len >= sizeof spec - 1) break;
        memcpy(spec, start, len);
        spec[len] = 0;

        if (suppress) { fscanf(f, spec); continue; }

        void *dst = (void *)(uintptr_t)rd32(argbase + 4u * argi++);
        if (!dst) break;
        if (fscanf(f, spec, dst) == 1) filled++;
        else break;                            /* a failed conversion ends it */
    }
    return filled;
}

static void h_fscanf(CPU *c)
{
    RET(guest_scan((FILE *)APTR(0), ASTR(1), c, c->esp + 8u));
}
