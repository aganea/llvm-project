# Clang vs MSVC `/dynamicdeopt` -- Object File Comparison and Design Analysis

**Date:** March 2026
**Status:** Living document -- updated as implementation progresses
**Test source:** `demo/dynamic-debugging/src/math_utils.cpp`

```cpp
static int g_call_count = 0;

static int multiply(int a, int b) {
    g_call_count++;
    return a * b;
}

int compute(int x, int y) {
    int product = multiply(x, y);
    int clamped = clamp(product, -1000, 1000);
    return clamped + g_call_count;
}
```

Compiled with:
- **MSVC:** `cl /O2 /Z7 /dynamicdeopt /c math_utils.cpp`
- **Clang:** `clang-cl /O2 /Z7 /dynamicdeopt:aot /c math_utils.cpp`

Both produce a pair: `math_utils.obj` (optimized) + `math_utils.alt.obj` (unoptimized).

---

## 1. Output File Comparison

### 1.1 File Sizes

| Artifact | Clang | MSVC |
|---|---|---|
| Optimized `.obj` | 6,287 B | 4,994 B |
| Unoptimized `.alt.obj` | 7,151 B | 4,981 B |

Clang's files are ~30% larger, mainly due to a larger `.debug$T` type stream
(0xDB0 vs 0x508) and additional `.xdata`/`.pdata` entries in the `.alt.obj`.

### 1.2 Section Layout

**Optimized `.obj`:**

| Section | MSVC | Clang | Notes |
|---|---|---|---|
| `.text$mn` / `.text` | 3 COMDAT sections | 2 COMDAT sections | MSVC uses `$mn` suffix (COMDAT group naming convention) |
| `.bss` | `?g_call_count@@3HA` | `g_call_count.dyndbg.<hash>` | Different naming for statics (see Section 3) |
| `.debug$S` | Present | Present | |
| `.debug$T` | 0x508 bytes | 0xDB0 bytes | Clang emits more verbose type info |
| `.pmd_optdep` | **Yes** (0x91 B) | **No** | MSVC-specific: optimization dependency graph |
| `.pmd_uniqueid` | **Yes** (0x50 B) | **No** | MSVC-specific: 128-bit unique IDs per symbol |
| `.chks64` | **Yes** (0x68 B) | **No** | MSVC-specific: checksums |
| `.llvm_addrsig` | **No** | **Yes** | LLVM-specific: address-significance table |
| `.drectve` | 0x2F bytes | 0x96 bytes | Clang emits more linker directives |

**Unoptimized `.alt.obj`:**

| Section | MSVC | Clang | Notes |
|---|---|---|---|
| `.text$mn` / `.text` | 3 sections | 3 sections | Same function count |
| `.xdata` | 1 entry (compute only) | 3 entries (all functions) | See Section 2.3 |
| `.pdata` | 1 entry | 3 entries | |
| `.pmd_uniqueidref` | **Yes** (0xB4 B) | **No** | MSVC-specific: cross-reference fixup table |
| `.pmd_uniqueid` | **Yes** (0x50 B) | **No** | Same content as in `.obj` |
| `.chks64` | **Yes** (0x78 B) | **No** | |

---

## 2. Code Generation Comparison

### 2.1 Optimized Code

Both compilers produce comparable optimized output. `multiply` is inlined into
`compute`, `clamp` is inlined, and `g_call_count` is accessed via RIP-relative
addressing. The optimized codegen quality is roughly equivalent for this test
case.

MSVC emits a separate COMDAT body for all three functions (`clamp`, `compute`,
`multiply`) even in the optimized `.obj`. Clang's prep pass promotes
`multiply` with a `.dyndbg.<hash>` suffix and emits it as a separate COMDAT,
but `clamp` (being a header-inline candidate) may be fully inlined and
eliminated.

### 2.2 Unoptimized Code -- The Critical Difference

This is where the two approaches diverge architecturally.

**MSVC uses `movabs` + indirect calls:**

```asm
; compute() in msvc_math_utils.alt.obj
movabsq  $0x0, %rax        ; 10-byte mov, patched by linker
callq    *%rax              ; indirect call to multiply()
...
movabsq  $0x0, %rax        ; patched to clamp()
callq    *%rax
...
movabsq  $0x0, %rax        ; patched to &g_call_count
movl     (%rax), %eax      ; indirect load
```

