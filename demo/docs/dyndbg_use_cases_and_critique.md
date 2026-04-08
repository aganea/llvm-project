# Dynamic Debugging: Use Cases, Design Critique, and Mode Tradeoffs

## Terminology: Three Modes

This document refers to three deoptimization "modes." These are specific to our
COFF/CodeView branch (`feat/dynamicdeopt_codeview`); the upstream RFC and
MSVC's `/dynamicdeopt` both advocate exclusively for what we call **AOT mode**.

- **AOT (Ahead-Of-Time):** The unoptimized code is fully compiled at build time
  alongside the optimized code. The result is a sidecar `.alt.obj` (our branch)
  or an embedded/nested object (RFC's nested ELF, MSVC's `/dynamicdeopt`). At
  debug time, no compiler is needed — the unoptimized code is ready to load.
  This is the only mode described by the RFC and the only mode MSVC ships.

- **Hybrid:** Pre-optimization LLVM IR bitcode is embedded (zstd-compressed) in
  the `.obj` file's `.dyndbg` section at build time. At debug time, the
  debugger extracts the bitcode, thins it to a single function, and invokes the
  compiler backend to codegen at `-O0`. Requires the compiler to be available
  at debug time, but has much lower build-time and artifact-size overhead than
  AOT.

- **Dynamic:** Nothing is pre-built. At debug time, the debugger reads the
  original cc1 command line from `LF_BUILDINFO` in the PDB, re-invokes the
  compiler on the original source at `-O0` with `-fdynamic-debug-extern` to
  externalize all symbols except the target function. Requires the source code,
  compiler, and `.obj` files to all be available and unchanged. Lowest build
  cost, highest debug-time latency.

The hybrid and dynamic modes are unique to our branch. When this document
discusses the RFC or MSVC, it refers to the AOT approach unless stated
otherwise.

---

## The Problem Statement (That Nobody Pinpoints)

Neither MSVC's `/dynamicdeopt` documentation nor the LLVM RFC explicitly state
the problem they are solving. The closest articulation is the RFC's opening:

> Dynamic Debugging lets developers step through unoptimized code from optimized
> binaries. Many of our users are interested in this feature as it largely
> removes the historical trade-off of runtime performance for improved
> debuggability.

This is a description of the mechanism, not the problem. The actual problem has
several dimensions:

1. **Optimized code is hard to debug.** Variables are optimized away, code is
   reordered, functions are inlined or deleted, and DWARF/CodeView location
   expressions become partial or empty. Stepping through optimized code is
   confusing — the debugger jumps between source lines unpredictably.

2. **Rebuilding at `-O0` is expensive.** For large codebases (game engines,
   browsers, OS kernels), a full rebuild can take 20–60+ minutes. Developers
   cannot afford to context-switch between "optimized for running" and
   "unoptimized for debugging" builds.

3. **`-O0` builds change program behavior.** Timing-sensitive bugs (races, heisenbugs,
   performance cliffs) may not reproduce at `-O0`. Developers need to debug in
   the same binary that exhibits the problem.

4. **The debugger is the wrong layer for fixing this.** Heroic debugger
   heuristics (recovering optimized-away variables, reconstructing inlined
   frames) are inherently lossy and confusing. The only way to get a perfect
   debugging experience is to actually execute unoptimized code.

---

## Use Cases from the Programmer's Perspective

### UC1: Local Developer — Daily Edit-Build-Debug Cycle

**Persona:** C++ developer on a large codebase (game engine, browser, database).
Builds optimized (`-O2 -g` or `/O2 /Zi`) for testing because `-O0` is too slow
to run or takes too long to link.

**Workflow without dynamic debugging:**
1. Build at `-O2 -g`. Run. Hit a bug.
2. Either: add `printf` statements and rebuild (minutes), or try to debug
   optimized code (confusing), or rebuild the entire project at `-O0` (tens of
   minutes, plus the bug might not reproduce).

**Workflow with dynamic debugging:**
1. Build once at `-O2 -g /dyndbg`. Run. Hit a bug.
2. Attach debugger, set a breakpoint on the suspicious function.
3. The debugger transparently deoptimizes that function — execution steps
   through clean, unoptimized code with all variables visible.
