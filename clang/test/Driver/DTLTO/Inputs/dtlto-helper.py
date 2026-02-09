from pathlib import Path
import sys

# Arg 1: "clang" path.
p = Path(sys.argv[1])
resolved = p.resolve()
print(f"clang-name:{resolved.name}")
# Arg 2: Non-zero for LLVM driver.
# PrependArg is only set when the canonical (resolved) path differs from the
# original path name (e.g. symlinks on Linux: clang -> llvm-driver). On Windows,
# hardlinks resolve to the same name, so PrependArg is not needed.
if sys.argv[2] != "0" and resolved.stem.lower() != p.stem.lower():
    print(f'prepend-arg:"--thinlto-remote-compiler-prepend-arg={p.name}"')
else:
    print("prepend-arg: ")