Every cross-reference (function call, global access) uses a 10-byte
`movabs`+`call *%rax` or `movabs`+`mov (%rax)` sequence. The 64-bit immediate
is filled in by the linker via the `.pmd_uniqueidref` section. There are **zero
standard `.text` relocations** in MSVC's `.alt.obj`.

**Clang uses standard RIP-relative addressing:**

```asm
; compute() in math_utils.alt.obj
callq    multiply          ; standard REL32 relocation
...
callq    clamp             ; standard REL32 relocation
...
addl     (%rip), %eax      ; RIP-relative access to g_call_count
```

Standard `IMAGE_REL_AMD64_REL32` relocations, no custom sections needed. This
is identical to what a normal `-O0` compilation would produce.

### 2.3 Unwind Information

MSVC only emits `.xdata`/`.pdata` for `compute` in the `.alt.obj`. The
simpler leaf functions (`clamp`, `multiply`) have no unwind info, presumably
because they don't touch the stack pointer.

Clang emits unwind info for all three functions. This is correct (Clang's
`-O0` codegen uses `push`/`sub rsp` even in small functions) but adds size.

---

## 3. Symbol Naming and Identity

### 3.1 Static/Internal Symbols

| Symbol | MSVC | Clang |
|---|---|---|
| `g_call_count` | `?g_call_count@@3HA` (C++ mangled, static linkage) | `g_call_count.dyndbg.<TU-hash>` (promoted to external) |
| `multiply` | `?multiply@@YAHHH@Z` (static linkage in both files) | `?multiply@@YAHHH@Z.dyndbg.<TU-hash>` (promoted to external in `.obj`) |

MSVC keeps static symbols with their original mangled names and static
linkage. The `.pmd_uniqueid` section assigns 128-bit UUIDs that the linker
uses to correlate symbols between `.obj` and `.alt.obj`.

Clang's prep pass promotes internal-linkage symbols to external with a
`.dyndbg.<TU-hash>` suffix, so they survive linking and can be resolved at
debug time. The `.alt.obj` retains the original names (no suffix) since it
is a standalone compilation unit.

### 3.2 Function Identity

| Mechanism | MSVC | Clang |
|---|---|---|
| **Matching `.obj` to `.alt.obj`** | `.pmd_uniqueid`: 128-bit UUID per function, present in both files | TU-level hash derived from source path + compiler flags |
| **Granularity** | Per-function (survives single-function edits) | Per-TU (any change to the TU invalidates the match) |
| **Callsite resolution** | `.pmd_uniqueidref` maps callsites to target UUIDs | Standard relocations against symbol names |

---

## 4. MSVC Proprietary Metadata Sections

### `.pmd_optdep` (Optimization Dependency Graph)

Present in the **optimized `.obj` only**. Contains records describing which
functions were inlined into which callers, enabling the debugger to know:
- "If the user wants to step into `multiply()` from inside `compute()`, we
  need to deoptimize `compute()` because `multiply()` was inlined there."
- Symbol: `__g_dd_optdep`

### `.pmd_uniqueid` (Symbol Identity Table)

Present in **both** `.obj` and `.alt.obj` with identical content. Maps each
function and global variable to a 128-bit UUID. Records are 20 bytes:
16-byte UUID + 4-byte zero padding, with `IMAGE_REL_AMD64_ADDR32NB`
relocations pointing to the associated symbol.

- Symbol: `__g_dd_uniqueid`

### `.pmd_uniqueidref` (Callsite Fixup Table)

Present in the **`.alt.obj` only**. Maps each `movabs $0x0` site in the
unoptimized code to the UUID of the target function or variable. The linker
resolves these by looking up UUIDs in the optimized `.obj`'s `.pmd_uniqueid`
table and patching the absolute addresses.

- Symbol: `__g_dd_fixupfixup`
- Contains `IMAGE_REL_AMD64_ADDR64` relocations pointing to function symbols

### `.chks64` (Checksums)

Present in both files. Purpose not fully documented; likely used for
incremental build validation.

---

## 5. Pros and Cons

### 5.1 MSVC Approach

**Pros:**
- **64-bit absolute addressing**: The `movabs` pattern can target any address
  in the 64-bit space. When the debugger loads `.alt.exe` code into the
  debuggee, there are no range limitations -- the absolute addresses just work.
