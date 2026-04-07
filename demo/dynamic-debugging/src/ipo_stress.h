#pragma once

// Exercise preserve-abi guards across IPO passes.
// Each function below would have its ABI changed by a specific
// optimization pass when compiled at -O2 without preserve-abi.

int test_dead_arg_elim();
int test_arg_promotion();
int test_func_specialization();
int test_globalopt_cc();
