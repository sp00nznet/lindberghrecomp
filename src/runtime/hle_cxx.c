/*
 * hle_cxx.c - the parts of libstdc++ that are algorithms, not wrappers.
 *
 * Most of what a C++ game imports from libstdc++ can be answered by handing
 * the work to the host's own library. These three cannot. They are the
 * out-of-line half of std::map and std::set: the tree walking and the
 * insert-time rebalance that every map in the game depends on for its
 * ordering invariant.
 *
 * A stub is not an option here. Returning zero from _Rb_tree_increment ends
 * an iteration early; skipping the rebalance leaves a tree that is still a
 * valid binary search tree and still answers lookups, so nothing fails until
 * it has degenerated into a list and the game is mysteriously slow, or until
 * an erase walks a colour invariant that was never maintained. Either way the
 * damage surfaces nowhere near here.
 *
 * So these are the real algorithms, working on the guest's nodes in place.
 * The layout is libstdc++'s _Rb_tree_node_base as an i386 build lays it out:
 *
 *      +0   colour      0 = red, 1 = black
 *      +4   parent
 *      +8   left
 *      +12  right
 *
 * The header node is the tree's sentinel: its parent is the root, its left
 * the leftmost node, its right the rightmost, and its colour is red - which
 * is what tells _Rb_tree_decrement it has been handed end().
 */

#include <stdio.h>

#include "lindbergh_rt.h"

#define RB_RED   0u
#define RB_BLACK 1u

#define COLOUR(n)  rd32((n) + 0)
#define PARENT(n)  rd32((n) + 4)
#define LEFT(n)    rd32((n) + 8)
#define RIGHT(n)   rd32((n) + 12)

#define SET_COLOUR(n, v) wr32((n) + 0, (v))
#define SET_PARENT(n, v) wr32((n) + 4, (v))
#define SET_LEFT(n, v)   wr32((n) + 8, (v))
#define SET_RIGHT(n, v)  wr32((n) + 12, (v))

static uint32_t rb_increment(uint32_t x)
{
    if (RIGHT(x)) {
        x = RIGHT(x);
        while (LEFT(x)) x = LEFT(x);
    } else {
        uint32_t y = PARENT(x);
        while (x == RIGHT(y)) { x = y; y = PARENT(y); }
        if (RIGHT(x) != y) x = y;
    }
    return x;
}

static uint32_t rb_decrement(uint32_t x)
{
    /* end() is the header, and the header is the only red node whose
     * grandparent is itself. */
    if (COLOUR(x) == RB_RED && PARENT(PARENT(x)) == x) {
        x = RIGHT(x);
    } else if (LEFT(x)) {
        uint32_t y = LEFT(x);
        while (RIGHT(y)) y = RIGHT(y);
        x = y;
    } else {
        uint32_t y = PARENT(x);
        while (x == LEFT(y)) { x = y; y = PARENT(y); }
        x = y;
    }
    return x;
}

/* Rotations take the root by pointer because a rotation at the root changes
 * which node the root is, and the caller has to be told. */
static void rb_rotate_left(uint32_t x, uint32_t *root)
{
    uint32_t y = RIGHT(x);
    SET_RIGHT(x, LEFT(y));
    if (LEFT(y)) SET_PARENT(LEFT(y), x);
    SET_PARENT(y, PARENT(x));

    if (x == *root)                     *root = y;
    else if (x == LEFT(PARENT(x)))      SET_LEFT(PARENT(x), y);
    else                                SET_RIGHT(PARENT(x), y);

    SET_LEFT(y, x);
    SET_PARENT(x, y);
}

static void rb_rotate_right(uint32_t x, uint32_t *root)
{
    uint32_t y = LEFT(x);
    SET_LEFT(x, RIGHT(y));
    if (RIGHT(y)) SET_PARENT(RIGHT(y), x);
    SET_PARENT(y, PARENT(x));

    if (x == *root)                     *root = y;
    else if (x == RIGHT(PARENT(x)))     SET_RIGHT(PARENT(x), y);
    else                                SET_LEFT(PARENT(x), y);

    SET_RIGHT(y, x);
    SET_PARENT(x, y);
}

/* Link the new node under p, then walk back up restoring the colour rules.
 * The header's left and right have to keep tracking the extremes, because
 * begin() and rbegin() read them directly rather than walking. */