- **Per-function identity**: The UUID system allows matching individual
  functions even across partial rebuilds or when only some files change.
- **Optimization dependency tracking**: `.pmd_optdep` gives the debugger a
  precise inlining map, enabling it to know exactly which functions to
  deoptimize when stepping into an inlined callee.
- **Visual Studio integration**: Full IDE support with "[Deoptimized]" markers,
  seamless step-in, and automatic deopt/reopt lifecycle.

**Cons:**
- **Proprietary metadata format**: `.pmd_*` sections are undocumented, only
  understood by MSVC's linker and Visual Studio's debugger. Third-party tools
  cannot consume them.
- **Requires custom linker support**: The `/DYNAMICDEOPT` linker flag is
  required to process `.alt.obj` files. A standard linker (lld, GNU ld)
  cannot link these without extension.
- **Code size overhead**: Every call and global access uses a 12-byte
  `movabs`+`call *%rax` sequence instead of a 5-byte `call rel32`, roughly
  2.4x the code size per callsite.
- **Indirect call overhead**: Even in the unoptimized path, every function call
  goes through a register. This adds a branch-predictor penalty and prevents
  the CPU from speculatively fetching the target (minor concern since this is
  debug-only code).
- **Dual binary bloat**: Full `.alt.exe` + `.alt.pdb` alongside the optimized
  binary roughly doubles distribution size.

### 5.2 Clang Approach (Current PoC)

**Pros:**
- **Standard COFF output**: The `.alt.obj` is a normal object file with
  standard relocations. Any COFF-aware linker (lld-link, MSVC link.exe, even
  GNU tools) can process it without custom support.
- **Simpler implementation**: No proprietary metadata sections, no custom
  relocation scheme. The compiler just emits a second `-O0` compilation of the
  same module.
- **Three-mode flexibility**: Supports `dynamic` (source recompilation),
  `hybrid` (embedded bitcode), and `aot` (pre-built `.alt.obj`), letting users
  choose the build-time/debug-time tradeoff.
- **Smaller code**: Standard 5-byte `call` instructions, standard RIP-relative
  data access. The unoptimized code is more compact per-callsite.
- **Bitcode-based modes**: The `hybrid` mode stores LLVM IR, enabling
  per-function extraction and recompilation. This avoids building (and
  distributing) a full `.alt.exe` -- only the functions the debugger actually
  needs are compiled on-demand.

**Cons:**
- **RIP-relative range limit**: `IMAGE_REL_AMD64_REL32` requires the target
  to be within +/-2GB of the callsite. When the debugger loads unoptimized
  code into the debuggee, it must allocate memory near the original module.
  This is usually feasible but could fail in pathological cases (very large
  address spaces, ASLR).
- **TU-level granularity**: The current identity scheme uses a per-TU hash.
  A change to any function in the TU invalidates the entire match. (Can be
  improved with per-function hashing.)
- **No inlining dependency map yet**: We don't emit `.pmd_optdep`-equivalent
  data, so the debugger doesn't know which callers inlined a given function.
  This must be reconstructed from PDB `S_INLINESITE` records at debug time.
- **No Visual Studio compatibility**: Our `.alt.obj` format is not compatible
  with VS's Dynamic Debugging feature. A user switching between compilers
  would need different debugger support.

### 5.3 Summary Matrix

| Dimension | MSVC | Clang (PoC) |
|---|---|---|
| Linker compatibility | MSVC only (`/DYNAMICDEOPT`) | Any COFF linker |
| Debugger compatibility | Visual Studio only | LLDB (planned), potentially others |
| Code density (`.alt.obj`) | Lower (12B per callsite) | Higher (5B per callsite) |
| Address range constraint | None (64-bit abs) | +/-2GB (REL32) |
| Function identity | Per-function UUID | Per-TU hash (improvable) |
| Inlining map | `.pmd_optdep` | PDB `S_INLINESITE` (at debug time) |
| Build modes | Single (AOT only) | Three (dynamic / hybrid / aot) |
| On-demand per-function | No (full `.alt.exe` required) | Yes (hybrid mode: extract + compile one function) |
| Distribution cost | 2x (`.alt.exe` + `.alt.pdb`) | Flexible (bitcode ~10-20% of `.obj`, or full `.alt.obj`) |

