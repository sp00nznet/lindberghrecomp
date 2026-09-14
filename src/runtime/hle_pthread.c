/*
 * hle_pthread.c - the threads the board provided.
 *
 * Let's Go Jungle sets up its mutexes and condition variables, spawns a worker
 * thread and waits on it, all before it opens the display - so none of this is
 * optional to get a picture.
 *
 * A guest thread gets its own CPU struct and its own guest stack. That is the
 * whole reason this project uses pcrecomp's CPU-struct lifter rather than its
 * global-register one: two machine states have to be live at once, and with
 * registers in globals they would be the same registers.
 *
 * Guest objects (pthread_mutex_t and friends) are opaque blobs of a fixed
 * size that the game allocates itself - on its stack, in its globals, inside
 * its own structs. Rather than model glibc's layout, this writes its own small
 * header into that space. The sizes on i386 glibc 2.3 are:
 *
 *     pthread_mutex_t     24 bytes
 *     pthread_cond_t      48
 *     pthread_attr_t      36
 *     pthread_mutexattr_t  4
 *     sem_t               16
 *
 * Everything here fits well inside the smallest of those, which is what makes
 * the approach safe. The magic word matters for a second reason: glibc lets a
 * mutex be initialised statically with PTHREAD_MUTEX_INITIALIZER and never
 * passed to pthread_mutex_init at all, so a lock on a zeroed blob has to work.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "lindbergh_rt.h"

#define GUEST_MAGIC 0x4C474A31u      /* "LGJ1" */

typedef struct {
    uint32_t magic;
    void    *host;
} GuestObj;

#ifdef _WIN32

/* One lock guards lazy creation of guest objects. Contended only on the first
 * touch of each object, so a single global is the right amount of machinery.
 * ponytail: per-object double-checked init if that ever shows up in a profile. */
static CRITICAL_SECTION g_init_lock;
static int g_init_ready;

static void init_once(void)
{
    if (!g_init_ready) { InitializeCriticalSection(&g_init_lock); g_init_ready = 1; }
}

/* Fetch the host object behind a guest blob, creating it if the guest never
 * called the _init function - which is legal for statically initialised
 * mutexes and condition variables. */
static void *obj_get(uint32_t va, size_t host_size, void (*ctor)(void *))
{
    GuestObj *g = (GuestObj *)(uintptr_t)va;
    if (g->magic == GUEST_MAGIC && g->host)
        return g->host;

    init_once();
    EnterCriticalSection(&g_init_lock);
    if (g->magic != GUEST_MAGIC || !g->host) {
        void *h = calloc(1, host_size);
        if (h) ctor(h);
        g->host  = h;
        g->magic = GUEST_MAGIC;
    }
    LeaveCriticalSection(&g_init_lock);
    return g->host;
}

static void ctor_cs(void *p)   { InitializeCriticalSection((CRITICAL_SECTION *)p); }
static void ctor_cond(void *p) { InitializeConditionVariable((CONDITION_VARIABLE *)p); }

#define MUTEX(n)  ((CRITICAL_SECTION *)obj_get(A32(n), sizeof(CRITICAL_SECTION), ctor_cs))
#define COND(n)   ((CONDITION_VARIABLE *)obj_get(A32(n), sizeof(CONDITION_VARIABLE), ctor_cond))

/* ---- mutexes ----
 *
 * A CRITICAL_SECTION is always recursive, where a default pthread mutex is
 * not. The game calls pthread_mutexattr_settype, so it wants recursive here;
 * and for code that does not relock, the difference is invisible. Where it
 * would show is a program that relies on deadlocking itself, which is not a
 * thing anyone relies on. */
static void h_mutex_init(CPU *c)    { (void)MUTEX(0); RET(0); }
static void h_mutex_lock(CPU *c)    { EnterCriticalSection(MUTEX(0)); RET(0); }
static void h_mutex_unlock(CPU *c)  { LeaveCriticalSection(MUTEX(0)); RET(0); }
static void h_mutex_trylock(CPU *c) { RET(TryEnterCriticalSection(MUTEX(0)) ? 0 : 16); }