static void rb_insert_and_rebalance(int insert_left, uint32_t x, uint32_t p,
                                    uint32_t header)
{
    uint32_t root = PARENT(header);

    SET_PARENT(x, p);
    SET_LEFT(x, 0);
    SET_RIGHT(x, 0);
    SET_COLOUR(x, RB_RED);

    if (insert_left) {
        SET_LEFT(p, x);
        if (p == header) {              /* first node: it is the whole tree */
            SET_PARENT(header, x);
            SET_RIGHT(header, x);
            root = x;
        } else if (p == LEFT(header)) {
            SET_LEFT(header, x);        /* a new leftmost */
        }
    } else {
        SET_RIGHT(p, x);
        if (p == RIGHT(header)) SET_RIGHT(header, x);
    }

    while (x != root && COLOUR(PARENT(x)) == RB_RED) {
        uint32_t xpp = PARENT(PARENT(x));
        if (PARENT(x) == LEFT(xpp)) {
            uint32_t y = RIGHT(xpp);
            if (y && COLOUR(y) == RB_RED) {
                SET_COLOUR(PARENT(x), RB_BLACK);
                SET_COLOUR(y, RB_BLACK);
                SET_COLOUR(xpp, RB_RED);
                x = xpp;
            } else {
                if (x == RIGHT(PARENT(x))) {
                    x = PARENT(x);
                    rb_rotate_left(x, &root);
                }
                SET_COLOUR(PARENT(x), RB_BLACK);
                SET_COLOUR(PARENT(PARENT(x)), RB_RED);
                rb_rotate_right(PARENT(PARENT(x)), &root);
            }
        } else {
            uint32_t y = LEFT(xpp);
            if (y && COLOUR(y) == RB_RED) {
                SET_COLOUR(PARENT(x), RB_BLACK);
                SET_COLOUR(y, RB_BLACK);
                SET_COLOUR(xpp, RB_RED);
                x = xpp;
            } else {
                if (x == LEFT(PARENT(x))) {
                    x = PARENT(x);
                    rb_rotate_right(x, &root);
                }
                SET_COLOUR(PARENT(x), RB_BLACK);
                SET_COLOUR(PARENT(PARENT(x)), RB_RED);
                rb_rotate_left(PARENT(PARENT(x)), &root);
            }
        }
    }
    SET_COLOUR(root, RB_BLACK);
    SET_PARENT(header, root);
}

/* ---- the entry points ----
 *
 * These are C++ symbols but plain C-linkage as far as the PLT is concerned,
 * so they bind by mangled name like anything else. Both the const and
 * non-const iterator forms exist in libstdc++ and do the same work. */

static void h_rb_increment(CPU *c) { RET(rb_increment(A32(0))); }
static void h_rb_decrement(CPU *c) { RET(rb_decrement(A32(0))); }

static void h_rb_insert(CPU *c)
{
    rb_insert_and_rebalance((int)A32(0), A32(1), A32(2), A32(3));
    RET(0);
}

/* ---- the C++ ABI, as far as reporting goes ----
 *
 * These four are what a C++ program calls when something has already gone
 * wrong, and leaving them unbound is why it goes wrong quietly. Under
 * permissive binding each returns zero: a pure virtual call carries on into
 * nothing, and an exception that starts unwinding never finishes, which ends
 * as a fail-fast with no handler able to report it.
 *
 * None of these tries to implement C++ exceptions. They say what happened,
 * which is the part that was missing. */
static void h_cxa_pure_virtual(CPU *c)
{
    fprintf(stderr, "\n[c++] pure virtual function called - a vtable is wrong\n");
    guest_backtrace(c);
    guest_report_state("pure virtual call");
    exit(46);
}

static void h_unwind_resume(CPU *c)
{
    fprintf(stderr, "\n[c++] an exception is unwinding, and this runtime has no "
                    "unwinder\n");
    guest_backtrace(c);
    guest_report_state("exception unwind");
    exit(47);
}

static void h_cxa_call_unexpected(CPU *c)
{
    fprintf(stderr, "\n[c++] unexpected exception\n");
    guest_report_state("unexpected exception");
    exit(48);
}

/* The personality routine drives unwinding. Answering 8 is
 * _URC_CONTINUE_UNWIND, which is the only honest answer when there is no
 * unwinder: it says "not my frame" rather than claiming to have handled it. */
static void h_gxx_personality(CPU *c)
{
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "[c++] personality routine reached; exceptions are not "
                        "supported here\n");
    }
    RET(8);
}

/* Registering a destructor to run at exit. Nothing here ever exits cleanly
 * enough for it to matter, and saying so once is better than each time. */
static void h_cxa_atexit(CPU *c) { RET(0); }


/* ---- std::string ----
 *
 * After Burner Climax, OutRun 2 SP SDX and Virtua Tennis 3 all build their
 * file paths and identifiers with std::string, and a game that gets nothing
 * back from a string constructor does not get as far as opening a window.
 *
 * This is libstdc++ 3.x's copy-on-write string, and its layout is fixed by
 * the ABI because the guest has the other half of it inlined. The object is
 * one pointer to the characters; immediately before them sits a _Rep:
 *
 *      -12  length
 *       -8  capacity
 *       -4  reference count
 *        0  the characters, always NUL terminated
 *
 * Every string made here is unshared - reference count zero - and a copy is a
 * real copy. Sharing is what the count exists for, and getting it wrong frees
 * a buffer somebody is still reading; copying is a little slower and cannot.
 * That matters because the guest destroys these itself: ~basic_string is
 * inlined and calls _M_dispose, which decrements and frees at zero, so the
 * arithmetic has to be exactly the library's own.
 */

