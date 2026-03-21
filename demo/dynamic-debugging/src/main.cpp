#include "math_utils.h"
#include <cstdio>

int main() {
    int result = 0;
    for (int i = 1; i <= 10; i++) {
        result += compute(i, i + 1);
        printf("Step %d: result = %d\n", i, result);
    }
    printf("Final: %d\n", result);
    return 0;
}
