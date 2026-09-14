/*
 * hle_cg.c - NVIDIA Cg, answered from the disc.
 *
 * The game compiles its shaders through Cg at startup: hand cgCreateProgram
 * the Cg source, ask cgGetProgramString for the result, feed that to
 * glProgramStringARB. Implementing that honestly would mean shipping a Cg
 * compiler.
 *
 * It is not necessary, because the answers are already on the disc. Every
 * shader is there twice - shader/Cg/vs/vs.cg next to shader/Cg/vs/vs.asm_gl,
 * the second being the first as cgc 1.4 compiled it for profile vp40 in 2005.
 * So this matches the source it is given against the .cg files it can find and
 * returns the .asm_gl beside it: the same text the real Cg runtime would have
 * produced, produced by the same compiler, just earlier.
 *
 * If a source does not match anything, that is reported rather than papered
 * over - a silently empty shader draws nothing and says nothing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "lindbergh_rt.h"

#define CG_TOKEN_CTX  0x43470001u      /* "CG" */
#define MAX_SHADERS   512

typedef struct {
    char  *source;      /* the .cg text */
    char  *compiled;    /* the .asm_gl beside it */
} Shader;

static Shader g_sh[MAX_SHADERS];
static int    g_sh_n;
static int    g_indexed;

static char *read_all(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = (char *)malloc((size_t)n + 1);
    if (p && fread(p, 1, (size_t)n, f) == (size_t)n) p[n] = 0;
    else { free(p); p = NULL; }
    fclose(f);
    if (p && len_out) *len_out = (size_t)n;
    return p;
}

#ifdef _WIN32
/* Walk a directory tree for *.cg, and for each one remember the .asm_gl that
 * sits beside it. Depth-limited because the tree is shallow by construction
 * (shader/Cg/{vs,ps,inc}) and a runaway walk during startup is worse than a
 * missed shader. */
static void index_dir(const char *dir, int depth)
{
    if (depth > 4 || g_sh_n >= MAX_SHADERS) return;

    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s/*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (depth == 0) fprintf(stderr, "[cg] cannot open %s\n", dir);
        return;
    }

    do {
        if (fd.cFileName[0] == '.') continue;

        char full[MAX_PATH];
        snprintf(full, sizeof full, "%s/%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            index_dir(full, depth + 1);
            continue;
        }
        size_t n = strlen(fd.cFileName);
        if (n < 4 || strcmp(fd.cFileName + n - 3, ".cg") != 0) continue;

        char asm_path[MAX_PATH];
        snprintf(asm_path, sizeof asm_path, "%.*s.asm_gl",
                 (int)(strlen(full) - 3), full);

        char *src = read_all(full, NULL);
        char *cmp = read_all(asm_path, NULL);
        if (src && cmp && g_sh_n < MAX_SHADERS) {
            g_sh[g_sh_n].source = src;
            g_sh[g_sh_n].compiled = cmp;
            g_sh_n++;
        } else {
            free(src);
            free(cmp);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

static void index_shaders(void)
{
    if (g_indexed) return;
    g_indexed = 1;

    const char *tea = getenv("TEA_DIR");
    if (!tea || !*tea) {
        fprintf(stderr, "[cg] TEA_DIR is unset, so there is nowhere to look for\n"
                        "     precompiled shaders. Set it to the game directory.\n");
        return;
    }
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s/shader", tea);
    index_dir(dir, 0);
    snprintf(dir, sizeof dir, "%s/extraShader", tea);
    index_dir(dir, 0);
    fprintf(stderr, "[cg] indexed %d precompiled shaders\n", g_sh_n);
}

/* Ignore whitespace when matching. The game may hand over a source it has
 * reassembled - line endings changed, a trailing newline gained or lost - and
 * a byte-exact compare would miss a shader that is plainly the same one. */
static int same_source(const char *a, const char *b)
{
    for (;;) {
        while (*a && (unsigned char)*a <= ' ') a++;
        while (*b && (unsigned char)*b <= ' ') b++;
        if (!*a || !*b) return !*a && !*b;
        if (*a != *b) return 0;
        a++; b++;
    }
}
#endif

static void h_cgCreateContext(CPU *c)  { index_shaders(); RET(CG_TOKEN_CTX); }
static void h_cgIsContext(CPU *c)      { RET(A32(0) == CG_TOKEN_CTX); }
static void h_cgDestroyContext(CPU *c) { (void)c; }
static void h_cgSetErrorCallback(CPU *c) { (void)c; }
static void h_cgGetError(CPU *c)       { (void)c; RET(0); }           /* CG_NO_ERROR */
static void h_cgGetErrorString(CPU *c) { RET(""); }
static void h_cgDestroyProgram(CPU *c) { (void)c; }

/* Every profile the game asks about is one cgc already compiled for, and the
 * host driver still supports the ARB programs that came out. */
static void h_cgGLIsProfileSupported(CPU *c) { RET(1); }

/* cgCreateProgram(ctx, type, source, profile, entry, args). The returned
 * handle is an index into the shader table, offset so that zero stays the
 * failure value Cg uses. */
static void h_cgCreateProgram(CPU *c)
{
#ifdef _WIN32
    index_shaders();
    const char *src = ASTR(2);
    if (src) {
        for (int i = 0; i < g_sh_n; i++)
            if (same_source(src, g_sh[i].source)) { RET(i + 1); return; }
    }
    fprintf(stderr, "[cg] no precompiled match for a %zu-byte shader source\n",
            src ? strlen(src) : 0);
#endif
    RET(0);
}

/* cgGetProgramString(program, string_type) - the game wants the compiled
 * output, which is the .asm_gl read at index time. */
static void h_cgGetProgramString(CPU *c)
{
    int i = (int)A32(0) - 1;
    if (i >= 0 && i < g_sh_n) { RET(g_sh[i].compiled); return; }
    RET("");
}

void hle_register_cg(void)
{
    hle_bind("cgCreateContext", h_cgCreateContext);
    hle_bind("cgIsContext", h_cgIsContext);
    hle_bind("cgDestroyContext", h_cgDestroyContext);
    hle_bind("cgSetErrorCallback", h_cgSetErrorCallback);
    hle_bind("cgGetError", h_cgGetError);
    hle_bind("cgGetErrorString", h_cgGetErrorString);
    hle_bind("cgGLIsProfileSupported", h_cgGLIsProfileSupported);
    hle_bind("cgCreateProgram", h_cgCreateProgram);
    hle_bind("cgGetProgramString", h_cgGetProgramString);
    hle_bind("cgDestroyProgram", h_cgDestroyProgram);
}