4. Stepping out returns to optimized code at full speed.

**Key requirement:** Build time overhead of `/dyndbg` must be small enough to
use on every build. The RFC reports +15% compile time (geomean); this is
probably acceptable for most teams as a daily-driver flag.

**Which mode fits:** Hybrid (embedded bitcode) or Dynamic (source recompile).
AOT is too expensive for every build.

**Caveat — source drift:** Even for local developers, the dynamic and hybrid
modes assume the source code and compiler match the binary being debugged.
If the developer has switched branches, edited files since the build, or
upgraded the toolchain, the on-demand recompile will produce code that does not
correspond to the running binary. In these situations, only AOT mode is safe
because the unoptimized code was compiled from the exact same source at the
exact same time as the optimized code. This is not a daily occurrence, but it
happens — and it is an argument in favor of the RFC's AOT-only approach for
correctness guarantees, even at the cost of build time and artifact size.

---

### UC2: QA / Test Engineer — Debugging Someone Else's Build

**Persona:** QA engineer who receives pre-built binaries (`.exe` + `.pdb`) from
the development team or CI. They do not have the source code checked out, do
not have the compiler installed, and cannot rebuild.

**Workflow:**
1. Receive optimized build artifacts from CI.
2. Run the application, reproduce a bug.
3. Attach debugger, deoptimize the relevant function, step through clean code.

**Key requirement:** Everything needed for deoptimization must be
**self-contained** in the build artifacts. This means either:
- The full unoptimized `.obj` is embedded (AOT / nested ELF), or
- The bitcode is embedded in the `.obj` files AND the `.obj` files ship
  alongside the `.exe` + `.pdb` (hybrid mode), or
- The unoptimized code is already linked into the binary (MSVC/RFC approach).

**Which mode fits:** AOT (`.alt.obj` sidecar or embedded in `.exe`) or the
RFC's nested-ELF approach. Hybrid works only if `.obj` files are distributed.
Dynamic mode does NOT work (no source, no compiler).

**This is the use case that justifies linker support and ahead-of-time
compilation of unoptimized code.** Without it, the feature is limited to
developers who have the full build environment.

---

### UC3: CI Pipeline — Build Once, Debug Anywhere

**Persona:** Build engineer configuring CI. Wants optimized builds that any team
member can debug later without rebuilding.

**Workflow:**
1. CI builds with `/dyndbg:aot` (or equivalent). Produces `.exe` + `.pdb` +
   embedded deopt artifacts.
2. Artifacts are stored in the artifact repository.
3. Any developer or QA engineer can download and debug months later.

**Key requirement:** Reproducibility and self-containment. The deopt information
must be tied to the exact same compilation — not "recompile from whatever
source is on disk now." The version/hash must match.

**Which mode fits:** AOT only. The nested-ELF (RFC) or `.alt.obj` sidecar
approach. Hybrid with embedded bitcode also works if `.obj` files are archived
alongside.

**This is the use case that justifies the +190% binary size increase.** If
artifact storage is cheap (and for most CI systems it is), the tradeoff is
acceptable.

---

### UC4: Performance Engineer — Profiling + Debugging

**Persona:** Developer profiling an optimized build, sees a hot function behaving
unexpectedly. Wants to inspect what that function does step-by-step without
changing its callers' behavior.

**Workflow:**
1. Profile the application at `-O2`.
2. Identify a suspicious function.
3. Deoptimize just that function, set a breakpoint, inspect its behavior.
4. The rest of the application runs at full speed — the performance
   characteristics of the overall system are preserved.

**Key requirement:** Per-function granularity. Deoptimizing the entire binary
would change the performance profile; deoptimizing a single function is
acceptable.

**Which mode fits:** Hybrid (bitcode thinning to single function) or Dynamic
(recompile single TU at `-O0` with `-fdynamic-debug-extern`). AOT also works
but is coarser-grained (full TU).

---

### UC5: Game Studio — Long Iteration, Optimized-Only Workflow

**Persona:** Game developer on Unreal Engine 5. Full rebuild takes 40+ minutes.
Development builds are "Development" configuration (optimized with debug info).
Debug builds are too slow to play the game.