---

## 6. Should Clang Use a Different Flag Name?

### The Problem

MSVC's `/dynamicdeopt` produces a specific, well-defined output format:
proprietary `.pmd_*` metadata sections, `movabs`-based indirect calls, and a
requirement for the MSVC linker's `/DYNAMICDEOPT` flag. Clang's implementation
uses the same flag name but produces fundamentally different object files. A
user who builds with `clang-cl /dynamicdeopt` and then tries to link with MSVC's
`link.exe /DYNAMICDEOPT` will get unexpected results (missing `.pmd_*` sections,
wrong relocation style), and vice versa.

### Recommendation: Rename to `/dyndbg`

Rename the Clang flag to **`/dyndbg`** (short for "dynamic debugging") with
three sub-modes:

```
clang-cl /dyndbg:dynamic ...   # Minimal: preserve symbols, recompile from source at debug time
clang-cl /dyndbg:hybrid  ...   # Store LLVM bitcode in .obj, extract + compile per-function at debug time
clang-cl /dyndbg:aot     ...   # Pre-build unoptimized .alt.obj at compile time
```

Rationale:

1. **Avoids user confusion**: `/dynamicdeopt` has an established meaning in
   the MSVC ecosystem. Using the same name for a different (incompatible)
   implementation creates a false expectation of interoperability.

2. **Signals a different design philosophy**: MSVC's approach is "full AOT
   dual-binary." Clang's approach spans a spectrum from lightweight (dynamic)
   to full AOT, with hybrid bitcode as the recommended default. A distinct
   flag name communicates that this is a different feature, not a clone.

3. **Keeps the door open for compatibility**: If we ever add a true
   MSVC-compatible mode (emitting `.pmd_*` sections, `movabs` codegen,
   `/DYNAMICDEOPT` linker support), we could offer it as
   `/dynamicdeopt` or `/dyndbg:msvc-compat`, making it clear which behavior
   the user gets.

4. **Precedent**: Clang already diverges from MSVC on flags where the
   underlying mechanism differs (e.g., `/Zi` vs `/Z7` behavior, sanitizer
   flags). Using a distinct name for a distinct feature is normal.

5. **Three modes remain**: The rename is cosmetic; the internal architecture
   (`dynamic`, `hybrid`, `aot`) is unchanged. The `-cc1` flags
   (`-fdynamic-debug-prep`, `-fdynamic-debug-bitcode`, `-fdynamic-debug-aot`)
   also remain unchanged.

### Alternative: Keep `/dynamicdeopt` with a Warning

If renaming is too disruptive, an alternative is to keep `/dynamicdeopt` but
emit a remark:

```
remark: Clang's /dynamicdeopt produces output incompatible with MSVC's
        /DYNAMICDEOPT linker flag. Use lld-link or Clang's llvm-dyndbg
        for debugging support. [-Rclang-dynamicdeopt]
```

This is less clean but preserves flag-name familiarity for users migrating
build systems.

### Migration Path

If the eventual goal is MSVC compatibility, the work would involve:

1. Emitting `.pmd_uniqueid` sections in both `.obj` and `.alt.obj`
2. Using `movabs`-based codegen for the `.alt.obj` path
3. Emitting `.pmd_optdep` (optimization dependency graph) in the `.obj`
4. Emitting `.pmd_uniqueidref` (callsite fixup table) in the `.alt.obj`
5. Teaching lld-link to process `/DYNAMICDEOPT` (match UUIDs, resolve fixups)

This could be offered as `/dyndbg:msvc-compat` or, once fully compatible,
as a re-introduction of the `/dynamicdeopt` name.

---

## 7. Appendix: Raw Object Dump

### Clang Optimized (`math_utils.obj`) -- Disassembly

```asm
; compute() -- multiply and clamp inlined, g_call_count RIP-relative
0: movl    (%rip), %eax              ; load g_call_count
6: leal    0x1(%rax), %r8d           ; g_call_count + 1
a: movl    %r8d, (%rip)              ; store g_call_count
11: imull   %edx, %ecx               ; x * y (inlined multiply)
14: cmpl    $0xfffffc19, %ecx         ; clamp lower bound check
1a: movl    $0xfffffc18, %edx
1f: cmovgel %ecx, %edx
22: cmpl    $0x3e8, %edx              ; clamp upper bound check
28: movl    $0x3e8, %ecx
2d: cmovll  %edx, %ecx
30: addl    %ecx, %eax                ; clamped + g_call_count
32: incl    %eax
34: retq
```

