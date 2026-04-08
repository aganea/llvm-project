# RFC vs. Branch Gap Analysis: Dynamic Debugging for COFF

Comparison of the upstream ELF-focused RFC
([discourse.llvm.org/t/rfc-dynamic-debugging-for-c-step-through-unoptimized-code-in-optimized-builds/90113](https://discourse.llvm.org/t/rfc-dynamic-debugging-for-c-step-through-unoptimized-code-in-optimized-builds/90113))
against the `feat/dynamicdeopt_codeview` branch which implements `/dyndbg` for
COFF/CodeView on Windows.

The RFC targets ELF with a "nested ELF" design where the full unoptimized
binary is compiled ahead of time and stored as an ET_REL object in
`.debug_llvm_dyndbg`. This branch targets COFF with a fundamentally different
approach: three modes (AOT via `.alt.obj`, hybrid via embedded bitcode in
`.dyndbg`, and dynamic via source recompile using `LF_BUILDINFO`). Many RFC
items either translate differently to COFF or are not applicable.

---

## Compiler

### Implemented

| RFC Item | Branch Status | Notes |
|----------|---------------|-------|
| Module cloning for unoptimized copy | Done | `CloneModule` in `BackendUtil.cpp` for bitcode embed and AOT. |
| `preserve-abi` attribute on all functions | Done | `DynamicDebugPrepPass` adds `"preserve-abi"` string attr to every defined non-intrinsic function. |
| IPO guards (GlobalOpt, FuncSpec, DAE, ArgPromo) | Done | `GlobalOpt.cpp`, `FunctionSpecialization.cpp`, `DeadArgumentElimination.cpp`, `ArgumentPromotion.cpp` all check `"preserve-abi"`. |
| Alias promotion for internal-linkage symbols | Done | `.dyndbg.<hash>` suffix aliases added to `@llvm.used`. Both functions and global variables promoted. |
| Internal-linkage globals (non-COMDAT) promoted | Done | `DynamicDebugPrepPass` covers functions and global variables with `hasLocalLinkage()`. |

### Design Differences (Not Gaps)

| RFC Item | Branch Approach | Rationale |
|----------|-----------------|-----------|
| Minimum function size (`tail-pad-to-size=5`) | `/FUNCTIONPADMIN:14` via `.drectve` | COFF standard mechanism for hotpatch padding. The linker inserts NOP/INT3 padding before each function entry, which the debugger uses for indirect trampolines. No need for an LLVM-level attribute on Windows. |
| Shared debug strings / debug type units | N/A | The RFC notes this as a known limitation of the nested-ELF approach. The bitcode-based design avoids it entirely since codegen happens on demand. |
| LTO support | N/A | Explicitly a non-goal in both the RFC and this branch. |

### Missing

| RFC Item | Impact | Details |
|----------|--------|---------|
| Version/compatibility metadata (RFC: `.note` section) | Low | No version or compatibility information is emitted in any mode. The `DYDB` magic header in the `.dyndbg` section has no version field. Adding a version byte/word to the header would allow the debugger to reject incompatible bitcode from older compilers, and would help with forward compatibility. |

### COFF-Specific Additions Beyond the RFC

- **Three deoptimization modes** (AOT / hybrid / dynamic) vs. the RFC's single ahead-of-time approach.
- **Zstd-compressed bitcode** in `.dyndbg` with `DYDB` magic header.
- **Sidecar bitcode** option (`-fdynamic-debug-bitcode-sidecar`).
- **Parallel AOT codegen** via `std::async` with separate `LLVMContext`.
- **`DynamicDebugExternPass`** for recompile-time externalization — converts internal-linkage symbols into extern declarations using promoted `.dyndbg.<hash>` names so the `-O0` `.obj` only contains external references that the debugger resolves against the PDB.

---

## Linker

### Design Differences (Not Gaps)

| RFC Item | Branch Approach | Rationale |
|----------|-----------------|-----------|
| Nested relocatable link of inner modules | Not needed | The COFF approach stores bitcode (hybrid) or uses sidecar `.alt.obj` (AOT). There is no inner ELF to link; the debugger handles relocations at load time from the COFF `.obj`. |
| `--force-group-allocation` / section merging | N/A | Only relevant to the nested-ELF approach. |
| Symbol dependency handling (inner→outer) | Handled by `DynamicDebugExternPass` + debugger-side COFF relocation resolution against PDB symbols | Moves work from the linker to the debugger, avoiding nested-linking complexity. |

### Implemented

| Item | Notes |
|------|-------|
| `/FUNCTIONPADMIN` from `.drectve` | LLD COFF accepts `/FUNCTIONPADMIN` from `.drectve` sections with max-with-CLI semantics (`lld/COFF/Driver.cpp`). |

---

## Debugger

### Implemented

| RFC Item | Branch Status | Notes |
|----------|---------------|-------|
| Read relocations from inner module and apply | Done | `LoadObjIntoDebuggee` resolves AMD64 `IMAGE_REL_AMD64_REL32*` and `IMAGE_REL_AMD64_ADDR64` relocations. |
| Load `.text` into debuggee memory | Done | Section-by-section allocation via `Process::AllocateMemory` + `WriteMemory`, including BSS zero-init. |
| REL32 overflow handling | Done | Trampoline pool (`mov r11, imm64; jmp r11`) appended to each section. |
| Patch optimized function entry | Done | Indirect trampoline written into `/FUNCTIONPADMIN` padding + `JMP rel32` from the entry to that trampoline. |
| Unpatch / restore original bytes | Done | `UnpatchFunctionEntry` restores saved bytes. |
| Synthetic JIT module for backtraces | Done | `[Deoptimized] <name>` symbol registered via `ObjectFileJIT` so that disassembly and stack frames show the deoptimized function. |
| `dyndbg break` (deoptimize + breakpoint) | Done | Deoptimizes first, then creates a function-name breakpoint. Falls back to optimized breakpoint on failure. |
| Memory deallocation on reoptimize | Done | `DeallocateMemory` for text and data sections in `Reoptimize`. |

### COFF-Specific Value-Adds Beyond the RFC

- **PDB/CodeView `LF_BUILDINFO` metadata extraction** for automatic compiler path, source file, and command-line discovery.
- **Three-mode fallback** (AOT → hybrid → dynamic) with automatic detection.
- **Bitcode thinning** — lazy bitcode loading, materialize only the target function, externalize the rest — enabling per-function granularity without compiling the entire TU.
- **DAP protocol integration** — custom requests `__lldb_dyndbgDeoptimize`, `__lldb_dyndbgReoptimize`, `__lldb_dyndbgStatus` for IDE integration.
- **TU hash extraction from `.obj` symbol table** for correct multi-TU binary support.
- **`dyndbg` CLI** — multiword command with subcommands `deoptimize`, `reoptimize`, `break`, `status`.

### Missing — Ranked by Importance

#### 1. Step-into-unoptimized-code logic (High)

The RFC specifies that when stepping into a function call from unoptimized
code, the debugger should:

1. Put a temporary breakpoint on the call target.
2. After stopping, map the PC to the corresponding unoptimized function.
3. Set the PC to the start of the unoptimized function.

This "transparent step-through" behavior makes dynamic debugging feel seamless.
Without it, users must manually `dyndbg deoptimize` every function they want to
step into.

#### 2. File/line breakpoint integration (High)

The RFC describes a flow where file/line breakpoints (e.g. `b file.cpp:42`)
automatically trigger deoptimization: look up the address in the inner module's
line information, set the breakpoint in unoptimized code, and patch the
optimized function. Currently the user must explicitly `dyndbg deoptimize`
before the breakpoint will land in unoptimized code.

#### 3. Inliner map for inlined functions (High)

The RFC says the debugger should "using `DW_TAG_inlined_subroutine`, build a
map from each inlined function to a set of parent functions that inline it."
When a user sets a breakpoint on an inlined function, all parent (caller)
functions should be patched. The current implementation only supports explicit
function-name deoptimization and does not build or consult an inliner map.

#### 4. Detach cleanup (Medium)

The RFC says: "When detaching, the Debugger should remove any patches it has
added to optimised code." There is no hook that automatically calls
`ReoptimizeAll` on debugger detach. The user must manually `dyndbg reoptimize`
before detaching, or the process continues running with patches in place.

#### 5. Thread safety during patching (Medium)

The RFC warns: "If the PC of any threads is currently in a function to be
patched, it may not be safe to apply." A fallback mechanism using temporary
breakpoints should be used when any thread's PC is inside the target function.
`PatchFunctionEntry` currently writes directly to memory without checking
thread PCs. On x86_64 this is partially mitigated by the atomicity of the
5-byte JMP write, but is not safe in all cases (e.g. a thread in the
padding/prologue area being overwritten).

#### 6. Patch reference counting (Medium)

The RFC notes: "Users may create breakpoints that require patching of the same
function. Patches in optimised functions should be reference counted. Patches
can be removed when the reference count drops to zero." The current
`m_patches` map stores one `DynDbgPatchInfo` per function name. If two
breakpoints both need the same function deoptimized, the second `Deoptimize`
returns early ("already deoptimized"), which works. However, a single
`Reoptimize` call restores the optimized version even if another consumer
still depends on the deoptimized code.

#### 7. Re-attach optimization (Low)

The RFC suggests the debugger could leave unoptimized code and metadata in the
process so that re-attaching later avoids re-loading and re-relocating. Not
implemented, but explicitly described as optional in the RFC.

---

## Summary

The branch delivers a solid compiler and debugger foundation that covers the
core mechanics: `preserve-abi` IPO guards, alias promotion, bitcode embedding,
COFF loading/relocation, function-entry patching, and three flexible
deoptimization modes that go beyond the RFC's single ahead-of-time design.

The main gaps are on the debugger's UX/integration side — transparent
step-into, file/line breakpoint hooks, and inliner map support. These are what
make the feature feel "automatic" to the user rather than requiring explicit
`dyndbg` commands. Thread safety, detach cleanup, and reference counting are
important for correctness in production use.
