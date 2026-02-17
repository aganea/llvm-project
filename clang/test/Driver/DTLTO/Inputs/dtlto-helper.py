from pathlib import Path
import sys

# Arg 1: "clang" path.
p = Path(sys.argv[1])
resolved = p.resolve()
print(f"clang-name:{resolved.name}")
# Arg 2: Non-zero for LLVM driver.
# The driver only sets PrependArg when the resolved executable path no longer
# identifies the tool (e.g. symlink "clang" -> "llvm").  On Windows with hard
# links, the resolved name is still "clang.exe", so no prepend arg is emitted.
need_prepend = False
if sys.argv[2] != "0":
    tool_stem = p.stem.lower()            # e.g. "clang"
    resolved_stem = resolved.stem.lower()  # e.g. "clang" or "llvm"
    need_prepend = tool_stem not in resolved_stem
if need_prepend:
    print(f'prepend-arg:"--thinlto-remote-compiler-prepend-arg={p.name}"')
else:
    print("prepend-arg: ")
