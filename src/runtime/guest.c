/*
 * guest.c - put a Linux/i386 process where the recompiled code expects one.
 *
 * Lifted code holds real addresses in its registers, so the game's segments
 * have to live at the virtual addresses it was linked for. A Lindbergh ELF is
 * ET_EXEC linked at 0x08048000, which is free in a 32-bit Windows process, so
 * this reserves it outright rather than relocating anything.
 *
 * Only PT_LOAD is honoured. The real kernel also honours PT_INTERP and hands
 * off to ld.so; here there is no ld.so, because every library the game would
 * have loaded is answered by hle_call() instead. Nothing needs relocating.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "lindbergh_rt.h"

#define PT_LOAD   1
#define PAGE      0x1000u
#define STACK_TOP 0xBFFFF000u     /* where Linux/i386 puts it */
#define STACK_SZ  (8u << 20)

static uint32_t g_entry;
static uint32_t g_brk;
static uint32_t g_stack_pointer;

uint32_t g_image_delta = 0;       /* cpu.h's GVA(): we map where it asked */

static void *reserve(uint32_t addr, uint32_t size)
{
#ifdef _WIN32
    return VirtualAlloc((LPVOID)(uintptr_t)addr, size,
                        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static uint32_t rd32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int guest_load(const char *elf_path, int argc, char **argv)
{
    FILE *f = fopen(elf_path, "rb");
    if (!f) { perror(elf_path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *img = (unsigned char *)malloc((size_t)len);
    if (!img || fread(img, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "%s: short read\n", elf_path); fclose(f); return -1;
    }
    fclose(f);

    if (memcmp(img, "\x7f" "ELF\x01\x01", 6) != 0) {
        fprintf(stderr, "%s: not a 32-bit little-endian ELF\n", elf_path);
        free(img); return -1;
    }
    g_entry = rd32le(img + 24);
    uint32_t phoff = rd32le(img + 28);
    unsigned phentsize = img[42] | (img[43] << 8);
    unsigned phnum     = img[44] | (img[45] << 8);

    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *ph = img + phoff + (size_t)i * phentsize;
        if (rd32le(ph) != PT_LOAD) continue;
        uint32_t off = rd32le(ph + 4), vaddr = rd32le(ph + 8);
        uint32_t filesz = rd32le(ph + 16), memsz = rd32le(ph + 20);

        uint32_t lo = vaddr & ~(PAGE - 1);
        uint32_t hi = (vaddr + memsz + PAGE - 1) & ~(PAGE - 1);
        if (!reserve(lo, hi - lo)) {
            fprintf(stderr, "cannot map %#x..%#x - is this a 32-bit build?\n", lo, hi);
            free(img); return -1;
        }
        memset((void *)(uintptr_t)lo, 0, hi - lo);          /* .bss */
        memcpy((void *)(uintptr_t)vaddr, img + off, filesz);
        if (hi > g_brk) g_brk = hi;                          /* heap starts past the image */
    }
    free(img);

    if (!reserve(STACK_TOP - STACK_SZ, STACK_SZ)) {
        fprintf(stderr, "cannot map the guest stack\n"); return -1;
    }
    memset((void *)(uintptr_t)(STACK_TOP - STACK_SZ), 0, STACK_SZ);

    /* The initial stack, exactly as the kernel lays it out for _start:
     *   esp -> argc, argv[0..argc-1], NULL, envp[0..], NULL, auxv, AT_NULL
     * The strings go above it. The game's CRT walks this to find its own
     * argv and to read AT_PAGESZ etc, so a wrong shape crashes before main. */
    char *strp = (char *)(uintptr_t)(STACK_TOP - 4096);
    uint32_t *sp = (uint32_t *)(uintptr_t)(STACK_TOP - 8192);
    uint32_t *base = sp;
    *sp++ = (uint32_t)argc;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(strp, argv[i], n);
        *sp++ = (uint32_t)(uintptr_t)strp;
        strp += n;
    }
    *sp++ = 0;            /* argv terminator */
    *sp++ = 0;            /* empty envp */
    *sp++ = 6; *sp++ = PAGE;   /* AT_PAGESZ */
    *sp++ = 0; *sp++ = 0;      /* AT_NULL */

    g_stack_pointer = (uint32_t)(uintptr_t)base;
    return 0;
}

uint32_t guest_entry(void) { return g_entry; }

void guest_init_cpu(CPU *c)
{
    memset(c, 0, sizeof *c);
    c->esp = g_stack_pointer;
    c->eip = g_entry;
}

uint32_t guest_brk(uint32_t newbrk)
{
    /* brk(0) queries; a grow commits more pages. The recompiled CRT uses this
     * before it ever reaches mmap, so it has to work from the first call. */
    if (newbrk > g_brk) {
        uint32_t lo = (g_brk + PAGE - 1) & ~(PAGE - 1);
        uint32_t hi = (newbrk + PAGE - 1) & ~(PAGE - 1);
        if (hi > lo && !reserve(lo, hi - lo)) return g_brk;
        g_brk = newbrk;
    }
    return g_brk;
}
