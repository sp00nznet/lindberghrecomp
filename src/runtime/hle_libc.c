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
#else
#include <unistd.h>
#endif

#include "lindbergh_rt.h"

/* ---- memory ----
 *
 * Every guest allocation goes through one family, and that is not fussiness.
 * The guest calls memalign for its SSE-aligned buffers and then releases them
 * with plain free(), exactly as glibc allows - but the host's aligned
 * allocator hands back pointers that only _aligned_free understands, and
 * feeding one to free() corrupts the heap. It does not fault where it happens
 * either; it faults later, somewhere else, as STATUS_HEAP_CORRUPTION in code
 * that did nothing wrong.
 *
 * So malloc, calloc, realloc, memalign and strdup all allocate the same way
 * and free releases it the same way. Everything gets 16-byte alignment, which
 * is what glibc's malloc gives on i386 anyway and what the game's SSE loads
 * want.
 */
#define GUEST_ALIGN 16

static void *gmalloc(size_t n, size_t align)
{
    if (align < GUEST_ALIGN) align = GUEST_ALIGN;
#ifdef _WIN32
    return _aligned_malloc(n ? n : 1, align);
#else
    void *p = NULL;
    if (posix_memalign(&p, align, n ? n : 1) != 0) p = NULL;
    return p;
#endif
}

static void gfree(void *p)
{
    if (!p) return;
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

static void *grealloc(void *p, size_t n)
{
#ifdef _WIN32
    return _aligned_realloc(p, n ? n : 1, GUEST_ALIGN);
#else
    /* No aligned realloc in POSIX; posix_memalign already meets GUEST_ALIGN
     * and realloc preserves at least malloc alignment, which is enough. */
    return realloc(p, n ? n : 1);
#endif
}

static void h_malloc (CPU *c) { RET(gmalloc(A32(0), GUEST_ALIGN)); }
static void h_realloc(CPU *c) { RET(grealloc(APTR(0), A32(1))); }
static void h_free   (CPU *c) { gfree(APTR(0)); }
static void h_memalign(CPU *c) { RET(gmalloc(A32(1), A32(0))); }

static void h_calloc(CPU *c)
{
    size_t n = (size_t)A32(0) * A32(1);
    void *p = gmalloc(n, GUEST_ALIGN);
    if (p) memset(p, 0, n);
    RET(p);
}

static void h_memcpy (CPU *c) { RET(memcpy(APTR(0), APTR(1), A32(2))); }
static void h_memmove(CPU *c) { RET(memmove(APTR(0), APTR(1), A32(2))); }
static void h_memset (CPU *c) { RET(memset(APTR(0), AI32(1), A32(2))); }
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
static int guest_format(char *out, size_t cap, const char *fmt, CPU *c, unsigned argi)
{
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
                     (int)(st - spec), spec, (int)A32(argi++), st + 1);
            snprintf(spec, sizeof spec, "%s", fixed);
        }

        char piece[512];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c':
            if (longlong) {
                unsigned long long v = (unsigned long long)A32(argi) |
                                       ((unsigned long long)A32(argi + 1) << 32);
                argi += 2;
                char *q = strchr(spec, 113);     /* glibc %q -> %l, MSVC knows %ll */
                if (q) *q = 108;
                snprintf(piece, sizeof piece, spec, v);
            } else {
                snprintf(piece, sizeof piece, spec, (int)A32(argi++));
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            /* A double is 8 bytes on the guest stack whether the call site
             * wrote a float or not - C promotes it in a variadic call. */
            uint64_t bits = (uint64_t)A32(argi) | ((uint64_t)A32(argi + 1) << 32);
            double d;
            argi += 2;
            memcpy(&d, &bits, 8);
            snprintf(piece, sizeof piece, spec, d);
            break;
        }
        case 's': {
            const char *sv = (const char *)(uintptr_t)A32(argi++);
            snprintf(piece, sizeof piece, spec, sv ? sv : "(null)");
            break;
        }
        case 'p':
            snprintf(piece, sizeof piece, "0x%08X", A32(argi++));
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
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(0), c, 1);
    fputs(g_fmtbuf, stdout);
    RET(n);
}
static void h_fprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), c, 2);
    fputs(g_fmtbuf, (FILE *)APTR(0));
    RET(n);
}
static void h_sprintf(CPU *c)
{
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(1), c, 2);
    memcpy(ASTR(0), g_fmtbuf, (size_t)n + 1);
    RET(n);
}
static void h_snprintf(CPU *c)
{
    uint32_t cap = A32(1);
    int n = guest_format(g_fmtbuf, sizeof g_fmtbuf, ASTR(2), c, 3);
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
static void h_exit  (CPU *c) { exit(AI32(0)); }
static void h_getenv(CPU *c) { RET(getenv(ASTR(0))); }
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

void hle_register_libc(void)
{
    ctype_init();

    hle_bind("malloc", h_malloc);        hle_bind("calloc", h_calloc);
    hle_bind("realloc", h_realloc);      hle_bind("free", h_free);
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
    hle_bind("__divdi3", h_divdi3);
    hle_bind("__udivdi3", h_udivdi3);
    hle_bind("__umoddi3", h_umoddi3);
}