#define REP_LEN(p)  rd32((p) - 12)
#define REP_CAP(p)  rd32((p) - 8)
#define REP_REF(p)  rd32((p) - 4)
#define SET_LEN(p, v) wr32((p) - 12, (v))
#define SET_CAP(p, v) wr32((p) - 8, (v))
#define SET_REF(p, v) wr32((p) - 4, (v))

/* A fresh unshared rep holding `n` bytes from `src`, NUL terminated. */
static uint32_t str_new(const char *src, uint32_t n)
{
    unsigned char *block = (unsigned char *)malloc(12 + n + 1);
    if (!block) return 0;
    uint32_t p = (uint32_t)(uintptr_t)block + 12;
    SET_LEN(p, n);
    SET_CAP(p, n);
    SET_REF(p, 0);
    if (src && n) memcpy((void *)(uintptr_t)p, src, n);
    ((char *)(uintptr_t)p)[n] = 0;
    return p;
}

static uint32_t str_from(uint32_t data, uint32_t n) { return str_new((const char *)(uintptr_t)data, n); }

/* Replace what `self` points at, freeing the old rep if this held the last
 * reference - the same test _M_dispose makes. */
static void str_set(uint32_t self, uint32_t newdata)
{
    uint32_t old = rd32(self);
    wr32(self, newdata);
    if (old && (int32_t)REP_REF(old) <= 0) free((void *)(uintptr_t)(old - 12));
}

/* basic_string(const char *s, const allocator &) */
static void h_str_ctor_cstr(CPU *c)
{
    const char *s = A32(1) ? (const char *)(uintptr_t)A32(1) : "";
    wr32(A32(0), str_new(s, (uint32_t)strlen(s)));
    RET(A32(0));
}

/* basic_string(const char *s, size_type n, const allocator &) */
static void h_str_ctor_cstr_n(CPU *c)
{
    wr32(A32(0), str_from(A32(1), A32(2)));
    RET(A32(0));
}

/* basic_string(const basic_string &) - a real copy, not a shared one */
static void h_str_ctor_copy(CPU *c)
{
    uint32_t src = rd32(A32(1));
    wr32(A32(0), src ? str_from(src, REP_LEN(src)) : str_new("", 0));
    RET(A32(0));
}

/* basic_string(const basic_string &, size_type pos, size_type n) */
static void h_str_ctor_substr(CPU *c)
{
    uint32_t src = rd32(A32(1)), pos = A32(2), n = A32(3);
    uint32_t len = src ? REP_LEN(src) : 0;
    if (pos > len) pos = len;
    if (n > len - pos) n = len - pos;
    wr32(A32(0), str_from(src + pos, n));
    RET(A32(0));
}

static void h_str_append_cstr(CPU *c)
{
    uint32_t self = A32(0), add = A32(1), n = A32(2);
    uint32_t cur = rd32(self), len = cur ? REP_LEN(cur) : 0;
    uint32_t out = str_new(NULL, len + n);
    if (out) {
        if (len) memcpy((void *)(uintptr_t)out, (void *)(uintptr_t)cur, len);
        if (n)   memcpy((void *)(uintptr_t)(out + len), (void *)(uintptr_t)add, n);
        ((char *)(uintptr_t)out)[len + n] = 0;
    }
    str_set(self, out);
    RET(self);
}

static void h_str_append_str(CPU *c)
{
    uint32_t other = rd32(A32(1));
    uint32_t self = A32(0), add = other, n = other ? REP_LEN(other) : 0;
    uint32_t cur = rd32(self), len = cur ? REP_LEN(cur) : 0;
    uint32_t out = str_new(NULL, len + n);
    if (out) {
        if (len) memcpy((void *)(uintptr_t)out, (void *)(uintptr_t)cur, len);
        if (n)   memcpy((void *)(uintptr_t)(out + len), (void *)(uintptr_t)add, n);
        ((char *)(uintptr_t)out)[len + n] = 0;
    }
    str_set(self, out);
    RET(self);
}

static void h_str_assign_cstr(CPU *c)
{
    str_set(A32(0), str_from(A32(1), A32(2)));
    RET(A32(0));
}

static void h_str_assign_str(CPU *c)
{
    uint32_t src = rd32(A32(1));
    str_set(A32(0), src ? str_from(src, REP_LEN(src)) : str_new("", 0));
    RET(A32(0));
}

