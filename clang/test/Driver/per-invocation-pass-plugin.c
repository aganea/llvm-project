// REQUIRES: clang-driver-per-invocation-plugin
// RUN: split-file %s %t
// RUN: rm -f %t/trace
// RUN: cd %t && %clang_driver_per_invocation_host -fintegrated-cc1 \
// RUN:   -target %clang_driver_per_invocation_triple -c -O1 \
// RUN:   -fpass-plugin="%clang_driver_per_invocation_plugin" \
// RUN:   -mllvm -per-invocation-plugin-value=driver \
// RUN:   -mllvm -per-invocation-plugin-trace="%t/trace" first.c second.c
// RUN: FileCheck %s --input-file=%t/trace --check-prefix=TRACE

// Both cc1 jobs run inside one folded Clang invocation. They therefore reuse
// one invocation-owned plugin option object, while ResetAllOptionOccurrences
// permits each cc1 job to parse the dynamically registered options again. The
// single destroy line is emitted when the enclosing tool context is released.
// TRACE:      run=driver
// TRACE-NEXT: run=driver
// TRACE-NEXT: destroy=driver
// TRACE-NOT:  {{.}}

//--- first.c
int first(void) { return 1; }

//--- second.c
int second(void) { return 2; }
