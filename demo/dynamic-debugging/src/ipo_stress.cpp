#include "ipo_stress.h"
#include <cstdio>

// ============================================================================
// These functions exercise the four IPO passes guarded by "preserve-abi".
//
// Without -fdynamic-debug-prep (which adds preserve-abi), each of these
// internal-linkage functions would have its calling convention or signature
// changed by the optimizer at -O2.  With preserve-abi the original ABI is
// kept, so unoptimized code compiled later can call them via the PDB.
//
// Build with and without /dyndbg:hybrid and compare the IR / assembly to
// verify:
//   clang-cl /O2 /Z7              /FA -c ipo_stress.cpp -o baseline.obj
//   clang-cl /O2 /Z7 /dyndbg:hybrid /FA -c ipo_stress.cpp -o dyndbg.obj
//
// In the baseline, the optimizer will:
//   1. Remove the unused 'tag' parameter from dead_arg_helper
//   2. Promote the pointer arg of promote_helper to pass-by-value
//   3. Create specialized clones of dispatch_helper for mode==1 / mode==2
//   4. Change internal_callee to fastcc calling convention
//
// With /dyndbg:hybrid none of those ABI changes should occur.
// ============================================================================

// ---------------------------------------------------------------------------
// 1. DeadArgumentElimination: 'tag' is never read inside the function.
//    The pass would normally remove it from the signature.
// ---------------------------------------------------------------------------
static int dead_arg_helper(int value, int tag) {
    return value * 3 + 1;
}

int test_dead_arg_elim() {
    return dead_arg_helper(10, 0xDEAD) + dead_arg_helper(20, 0xBEEF);
}

// ---------------------------------------------------------------------------
// 2. ArgumentPromotion: 'ptr' is a pointer to int that is only loaded once.
//    The pass would normally promote it to pass-by-value (int instead of int*).
// ---------------------------------------------------------------------------
struct Pair { int x; int y; };

static int promote_helper(const Pair *p) {
    return p->x + p->y;
}

int test_arg_promotion() {
    Pair a = {3, 4};
    Pair b = {5, 6};
    return promote_helper(&a) + promote_helper(&b);
}

// ---------------------------------------------------------------------------
// 3. FunctionSpecialization: called with constant 'mode' values.
//    The pass would normally clone it into dispatch_helper.specialized.1 etc.
// ---------------------------------------------------------------------------
static int dispatch_helper(int mode, int value) {
    switch (mode) {
    case 1:  return value * 2;
    case 2:  return value + 100;
    default: return value;
    }
}

int test_func_specialization() {
    int a = dispatch_helper(1, 42);
    int b = dispatch_helper(2, 42);
    int c = dispatch_helper(1, 99);
    return a + b + c;
}

// ---------------------------------------------------------------------------
// 4. GlobalOpt: internal_callee is only called within this TU.
//    The pass would normally change it from the default CC to fastcc
//    (or coldcc if all callers are cold).
// ---------------------------------------------------------------------------
static int internal_callee(int a, int b, int c, int d) {
    return ((a + b) * (c - d)) ^ (a - c);
}

int test_globalopt_cc() {
    int r = 0;
    for (int i = 0; i < 10; i++)
        r += internal_callee(i, i + 1, i + 2, i + 3);
    return r;
}
