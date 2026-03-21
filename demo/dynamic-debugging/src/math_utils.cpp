#include "math_utils.h"

static int g_call_count = 0;

static int multiply(int a, int b) {
    g_call_count++;
    return a * b;
}

int compute(int x, int y) {
    int product = multiply(x, y);
    int clamped = clamp(product, -1000, 1000);
    return clamped + g_call_count;
}