### MSVC Optimized (`msvc_math_utils.obj`) -- Disassembly

```asm
; compute() -- multiply and clamp inlined
0: movl    (%rip), %eax              ; load g_call_count
6: imull   %edx, %ecx               ; x * y
9: incl    %eax                      ; g_call_count++
b: movl    $0xfffffc18, %edx
10: movl    %eax, (%rip)              ; store g_call_count
16: cmpl    %edx, %ecx               ; clamp logic
18: jge     0x1d
1a: addl    %edx, %eax
1c: retq
1d: movl    $0x3e8, %edx
22: cmpl    %edx, %ecx
24: cmovgl  %edx, %ecx
27: addl    %ecx, %eax
29: retq
```

### Clang Unoptimized (`math_utils.alt.obj`) -- Disassembly

```asm
; compute() -- standard -O0 codegen with REL32 calls
 0: subq    $0x38, %rsp
 4: movl    %edx, 0x34(%rsp)
 8: movl    %ecx, 0x30(%rsp)
 c: movl    0x34(%rsp), %edx
10: movl    0x30(%rsp), %ecx
14: callq   multiply                  ; REL32 relocation
19: movl    %eax, 0x2c(%rsp)
2c: callq   clamp                     ; REL32 relocation
31: movl    %eax, 0x28(%rsp)
39: addl    (%rip), %eax              ; REL32 -> g_call_count
43: retq
```

### MSVC Unoptimized (`msvc_math_utils.alt.obj`) -- Disassembly

```asm
; compute() -- movabs + indirect call pattern
 0: movl    %edx, 0x10(%rsp)
 4: movl    %ecx, 0x8(%rsp)
 8: subq    $0x38, %rsp
 c: movabsq $0x0, %rax               ; patched to &multiply by linker
16: movl    0x48(%rsp), %edx
1a: movl    0x40(%rsp), %ecx
1e: callq   *%rax                     ; indirect call
24: movabsq $0x0, %rax               ; patched to &clamp
34: movl    $0xfffffc18, %edx
39: movl    0x20(%rsp), %ecx
3d: callq   *%rax                     ; indirect call
43: movabsq $0x0, %rax               ; patched to &g_call_count
4d: movl    (%rax), %eax              ; indirect load
55: addl    %eax, %ecx
57: movl    %ecx, %eax
5b: retq
```

### Relocation Comparison (`.text` sections only)

**Clang `.alt.obj`:**
```
.text (compute): 3 relocations
  IMAGE_REL_AMD64_REL32 -> ?multiply@@YAHHH@Z
  IMAGE_REL_AMD64_REL32 -> ?clamp@@YAHHHH@Z
  IMAGE_REL_AMD64_REL32 -> g_call_count

.text (multiply): 2 relocations
  IMAGE_REL_AMD64_REL32 -> g_call_count  (load)
  IMAGE_REL_AMD64_REL32 -> g_call_count  (store)
```

**MSVC `.alt.obj`:**
```
.text: 0 relocations
(All fixups are in .pmd_uniqueidref via IMAGE_REL_AMD64_ADDR64)
```

---

## 8. Linked Executable Analysis

### 8.1 Build Pipeline

Full build with MSVC `/dynamicdeopt` and linker `/DYNAMICDEOPT`:
```
cl /O2 /Z7 /EHsc /dynamicdeopt /c math_utils.cpp main.cpp
link /DEBUG:FULL /DYNAMICDEOPT math_utils.obj main.obj /OUT:demo.exe
```

This produces six files from two source files:

| File | Size | Description |
|---|---|---|
| `demo.exe` | 548,864 B | Optimized executable |
| `demo.pdb` | 6,098,944 B | Optimized PDB |
| `demo.alt.exe` | 550,400 B | Unoptimized "template" executable |
| `demo.alt.pdb` | 6,098,944 B | Unoptimized PDB with fixup data |

### 8.2 The `.alt.exe` is a Non-Runnable Template

