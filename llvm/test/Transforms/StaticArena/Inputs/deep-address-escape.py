import sys


count = int(sys.argv[1])


def emit_chain(prefix, base):
    previous = base
    for index in range(count):
        current = f"%{prefix}.{index}"
        print(f"  {current} = getelementptr i8, ptr {previous}, i64 0")
        previous = current
    return previous


print("@__llvm_arena_var_v1.arena_value = external global i8")
print("@bad_cache = global ptr null")
print("declare ptr @__llvm_arena_addr_v1(ptr)")

print("define void @deep_source() {")
print("  %arena = call ptr @__llvm_arena_addr_v1(")
print("      ptr @__llvm_arena_var_v1.arena_value)")
source = emit_chain("source", "%arena")
print(f"  store ptr {source}, ptr @bad_cache")
print("  ret void")
print("}")

print("define void @deep_destination() {")
print("  %arena = call ptr @__llvm_arena_addr_v1(")
print("      ptr @__llvm_arena_var_v1.arena_value)")
destination = emit_chain("destination", "@bad_cache")
print(f"  store ptr %arena, ptr {destination}")
print("  ret void")
print("}")

# Universal proof fails immediately on the alloca arm. The independent
# destination-name search must remain bounded while inspecting the other arm.
print("define void @early_failure_with_deep_destination(i1 %which) {")
print("  %slot = alloca i8")
print("  %arena = call ptr @__llvm_arena_addr_v1(")
print("      ptr @__llvm_arena_var_v1.arena_value)")
destination = emit_chain("early.destination", "@bad_cache")
print(f"  %destination = select i1 %which, ptr %slot, ptr {destination}")
print("  store ptr %arena, ptr %destination")
print("  ret void")
print("}")