**Workflow:**
1. Build the game in Development configuration with `/dyndbg`.
2. Play-test the game at full speed.
3. Hit a gameplay bug. Attach the debugger.
4. Deoptimize the relevant gameplay function, step through it.
5. Fix the bug, hot-reload the fix (Live++, etc.), continue play-testing.

**Key requirement:** Zero runtime overhead when not debugging. The optimized
code runs untouched until the debugger explicitly patches a function.

**Which mode fits:** Hybrid is ideal — minimal build-time overhead (+15%), no
runtime cost, per-function on-demand deopt. Dynamic also works if source is
available.

---

## Use Cases That Dynamic Debugging Does NOT Address

### Post-Mortem / Crash Dump Analysis

Dynamic debugging requires a **live, running process**. It cannot help with:
- Analyzing `.dmp` / core files after a crash
- Debugging a process that has already terminated
- Offline crash analysis tools

The patches are applied to live memory; they have no meaning in a dump. This is
a significant limitation that neither the RFC nor MSVC documentation mentions
explicitly.

### Retail / Shipping Builds

No sane shipping pipeline will include `/dyndbg` artifacts in retail builds:
- +190% binary size is unacceptable for distribution
- The unoptimized code and bitcode are essentially the full source-equivalent
  (IP/security concern)
- Customers should not be debugging your optimized code

### Remote Debugging Without Build Artifacts

If a remote machine has only the `.exe` and no `.pdb` / `.obj` / `.alt.obj` /
bitcode, dynamic debugging cannot work. The deopt artifacts must be accessible
to the debugger.

### Debugging Third-Party Libraries

Dynamic debugging only works for code compiled with `/dyndbg`. If a bug is in
a third-party static or dynamic library not compiled with this flag, the feature
provides no benefit.

---

## Why Does the RFC Need Linker Support?

The RFC's nested-ELF design requires the linker to perform a relocatable link
of the unoptimized inner modules. This is specifically to support **UC2 and
UC3** — the "self-contained artifact" use cases where everything needed for
deoptimization is embedded in the final `.so` / executable.

The key insight the RFC doesn't state explicitly:

> **If you assume the developer always has source + compiler + `.obj` files,
> you don't need linker support at all.** You can recompile on demand (dynamic
> mode) or codegen from embedded bitcode (hybrid mode).

Linker support is necessary **only** when:
1. The `.obj` files are not available at debug time (they've been cleaned up
   after the build), AND
2. You want the deopt artifacts inside the linked binary rather than as
   sidecars.

The RFC's nested-ELF approach links the unoptimized modules into the final
binary so that the `.exe` / `.so` alone is sufficient. MSVC's `/dynamicdeopt`
produces two separate binaries (optimized + deoptimized). Both solve the same
problem: making the artifact self-contained.

**Our COFF approach avoids most linker complexity** by storing LLVM bitcode
(much smaller than compiled object code) in the `.obj` files' `.dyndbg`
sections. The linker passes these sections through unchanged. At debug time,
the debugger extracts the bitcode, thins it to a single function, and codegens
on demand. This requires the `.obj` files to be available but avoids the
entire nested-linking problem.

For the self-contained-artifact case, we offer AOT mode (`.alt.obj` sidecar)
which could be extended to embed the unoptimized code in the `.exe` if needed.

---

## Mode Tradeoff Summary

| | AOT | Hybrid (Bitcode) | Dynamic (Recompile) |
|---|---|---|---|
| **Build time overhead** | High (+40% or more — two full codegens) | Low (+15% — one codegen + bitcode serialize) | Minimal (just `preserve-abi` prep pass) |
| **Artifact size overhead** | High (~2x for `.alt.obj`) | Moderate (compressed bitcode ~10-30% of `.obj`) | None |
| **Debug-time latency** | None (pre-built) | Medium (per-function codegen, ~100-500ms) | High (full TU recompile, seconds) |
| **Self-contained artifacts** | Yes (sidecar `.alt.obj`) | Requires `.obj` files | Requires source + compiler |
| **Per-function granularity** | No (full TU) | Yes (bitcode thinning) | Possible (`-fdynamic-debug-extern-keep`) |
| **Requires compiler at debug time** | No | Yes (for codegen step) | Yes |
| **Best for** | UC2, UC3 (QA, CI) | UC1, UC4, UC5 (developers) | UC1 (local dev, lowest build cost) |