static void h_mutex_destroy(CPU *c)
{
    GuestObj *g = (GuestObj *)(uintptr_t)A32(0);
    if (g->magic == GUEST_MAGIC && g->host) {
        DeleteCriticalSection((CRITICAL_SECTION *)g->host);
        free(g->host);
        g->host = NULL;
        g->magic = 0;
    }
    RET(0);
}

/* ---- condition variables ---- */
static void h_cond_init(CPU *c)      { (void)COND(0); RET(0); }
static void h_cond_signal(CPU *c)    { WakeConditionVariable(COND(0)); RET(0); }
static void h_cond_broadcast(CPU *c) { WakeAllConditionVariable(COND(0)); RET(0); }

static void h_cond_wait(CPU *c)
{
    /* The mutex must be reacquired before returning, which
     * SleepConditionVariableCS already guarantees. */
    SleepConditionVariableCS(COND(0), MUTEX(1), INFINITE);
    RET(0);
}

static void h_cond_timedwait(CPU *c)
{
    /* The guest passes an ABSOLUTE deadline as struct timespec { long sec;
     * long nsec; }, and Windows wants a relative millisecond count. A deadline
     * already past must not block. */
    uint32_t ts = A32(2);
    DWORD ms = INFINITE;
    if (ts) {
        int64_t deadline = (int64_t)(int32_t)rd32(ts) * 1000
                         + (int64_t)(int32_t)rd32(ts + 4) / 1000000;
        int64_t now = (int64_t)GetTickCount64();
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        int64_t wall = ((((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime)
                        / 10000) - 11644473600000LL;     /* 1601 epoch -> 1970 */
        (void)now;
        int64_t delta = deadline - wall;
        ms = delta <= 0 ? 0 : (DWORD)delta;
    }
    BOOL ok = SleepConditionVariableCS(COND(0), MUTEX(1), ms);
    RET(ok ? 0 : 110);                                   /* ETIMEDOUT */
}

static void h_cond_destroy(CPU *c)
{
    GuestObj *g = (GuestObj *)(uintptr_t)A32(0);
    if (g->magic == GUEST_MAGIC && g->host) {            /* no destroy call exists */
        free(g->host);
        g->host = NULL;
        g->magic = 0;
    }
    RET(0);
}

/* ---- thread attributes ----
 *
 * Written into the guest's own pthread_attr_t storage rather than a side
 * table, because the guest allocates it (usually on its stack) and hands us
 * only the pointer. 36 bytes there, 20 used. */
typedef struct {
    uint32_t magic;
    uint32_t stacksize;
    int32_t  policy;
    int32_t  priority;
    int32_t  detached;
} GuestAttr;

#define GUEST_STACK_DEFAULT (2u << 20)

static GuestAttr *attr_of(uint32_t va)
{
    GuestAttr *a = (GuestAttr *)(uintptr_t)va;
    if (a->magic != GUEST_MAGIC) {
        memset(a, 0, sizeof *a);
        a->magic = GUEST_MAGIC;
        a->stacksize = GUEST_STACK_DEFAULT;
    }
    return a;
}

static void h_attr_init(CPU *c)         { (void)attr_of(A32(0)); RET(0); }
static void h_attr_setstacksize(CPU *c) { attr_of(A32(0))->stacksize = A32(1); RET(0); }
static void h_attr_setschedpolicy(CPU *c) { attr_of(A32(0))->policy = AI32(1); RET(0); }
static void h_attr_setschedparam(CPU *c)
{
    /* struct sched_param is a single int. */
    if (A32(1)) attr_of(A32(0))->priority = (int32_t)rd32(A32(1));
    RET(0);
}

/* Nothing here honours a scheduling policy: the arcade board ran SCHED_RR at a
 * fixed priority and a desktop will not, so promising a range we cannot keep
 * would be worse than reporting the one we have. */
static void h_sched_get_priority_max(CPU *c) { RET(0); }
static void h_sched_get_priority_min(CPU *c) { RET(0); }

static void h_mutexattr_init(CPU *c)    { if (A32(0)) wr32(A32(0), 0); RET(0); }
static void h_mutexattr_settype(CPU *c) { RET(0); }   /* always recursive here */

/* ---- threads ----
 *
 * The start routine is a GUEST function pointer, so the host thread that runs
 * it needs a whole guest machine: its own CPU struct and its own guest stack.
 * The stack comes from the host allocator, which is fine and not a shortcut -
 * this is a 32-bit process, so every allocation is already inside the address
 * space the guest can name, and a guest pointer is a host pointer.
 */
typedef struct {
    uint32_t start;          /* guest void *(*)(void *) */
    uint32_t arg;
    void    *stack;
    uint32_t stacksize;
    HANDLE   handle;
    uint32_t retval;
} GuestThread;

static DWORD WINAPI thread_trampoline(LPVOID p)
{
    GuestThread *t = (GuestThread *)p;
    CPU cpu;

    memset(&cpu, 0, sizeof cpu);
    guest_set_current_cpu(&cpu);

    /* Stack grows down, so start at the top. Leave a little headroom and keep
     * esp 16-byte aligned, which the SSE the game is full of requires. */
    uint32_t top = (uint32_t)(uintptr_t)t->stack + t->stacksize - 64;
    cpu.esp = top & ~15u;

    uint32_t args[1] = { t->arg };
    t->retval = guest_call(&cpu, t->start, args, 1);
    return 0;
}

static void h_pthread_create(CPU *c)
{
    uint32_t out = A32(0), attr = A32(1);
    GuestThread *t = (GuestThread *)calloc(1, sizeof *t);
    if (!t) { RET(11); return; }                          /* EAGAIN */

    t->start     = A32(2);
    t->arg       = A32(3);
    t->stacksize = attr ? attr_of(attr)->stacksize : GUEST_STACK_DEFAULT;
    if (t->stacksize < (256u << 10)) t->stacksize = 256u << 10;
    t->stack = malloc(t->stacksize);
    if (!t->stack) { free(t); RET(11); return; }

    t->handle = CreateThread(NULL, 0, thread_trampoline, t, 0, NULL);
    if (!t->handle) { free(t->stack); free(t); RET(11); return; }

    /* pthread_t is an opaque word; hand back the record itself. */
    if (out) wr32(out, (uint32_t)(uintptr_t)t);
    fprintf(stderr, "[pthread] created thread at %#010x, %u KB stack\n",
            t->start, t->stacksize >> 10);
    RET(0);
}

static void h_pthread_join(CPU *c)
{
    GuestThread *t = (GuestThread *)(uintptr_t)A32(0);
    if (t && t->handle) {
        WaitForSingleObject(t->handle, INFINITE);
        CloseHandle(t->handle);
        if (A32(1)) wr32(A32(1), t->retval);
        free(t->stack);
        free(t);
    }
    RET(0);
}

static void h_pthread_detach(CPU *c) { RET(0); }
static void h_pthread_self(CPU *c)   { RET(GetCurrentThreadId()); }
static void h_pthread_equal(CPU *c)  { RET(A32(0) == A32(1)); }
static void h_pthread_getschedparam(CPU *c) { RET(0); }
static void h_pthread_setschedparam(CPU *c) { RET(0); }

static void h_pthread_once(CPU *c)
{
    /* pthread_once_t is a guest int, zero until done. The routine takes no
     * arguments and the guest is not racing on it at this point. */
    uint32_t flag = A32(0), fn = A32(1);
    if (flag && !rd32(flag)) {
        wr32(flag, 1);
        guest_call(c, fn, NULL, 0);
    }
    RET(0);
}

/* ---- thread-local storage ---- */
static void h_key_create(CPU *c)
{
    DWORD k = TlsAlloc();
    if (k == TLS_OUT_OF_INDEXES) { RET(12); return; }     /* ENOMEM */
    if (A32(0)) wr32(A32(0), (uint32_t)k);
    RET(0);
}
static void h_getspecific(CPU *c) { RET(TlsGetValue(A32(0))); }
static void h_setspecific(CPU *c) { TlsSetValue(A32(0), (LPVOID)(uintptr_t)A32(1)); RET(0); }

/* ---- semaphores ---- */
static void ctor_sem(void *p) { *(HANDLE *)p = NULL; }

static HANDLE *sem_of(uint32_t va)
{
    return (HANDLE *)obj_get(va, sizeof(HANDLE), ctor_sem);
}

static void h_sem_init(CPU *c)
{
    HANDLE *h = sem_of(A32(0));
    if (*h) CloseHandle(*h);
    *h = CreateSemaphore(NULL, (LONG)A32(2), 0x7FFFFFFF, NULL);
    RET(*h ? 0 : -1);
}
static void h_sem_post(CPU *c) { ReleaseSemaphore(*sem_of(A32(0)), 1, NULL); RET(0); }
static void h_sem_wait(CPU *c) { WaitForSingleObject(*sem_of(A32(0)), INFINITE); RET(0); }
static void h_sem_destroy(CPU *c)
{
    GuestObj *g = (GuestObj *)(uintptr_t)A32(0);
    if (g->magic == GUEST_MAGIC && g->host) {
        HANDLE *h = (HANDLE *)g->host;
        if (*h) CloseHandle(*h);
        free(g->host);
        g->host = NULL;
        g->magic = 0;
    }
    RET(0);
}

void hle_register_pthread(void)
{
    init_once();

    hle_bind("pthread_mutex_init", h_mutex_init);
    hle_bind("pthread_mutex_lock", h_mutex_lock);
    hle_bind("pthread_mutex_unlock", h_mutex_unlock);
    hle_bind("pthread_mutex_trylock", h_mutex_trylock);
    hle_bind("pthread_mutex_destroy", h_mutex_destroy);
    hle_bind("pthread_mutexattr_init", h_mutexattr_init);
    hle_bind("pthread_mutexattr_settype", h_mutexattr_settype);

    hle_bind("pthread_cond_init", h_cond_init);
    hle_bind("pthread_cond_signal", h_cond_signal);
    hle_bind("pthread_cond_broadcast", h_cond_broadcast);
    hle_bind("pthread_cond_wait", h_cond_wait);
    hle_bind("pthread_cond_timedwait", h_cond_timedwait);
    hle_bind("pthread_cond_destroy", h_cond_destroy);

    hle_bind("pthread_attr_init", h_attr_init);
    hle_bind("pthread_attr_setstacksize", h_attr_setstacksize);
    hle_bind("pthread_attr_setschedpolicy", h_attr_setschedpolicy);
    hle_bind("pthread_attr_setschedparam", h_attr_setschedparam);
    hle_bind("sched_get_priority_max", h_sched_get_priority_max);
    hle_bind("sched_get_priority_min", h_sched_get_priority_min);

    hle_bind("pthread_create", h_pthread_create);
    hle_bind("pthread_join", h_pthread_join);
    hle_bind("pthread_detach", h_pthread_detach);
    hle_bind("pthread_self", h_pthread_self);
    hle_bind("pthread_equal", h_pthread_equal);
    hle_bind("pthread_once", h_pthread_once);
    hle_bind("pthread_getschedparam", h_pthread_getschedparam);
    hle_bind("pthread_setschedparam", h_pthread_setschedparam);

    hle_bind("pthread_key_create", h_key_create);
    hle_bind("pthread_getspecific", h_getspecific);
    hle_bind("pthread_setspecific", h_setspecific);

    hle_bind("sem_init", h_sem_init);
    hle_bind("sem_post", h_sem_post);
    hle_bind("sem_wait", h_sem_wait);
    hle_bind("sem_destroy", h_sem_destroy);
}

#else   /* !_WIN32 */
void hle_register_pthread(void) { }
#endif
