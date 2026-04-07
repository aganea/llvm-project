#include "math_utils.h"
#include "ipo_stress.h"
#include <cstdio>

int main() {
    int result = 0;
    for (int i = 1; i <= 10; i++) {
        result += compute(i, i + 1);
        printf("Step %d: result = %d\n", i, result);
    }
    printf("Final: %d\n", result);

    printf("IPO stress tests:\n");
    printf("  dead_arg_elim:      %d\n", test_dead_arg_elim());
    printf("  arg_promotion:      %d\n", test_arg_promotion());
    printf("  func_specialization:%d\n", test_func_specialization());
    printf("  globalopt_cc:       %d\n", test_globalopt_cc());

    return 0;
}
