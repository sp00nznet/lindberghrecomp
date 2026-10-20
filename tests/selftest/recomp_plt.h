/* Stand-in for the per-game generated file. Ascending, as the driver emits. */
#define HLE_PLT(X) \
    X(0x08071410u, HLE_write) \
    X(0x08071420u, HLE_glClear)