A critical discovery: **`demo.alt.exe` crashes on launch** (access violation).
All `movabs` instructions remain as `mov rax, 0` in the linked executable:

```asm
; compute() in demo.alt.exe -- all addresses are zero!
0000000140001060: mov  dword ptr [rsp+10h], edx
0000000140001064: mov  dword ptr [rsp+8], ecx
0000000140001068: sub  rsp, 38h
000000014000106C: mov  rax, 0              ; <-- still zero after linking!
0000000140001076: mov  edx, dword ptr [rsp+48h]
000000014000107A: mov  ecx, dword ptr [rsp+40h]
000000014000107E: call rax                 ; <-- NULL call -> access violation
```

The MSVC linker deliberately leaves these as zero. The `.alt.exe` is a
**template** -- a pre-linked PE image with holes that the **debugger**
patches at runtime, using information from the `.alt.pdb`.

By contrast, `demo.exe` (optimized) runs correctly:
```
Step 1: result = 3
...
Final: 495
```

### 8.3 PE Sections (Linked Executables)

**`demo.exe` (optimized) -- 7 sections:**

| Section | VSize | Flags | Notes |
|---|---|---|---|
| `.text` | 0x6C810 | Execute/Read | Optimized code |
| `.rdata` | 0x12C7C | Read | Read-only data |
| `.data` | 0x2598 | Read/Write | Mutable data (g_call_count) |
| `.pdata` | 0x498C | Read | Exception unwind info |
| `.pmd_uni` | 0x118 | Read | UniqueID table (zeroed at load) |
| `.fptable` | 0x100 | **Read/Write** | Function pointer table (zeroed) |
| `.reloc` | 0x834 | Read/Discardable | Base relocations |

**`demo.alt.exe` (unoptimized) -- 8 sections:**

| Section | VSize | Flags | Notes |
|---|---|---|---|
| `.text` | 0x6C810 | Execute/Read | Unoptimized code (with zero-filled movabs) |
| `.rdata` | 0x12C7C | Read | |
| `.data` | 0x2598 | Read/Write | |
| `.pdata` | 0x498C | Read | |
| `.pmd_uni` (1) | 0x118 | Read | UniqueID table |
| `.pmd_uni` (2) | 0x21C | Read | UniqueIDRef table (callsite fixup data) |
| `.fptable` | 0x100 | **Read/Write** | Function pointer table |
| `.reloc` | 0x834 | Read/Discardable | |

Key observations:
- Both exes have identical `.text` size (0x6C810 = 444,432 bytes), even
  though the alt.exe has the CRT code optimized identically. Only the
  user functions differ.
- `.pmd_uni` in the alt.exe has **two** instances (0x118 + 0x21C = 0x334
  bytes total) vs one in the optimized exe (0x118). The second one holds
  the `UniqueIDRef` fixup table.
- `.fptable` is Read/Write in both, and **all zeros**. The debugger likely
  writes function pointers here at runtime that the `movabs $0` sites can
  redirect to.
- All `.pmd_uni` data is zeroed in the PE image. The actual UUID data lives
  in the PDB named streams.

---

## 9. PDB Named Streams -- The Debugger's Roadmap

The PDBs contain several named streams specific to `/dynamicdeopt`:

### 9.1 Named Stream Map

**`demo.pdb` (optimized):**
| Stream | Index | Size | Purpose |
|---|---|---|---|
| `/OptDep` | 248 | 318 B | Optimization dependency graph (inlining map) |
| `/UniqueID` | 15 | 288 B | Function/global UUID table |

**`demo.alt.pdb` (unoptimized):**
| Stream | Index | Size | Purpose |
|---|---|---|---|
| `/UniqueID` | 13 | 288 B | Same UUID table (for correlation) |
| `/UniqueIDRef` | 15 | 548 B | Callsite-to-UUID fixup map |
| `<DynamicDeoptimizeData>` | 249 | 576 B | Master deoptimization control data |

### 9.2 `/UniqueID` Stream (288 bytes, both PDBs)

Magic: `STIU` (0x53544955 = "UniqueID STream")

Contains per-function/per-global records mapping 128-bit UUIDs to RVAs.
The UUIDs are identical between `demo.pdb` and `demo.alt.pdb` (the only
difference is the RVAs, which differ because the same functions are at
different addresses in the two executables).

