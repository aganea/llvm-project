// RUN: %clang_cc1 -fsanitize=address -triple=x86_64-pc-windows-msvc -emit-llvm -o - %s | FileCheck %s --check-prefix=ASAN
// RUN: %clang_cc1 -triple=x86_64-pc-windows-msvc -emit-llvm -o - %s | FileCheck %s --check-prefix=NOASAN

// Windows atexit() is not reliably interceptable: for /MT builds, its
// implementation (and everything it calls) is compiled directly into the
// executable, with no import for ASan to hook. So instead of trying to
// intercept atexit(), ASan has the atexit stub generated for each global
// with a non-trivial destructor call into the ASan runtime DLL -- always a
// genuine cross-module call regardless of the caller's CRT linkage -- right
// before it invokes the real destructor. Registration itself must stay a
// plain, unmodified call to atexit(): /MT and /MD each walk their own
// separate onexit table at real program exit, and the destructor has to
// land in whichever one the caller's own atexit() resolves to.

struct Foo {
  ~Foo() {}
};
Foo f;

// The dynamic initializer registers the destructor stub with a plain,
// unmodified call to atexit() -- unchanged from the non-ASan case below.
// ASAN-LABEL: define internal void @"??__Ef@@YAXXZ"()
// ASAN: call i32 @atexit(ptr @"??__Ff@@YAXXZ")

// The destructor stub itself calls into the ASan runtime DLL immediately
// before running the real destructor.
// ASAN-LABEL: define internal void @"??__Ff@@YAXXZ"()
// ASAN: call void @__asan_before_global_dtor()
// ASAN-NEXT: call void @"??1Foo@@QEAA@XZ"

// NOASAN-NOT: __asan_before_global_dtor

// NOASAN-LABEL: define internal void @"??__Ef@@YAXXZ"()
// NOASAN: call i32 @atexit(ptr @"??__Ff@@YAXXZ")

// NOASAN-LABEL: define internal void @"??__Ff@@YAXXZ"()
// NOASAN-NEXT: entry:
// NOASAN-NEXT: call void @"??1Foo@@QEAA@XZ"