/* replace(pos, n1, const char *s, n2) */
static void h_str_replace(CPU *c)
{
    uint32_t self = A32(0), pos = A32(1), n1 = A32(2), s = A32(3), n2 = A32(4);
    uint32_t cur = rd32(self), len = cur ? REP_LEN(cur) : 0;
    if (pos > len) pos = len;
    if (n1 > len - pos) n1 = len - pos;

    uint32_t out = str_new(NULL, len - n1 + n2);
    if (out) {
        char *o = (char *)(uintptr_t)out;
        if (pos) memcpy(o, (void *)(uintptr_t)cur, pos);
        if (n2)  memcpy(o + pos, (void *)(uintptr_t)s, n2);
        if (len - pos - n1)
            memcpy(o + pos + n2, (void *)(uintptr_t)(cur + pos + n1), len - pos - n1);
        o[len - n1 + n2] = 0;
    }
    str_set(self, out);
    RET(self);
}

/* _Rep::_M_destroy(const allocator &) - `this` is the rep, twelve bytes
 * before the characters, and that is what was allocated. */
static void h_rep_destroy(CPU *c) { if (A32(0)) free((void *)(uintptr_t)A32(0)); RET(0); }

/* _Rep::_M_dispose(const allocator &) - the library decrements first and
 * destroys when the old value was already at or below zero. */
static void h_rep_dispose(CPU *c)
{
    uint32_t rep = A32(0);
    if (!rep) { RET(0); return; }
    int32_t before = (int32_t)rd32(rep + 8);      /* refcount is the third word */
    wr32(rep + 8, (uint32_t)(before - 1));
    if (before <= 0) free((void *)(uintptr_t)rep);
    RET(0);
}

/* Already unshared, so there is nothing to leak apart. */
static void h_str_leak(CPU *c) { RET(0); }

/* The SGI pool allocator these builds still use. A pool is an optimisation;
 * malloc is the thing it was optimising. */
static void h_sgi_allocate(CPU *c)   { RET(malloc(A32(0) ? A32(0) : 1)); }
static void h_sgi_deallocate(CPU *c) { if (A32(0)) free((void *)(uintptr_t)A32(0)); RET(0); }

/* iostreams. Setting up cin, cout and cerr for a game that writes to a
 * console the cabinet does not have. */
static void h_ios_init(CPU *c) { RET(0); }

void hle_register_cxx(void)
{
    int n = 0;
    n += hle_bind("_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", h_rb_increment);
    n += hle_bind("_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base", h_rb_increment);
    n += hle_bind("_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", h_rb_decrement);
    n += hle_bind("_ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base", h_rb_decrement);
    n += hle_bind("_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_",
                  h_rb_insert);

    n += hle_bind("__cxa_pure_virtual", h_cxa_pure_virtual);
    n += hle_bind("_Unwind_Resume", h_unwind_resume);
    n += hle_bind("__cxa_call_unexpected", h_cxa_call_unexpected);
    n += hle_bind("__gxx_personality_v0", h_gxx_personality);
    n += hle_bind("__cxa_atexit", h_cxa_atexit);

    /* std::string, the SGI allocator under it, and the iostream set up these
     * games do at start up. Mangled, but plain C-linkage to the PLT. */
    n += hle_bind("_ZNSsC1EPKcRKSaIcE", h_str_ctor_cstr);
    n += hle_bind("_ZNSsC1EPKcjRKSaIcE", h_str_ctor_cstr_n);
    n += hle_bind("_ZNSsC1ERKSs", h_str_ctor_copy);
    n += hle_bind("_ZNSsC1ERKSsjj", h_str_ctor_substr);
    n += hle_bind("_ZNSs6appendEPKcj", h_str_append_cstr);
    n += hle_bind("_ZNSs6appendERKSs", h_str_append_str);
    n += hle_bind("_ZNSs6assignEPKcj", h_str_assign_cstr);
    n += hle_bind("_ZNSs6assignERKSs", h_str_assign_str);
    n += hle_bind("_ZNSs7replaceEjjPKcj", h_str_replace);
    n += hle_bind("_ZNSs4_Rep10_M_destroyERKSaIcE", h_rep_destroy);
    n += hle_bind("_ZNSs4_Rep10_M_disposeERKSaIcE", h_rep_dispose);
    n += hle_bind("_ZNSs12_M_leak_hardEv", h_str_leak);
    n += hle_bind("_ZNSt24__default_alloc_templateILb1ELi0EE8allocateEj", h_sgi_allocate);
    n += hle_bind("_ZNSt24__default_alloc_templateILb1ELi0EE10deallocateEPvj", h_sgi_deallocate);
    n += hle_bind("_ZNSt8ios_base4InitC1Ev", h_ios_init);
    n += hle_bind("_ZNSt8ios_base4InitD1Ev", h_ios_init);
    fprintf(stderr, "[c++] %d tree entry points bound\n", n);
}
