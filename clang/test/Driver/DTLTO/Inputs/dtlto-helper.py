from pathlib import Path
import platform
import sys

# Arg 1: "clang" path.
p = Path(sys.argv[1])
resolved = p.resolve()
# The driver uses the original (unresolved) binary name for
# --thinlto-remote-compiler, so we output the original name here.
print(f"clang-name:{p.name}")
# Arg 2: Non-zero for LLVM driver.
# PrependArg is only set when the canonical (resolved) path differs from the
# original path name (e.g. symlinks on Linux: clang -> llvm-driver).
# On Windows, argv[0] always reflects the invoked name (whether the binary is
# a hardlink or a symlink), so the multicall driver dispatches correctly based
# on argv[0] alone and no prepend arg is needed.
is_windows = platform.system() == "Windows"
if (not is_windows and sys.argv[2] != "0"
        and resolved.stem.lower() != p.stem.lower()):
    print(f'prepend-arg:"--thinlto-remote-compiler-prepend-arg={p.name}"')
else:
    print("prepend-arg: ")