Approximate record format (20 bytes each):
```
[16-byte UUID] [4-byte RVA or section-relative offset]
```

The stream contains entries for all user-defined symbols: `g_call_count`,
`compute`, `clamp`, `multiply`, `main`, `printf`, and several CRT functions.

### 9.3 `/OptDep` Stream (318 bytes, optimized PDB only)

Magic: `STPO` (0x5354504F = "OPtimization dependency STream")

Contains the optimization dependency graph -- records which functions were
inlined into which callers. Each record references source and target UUIDs:

Approximate structure:
```
[16-byte source UUID] [count: u32] [flags: u32]
[16-byte target UUID] ... (repeated per dependency)
```

For our demo, this encodes:
- `compute` depends on `multiply` (inlined)
- `compute` depends on `clamp` (inlined)
- `main` depends on `compute` (called, not inlined -- but tracked for callsite info)

This enables Visual Studio to know: "When the user sets a breakpoint in
`multiply()`, we also need to deoptimize `compute()` because it inlined
`multiply()`."

### 9.4 `/UniqueIDRef` Stream (548 bytes, alt PDB only)

Magic: `UXIF` (0x55584946 = "UniqueID Xref/FIxup")

Maps each `movabs $0` site in the `.alt.exe` to the UUID of the target
function or variable. The debugger reads this stream, looks up each UUID in
the optimized exe's `/UniqueID` stream, resolves the RVA, and patches the
`movabs` immediate in the `.alt.exe`'s in-memory image.

Record format (variable):
```
[16-byte target UUID] [count: u32]
[RVA of movabs site: u32] [flags: u32] [extra: u32] ...
```

### 9.5 `<DynamicDeoptimizeData>` Stream (576 bytes, alt PDB only)

The master control stream. Contains:
- Function count and metadata counts
- Per-function deoptimization records mapping optimized RVAs to alt RVAs
- Paired optimized/alt RVA tables for the debugger to set up the
  deopt/reopt redirects (the "[Deoptimized]" indicator in VS)

This stream includes **both** the optimized and alt RVAs side-by-side,
enabling the debugger to:
1. Intercept execution at the optimized function entry
2. Redirect to the alt function at the corresponding alt RVA
3. Display the "[Deoptimized]" marker
4. Later restore the original when the breakpoint is removed

The final 16 bytes match the optimized PDB's GUID:
```
3D7AC79F-2DF5-4949-B422-6C05D464DEB0
```
This links the alt PDB to its optimized counterpart.

---

## 10. CodeView Symbol Records

### 10.1 `S_ALTOBJNAME` (alt PDB only)

A new CodeView symbol record type present in each module of the `.alt.pdb`:
```
88 | S_ALTOBJNAME [size = 80]
     bytes: c:\git\llvm-project\demo\dynamic-debugging\msvc_build\math_utils.obj
```

This tells the debugger the path to the **optimized** `.obj` file that
corresponds to this alt module. It enables symbol correlation: the debugger
can read the optimized `.obj` to find the original function addresses.

### 10.2 `S_COMPILE3` Flags Difference

**Optimized PDB:**
```
flags = security checks | hot patchable
```

**Alt PDB:**
```
flags = hot patchable
```

The alt code drops `security checks` (no `/GS` buffer overrun detection in
the unoptimized build). The `hot patchable` flag remains in both, ensuring
that function entry points have the 2-byte `mov edi, edi` or NOP sled
that enables runtime patching.

### 10.3 `S_INLINESITE` / `S_INLINEES` (optimized PDB)

The optimized PDB contains standard inlining records in `compute`:
```
664 | S_INLINESITE: inlinee = 0x100D (clamp), parent = compute
700 | S_INLINESITE: inlinee = 0x1011 (multiply), parent = compute
764 | S_INLINEES: [clamp, multiply]
```

These encode the binary annotations (line/column deltas, code offsets)
that map inlined code ranges back to the original source. The alt PDB has
**no** `S_INLINESITE` records for our functions -- since they're compiled
at `-O0`, nothing is inlined.

### 10.4 Function Debug Flags

**Optimized PDB:**
```
S_GPROC32 `compute`: flags = opt debuginfo
S_GPROC32 `clamp`: flags = opt debuginfo
S_LPROC32 `multiply`: flags = opt debuginfo
```

