/*
 * hle_matrix.c - the engine's SSE matrix multiplies, done in C.
 *
 * The world-view-projection matrix the engine uploads has an all-zero first
 * column, which collapses every vertex onto a single vertical line: the scene
 * is submitted, rasterised and shaded, and lands in a sliver nobody can see.
 * The upload itself is faithful - the guest hands us those zeros - so the
 * matrix is computed wrong, and it is computed in two hand-written SSE
 * routines that do nothing but movups/shufps/mulps/addps.
 *
 * These overrides are first a measurement: run the same maths in C and see
 * whether the frame appears. They are cheap to keep either way, because a
 * 4x4 multiply in C is not slower than a lifted one that unpacks four lanes
 * through a union on every instruction.
 *
 * Both routines allow the destination to alias an operand - the engine calls
 * setMul(m, x, m) - which the originals handle by loading all of the second
 * operand into registers before storing anything. Computing into a local and
 * copying at the end is the same guarantee.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lindbergh_rt.h"

typedef struct { float m[4][4]; } Mat4;

static void rd_rows(uint32_t va, float *out, int rows)
{
    memcpy(out, (const void *)(uintptr_t)va, (size_t)rows * 16);
}

/* Running the same multiply in C reproduces the engine's zero row exactly, so
 * the multiply is faithful and an operand is already wrong when it arrives.
 * LINDBERGH_MATRIX=1 shows what does arrive. */
static void show(const char *tag, uint32_t va, const float *m, int rows)
{
    static int shown;
    if (shown > 90 || !getenv("LINDBERGH_MATRIX")) return;
    shown++;
    fprintf(stderr, "[mat] %s at 0x%08X:", tag, va);
    for (int i = 0; i < rows; i++)
        fprintf(stderr, " (%.4g %.4g %.4g %.4g)",
                m[i*4], m[i*4+1], m[i*4+2], m[i*4+3]);
    fprintf(stderr, "\n");
}

/* dst = a * b, row-vector convention: dst row i is a[i].x*b[0] + a[i].y*b[1]
 * + a[i].z*b[2] + a[i].w*b[3], which is exactly what the shufps broadcasts of
 * a[i] multiplied against the four rows of b were doing. */
static void mul_rows(const float *a, const float *b, float *dst, int rows,
                     const float *row3)
{
    Mat4 t;
    for (int i = 0; i < rows; i++)
        for (int c = 0; c < 4; c++)
            t.m[i][c] = a[i*4+0] * b[0*4+c] + a[i*4+1] * b[1*4+c]
                      + a[i*4+2] * b[2*4+c] + a[i*4+3] * row3[c];
    memcpy(dst, t.m, (size_t)rows * 16);
}

/* _sMatrix44::setMul(const _sMatrix44 &, const _sMatrix44 &) */
static void h_mul44(CPU *c)
{
    uint32_t dst = A32(0), a = A32(1), b = A32(2);
    float A[16], B[16], out[16];
    rd_rows(a, A, 4);
    rd_rows(b, B, 4);
    mul_rows(A, B, out, 4, B + 12);
    /* The uploaded matrix has a dead first row, and this is one of the two
     * routines that could produce it. Reporting only the multiply that does
     * skips the hundreds of healthy scene-graph transforms. */
    if (!out[0] && !out[1] && !out[2]) {
        show("mul44 a", a, A, 4);
        show("mul44 b", b, B, 4);
        show("mul44 =", dst, out, 4);
    }
    memcpy((void *)(uintptr_t)dst, out, 64);
    RET(dst);
}

/* _sMatrix44::setMul(const _sMatrix34 &, const _sMatrix34 &)
 *
 * A 3x4 has three rows; the fourth is the constant the original loads once
 * into xmm4 and stores straight into dst row 3. Reading it from the image
 * rather than assuming (0,0,0,1) keeps this honest if it is anything else. */
#define MAT34_ROW3 0x88401D0u

static void h_mul34(CPU *c)
{
    uint32_t dst = A32(0), a = A32(1), b = A32(2);
    float A[12], B[12], K[4], out[12];
    rd_rows(a, A, 3);
    rd_rows(b, B, 3);
    rd_rows(MAT34_ROW3, K, 1);
    mul_rows(A, B, out, 3, K);
    if (!out[0] && !out[1] && !out[2]) {
        show("mul34 a", a, A, 3);
        show("mul34 b", b, B, 3);
        show("mul34 =", dst, out, 3);
    }
    memcpy((void *)(uintptr_t)dst, out, 48);
    memcpy((void *)(uintptr_t)(dst + 48), K, 16);
    RET(dst);
}

void hle_register_matrix(void)
{
    int n = 0;
    n += guest_override("_ZN8_sSerial10_sMatrix446setMulERKS0_S2_", h_mul44);
    n += guest_override("_ZN8_sSerial10_sMatrix446setMulERKNS_10_sMatrix34ES3_", h_mul34);
    fprintf(stderr, "[matrix] %d of 2 multiplies overridden\n", n);
}
