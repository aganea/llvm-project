// Test the /compilation-database option which reads a JSON compilation database
// and creates jobs for each entry.

// REQUIRES: x86-registered-target

//--- Test basic compilation database with a single clang entry.
// RUN: echo '[{"directory": "%/T", "file": "foo.c", "output": "foo.obj", "arguments": ["clang-cl", "/c", "foo.c", "/Fofoo.obj"]}]' > %t.cdb
// RUN: %clang_cl /compilation-database:%t.cdb -### 2>&1 | FileCheck %s --check-prefix=BASIC
// BASIC: "clang-cl"
// BASIC-SAME: "/c"
// BASIC-SAME: "foo.c"

//--- Test that a missing compilation database file produces an error.
// RUN: not %clang_cl /compilation-database:%t.nonexistent -### 2>&1 | FileCheck %s --check-prefix=MISSING
// MISSING: error: cannot open file

//--- Test that a malformed JSON file produces an error.
// RUN: echo 'not valid json' > %t.bad.cdb
// RUN: not %clang_cl /compilation-database:%t.bad.cdb -### 2>&1 | FileCheck %s --check-prefix=BADJSON
// BADJSON: error: cannot open file

//--- Test that an empty array produces no jobs.
// RUN: echo '[]' > %t.empty.cdb
// RUN: %clang_cl /compilation-database:%t.empty.cdb -### 2>&1 | FileCheck %s --check-prefix=EMPTY --allow-empty
// EMPTY-NOT: "clang-cl"

//--- Test compilation database with multiple entries.
// RUN: echo '[{"directory": "%/T", "file": "a.c", "output": "a.obj", "arguments": ["clang-cl", "/c", "a.c", "/Foa.obj"]}, {"directory": "%/T", "file": "b.c", "output": "b.obj", "arguments": ["clang-cl", "/c", "b.c", "/Fob.obj"]}]' > %t.multi.cdb
// RUN: %clang_cl /compilation-database:%t.multi.cdb -### 2>&1 | FileCheck %s --check-prefix=MULTI
// MULTI: "clang-cl"
// MULTI-SAME: "a.c"
// MULTI: "clang-cl"
// MULTI-SAME: "b.c"
