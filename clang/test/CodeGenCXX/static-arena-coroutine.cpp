// Verify that static-arena lowering composes with coroutine splitting. The
// scheduler, rather than generated code, is responsible for reinstalling the
// same ToolExecutionContext before each resume.
//
// RUN: split-file %s %t
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -std=c++20 -O1 \
// RUN:   -emit-llvm -fstatic-arena=coroutine_test \
// RUN:   -fstatic-arena-list=%t/arena.list -o - %t/test.cpp \
// RUN:   | FileCheck %s
//
// CHECK: @__llvm_arena_var_v1.{{.*}}coro_state = externally_initialized global
// CHECK: @__llvm_arena_ptr_v1.{{.*}}coro_state = private constant ptr
// CHECK-SAME: section "llvma_v1"
// CHECK-LABEL: define internal void @{{.*}}use_arena{{.*}}resume
// CHECK: call ptr @__llvm_arena_addr_v1(

//--- arena.list
[arena]
name:coro_state

//--- test.cpp
namespace std {
template <class Return, class... Args> struct coroutine_traits {
  using promise_type = typename Return::promise_type;
};

template <class Promise = void> struct coroutine_handle {
  static coroutine_handle from_address(void *) noexcept { return {}; }
};

template <> struct coroutine_handle<void> {
  coroutine_handle() = default;
  template <class Promise>
  coroutine_handle(coroutine_handle<Promise>) noexcept {}
};
} // namespace std

struct suspend_always {
  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<>) noexcept {}
  void await_resume() noexcept {}
};

struct Task {
  struct promise_type {
    Task get_return_object() { return {}; }
    suspend_always initial_suspend() noexcept { return {}; }
    suspend_always final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
  };
};

int coro_state;

Task use_arena() {
  ++coro_state;
  co_await suspend_always{};
  ++coro_state;
}
