/* Stand-in for the per-game generated file, so the runtime builds standalone. */
#define HLE_IMPORTS(X) \
    X(HLE_write, "write") \
    X(HLE_glClear, "glClear")