**Alt PDB:**
```
S_GPROC32 `compute`: flags = none
S_GPROC32 `clamp`: flags = none
S_LPROC32 `multiply`: flags = none
```

The `opt debuginfo` flag tells the debugger that variable locations may be
incomplete or optimized away. The alt PDB has `flags = none`, indicating
full debug fidelity -- all variables are on the stack with reliable locations.

### 10.5 Variable Location Quality

**Optimized PDB -- register-based, with gaps:**
```
S_LOCAL 'x': type=int, flags = param
S_DEFRANGE_REGISTER: register = ECX, range = [0001:0064, +9)
```
Variables live in registers for short ranges.

**Alt PDB -- stack-based, full scope:**
```
S_REGREL32 'x': type = int, register = RSP, offset = 64
```
Variables live on the stack for the entire function. The debugger can always
read them -- this is the whole point of deoptimization.

---

## 11. Runtime Deoptimization Flow (Reconstructed)

Based on the analysis of the PDB streams and PE structure, the Visual Studio
debugger performs the following sequence when a user requests deoptimization
of a function (e.g., stepping into an inlined call):

1. **Read `/OptDep`** from `demo.pdb`: Determine which optimized functions
   inline the target. If `multiply` is the target and it's inlined into
   `compute`, the debugger knows it must deoptimize `compute`.

2. **Read `<DynamicDeoptimizeData>`** from `demo.alt.pdb`: Find the
   optimized-to-alt RVA mapping for `compute`.

3. **Read `/UniqueIDRef`** from `demo.alt.pdb`: Find all `movabs $0` fixup
   sites within alt-`compute`. Each site has a target UUID.

4. **Read `/UniqueID`** from `demo.pdb`: Resolve each target UUID to an
   optimized RVA. For example, the UUID for `multiply` resolves to
   `0x140001080` in the optimized exe.

5. **Map `.alt.exe` into debuggee memory**: Load the relevant `.text`
   pages from the alt exe. Patch each `movabs $0` with the resolved
   absolute address from step 4.

6. **Patch `.fptable`**: If the `.fptable` section is used as an
   indirection layer, write resolved function pointers there.

7. **Redirect execution**: Patch the optimized `compute` entry point with
   a `jmp` to the alt-`compute`. The `hot patchable` flag ensures there's
   room for this patch.

8. **Display "[Deoptimized]"**: Show the alt function's source mapping
   (from `demo.alt.pdb`) in the VS debugger UI.

9. **On reoptimization** (breakpoint removed): Restore the original bytes
   at the optimized entry point, removing the redirect.

---

## 12. Implications for Clang's Design

### What we can learn from MSVC

1. **PDB is the right place for metadata**: MSVC stores all deopt data in
   PDB named streams, not in the PE. This avoids bloating the executable
   and keeps the data accessible to the debugger without loading the exe.
   Our plan to store bitcode in PDB named streams aligns with this.

2. **The UUID approach is elegant for AOT**: For a full dual-binary scheme,
   per-function UUIDs that survive incremental rebuilds are valuable. If
   we ever implement MSVC-compatible AOT, we should consider adopting a
   similar scheme.

3. **The `S_ALTOBJNAME` record is useful**: It provides a direct link from
   the alt PDB back to the optimized obj. We could emit a similar record
   (or use `LF_BUILDINFO`) to link our alt/bitcode artifacts.

4. **`<DynamicDeoptimizeData>` is the key**: This master stream is what
   makes the whole system work at runtime. Any AOT-compatible mode we
   implement would need an equivalent.

### Where our approach is stronger

1. **No need for a non-runnable `.alt.exe`**: MSVC's `.alt.exe` is a 550KB
   file that can never run standalone. Our hybrid mode avoids this entirely
   -- we store compressed bitcode (~10-20% of obj size) and compile only
   what's needed.

2. **Standard relocations**: Our REL32-based approach works with any linker
   and any debugger that can apply COFF relocations. MSVC's `movabs $0`
   scheme requires a specialized debugger that understands the UUID fixup
   protocol.

3. **Per-function granularity at debug time**: Our hybrid mode extracts and
   compiles individual functions on demand. MSVC must load the entire
   `.alt.exe` even to deoptimize one function (though it only patches the
   relevant pages).
