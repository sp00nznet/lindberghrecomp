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
    fprintf(stderr, "[c++] %d tree entry points bound\n", n);
}