---

## Critique of the RFC

1. **No explicit problem statement.** The RFC describes a mechanism without
   clearly articulating which user scenarios it targets. This makes it hard to
   evaluate whether the design tradeoffs are appropriate.

2. **Only one mode (ahead-of-time).** The RFC proposes compiling the full
   unoptimized binary at build time — the "brute force" approach. This is the
   most expensive option in terms of build time and binary size, but it has a
   strong correctness advantage: the unoptimized code is guaranteed to match
   the optimized binary because both are compiled from the same source in the
   same invocation. On-demand alternatives (bitcode, recompile) are not
   explored, despite @aganea and @MolecularMatters raising this in the
   discussion. A middle ground — offering AOT as the default with lighter
   modes as opt-in alternatives — would address more use cases.

3. **+190% binary size and +15% compile time are presented without context.**
   Without knowing which use case justifies these costs, it's hard to evaluate
   them. For UC1 (local developer), +15% compile time might be fine but +190%
   binary size is wasteful since the developer could recompile on demand. For
   UC3 (CI), +190% size is acceptable because artifact storage is cheap.

4. **No discussion of partial / incremental deoptimization.** The RFC compiles
   the entire program unoptimized. But as @MolecularMatters points out,
   developers typically debug <5% of their code in a session. Per-function or
   per-TU deoptimization (as implemented in our hybrid/dynamic modes) would
   significantly reduce the upfront cost.

5. **Linker complexity is justified by an unstated use case.** The nested ELF
   approach requires significant linker changes — but only to support the
   "self-contained artifact" use case (UC2/UC3) that the RFC never explicitly
   describes. If the target audience is developers with source code, the linker
   work is unnecessary.

6. **Debugger UX items (step-into, file/line breakpoints) are underspecified.**
   The RFC's debugger section reads like a technical checklist rather than a
   user-experience design. The difference between "the debugger can do X" and
   "X happens transparently when the user does Y" is the difference between a
   feature and a useful feature.

---

## Estimated Usage Frequency

For a typical team of ~50 engineers (game studio, browser team, or similar
large C++ project), how often would each use case actually occur?

| Use Case | % of Sessions | Typical Frequency | Who |
|----------|---------------|-------------------|-----|
| UC1: Local dev debugging own build | ~75% | Multiple times/day | Every developer |
| UC5: Game studio (UC1 variant) | ~15% | Daily | Developers on optimized-only workflows |
| UC4: Performance engineer | ~5–8% | Weekly | 2–3 specialists on the team |
| UC3: Debug a CI artifact | ~2–3% | Monthly | Any developer, when a CI-only failure can't be reproduced locally |
| UC2: QA debugging someone else's build | ~1% | Quarterly | QA engineers rarely attach debuggers; they file bug reports with repro steps |

UC1 and UC5 are fundamentally the same pattern (developer debugging their own
local build) in different domains. Combined, they represent **~90% of all
real-world dynamic debugging usage**.

### Strategic Implication

The RFC's AOT-only approach (and MSVC's `/dynamicdeopt`) imposes a build-time
tax (+15% compile time) and artifact-size tax (+190% binary size) on **every
build** to support the ~3% of sessions where self-contained artifacts matter
(UC2, UC3). The 90% majority — local developers who have source, compiler, and
`.obj` files at hand — pay for something they don't need.

Our branch's hybrid mode is better calibrated to this reality: it handles the
90% case (UC1/UC5) with much lower build overhead (bitcode serialization only,
no second codegen pass), and offers AOT as an opt-in (`/dyndbg:aot`) for the
small percentage of sessions where self-contained artifacts are required.

The source-drift caveat (developer has switched branches or edited files since
the build) is real but narrow. Most debugging happens within minutes of the
build, not days later. The "I built this last week on a different branch, now I
need to debug it" scenario accounts for perhaps ~5% of UC1 sessions — not
enough to shift the overall picture, but enough to justify offering AOT as an
option rather than not having it at all.
