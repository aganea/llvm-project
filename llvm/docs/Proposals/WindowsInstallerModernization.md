# [RFC] Modernize LLVM Windows Release Build and Installer

## TL;DR

- **Problem.** The Windows release ships 77 standalone `.exe` files that each statically link most of LLVM — ~820 MB of them are byte-identical copies of one another. An LLVM 23 install is **2.98 GB**. The release script is an unmaintainable `.bat` file that only runs on a hand-maintained build machine.
- **Proposal.** Three independent changes: (A) rewrite the build script in PowerShell 7 so it self-bootstraps on a stock Windows box; (B) turn on the existing `llvm-driver` so 45 tools become one multicall `llvm.exe`; (C) materialize the tool aliases as NTFS hard links at install time, with a ~4 KB stub fallback for non-NTFS.
- **Measured result.** `bin/` **2.79 GB → 1.94 GB**, total install **2.98 GB → 2.02 GB**, installer **458 MB** (against 434 MB for the last NSIS release and 780 MB for today's WIX MSI).
- **No user-visible change.** Every existing tool name keeps working; hard links are indistinguishable from regular files.
- **Status.** Fully implemented on a branch, splittable into small independent PRs.
- **Later, not proposed here.** Folding the remaining 50 standalone tools into `llvm.exe` would take the install to ~1,019 MB and the installer to ~228 MB.

## Summary

This RFC proposes three related changes to the LLVM Windows release process:

1. **Replace the batch build script** with a PowerShell 7 script for maintainability and clean-machine reproducibility.
2. **Enable the unified `llvm-driver` build** for Windows releases, producing a single multicall `llvm.exe`.
3. **Use NTFS hard links** for tool aliases, materialized at install time, with a minimal stub fallback for non-NTFS filesystems.

Together these take the installed toolchain from **2.98 GB to 2.02 GB** and the installer to **458 MB**, while preserving full backward compatibility.

There is considerably more headroom beyond that. The 50 executables that remain standalone even after this change are **73% of the installer payload**, and folding the ones that need no or bounded work into `llvm.exe` would reach **1,019 MB installed, against 3,052 MB (2.98 GB) today**, with a **228 MB installer, against 780 MB today**. That work is deliberately *not* part of this proposal; see [Future Work](#future-work-folding-more-tools-into-llvmexe).

## The Problem

**The binaries duplicate each other.** When `LLVM_TOOL_LLVM_DRIVER_BUILD` is `OFF` (today's default), every tool is a standalone statically-linked binary, and they share the bulk of LLVM's library code. The duplication is literal, not just conceptual: an LLVM 23 install carries roughly **820 MB of byte-identical `.exe` copies** — `clang`/`clang++`/`clang-cl`/`clang-cpp` at 104.5 MB each, the five `lld` names at 72.0 MB each, four `llvm-ar` names, and so on. A measured LLVM 23 x64 installation occupies **2.98 GB**, of which **2.79 GB is `bin/`**.

**The build script is hard to maintain.** `build_llvm_release.bat` has no structured error handling, limited parameterization, and no way to resume a failed multi-hour PGO build.

**Producing a release depends on one machine.** It requires Visual Studio, CMake, Ninja, Python, NSIS and more, already installed, in whatever versions happen to be there. Nothing records which. That makes the process hard to reproduce, hard to hand off, and unusable on ephemeral infrastructure such as a fresh CI runner or cloud VM.

## Proposed Changes

### Part A: Build script modernization

Replace the batch script with `build_llvm_release.ps1` (PowerShell 7), keeping a 25-line `.bat` bootstrap wrapper that checks for `pwsh`, installs it via `winget` if missing, and forwards all arguments — so existing CI entry points keep working.

- **Structured parameters**: `-x64`, `-arm64`, `-x86`, `-Version`, `-InstallPrerequisites`, `-StartAt <step>`.
- **Prerequisite auto-install**: Visual Studio Build Tools (with ATL, MFC, DIA SDK), CMake, Ninja, Python, 7-Zip, NSIS, Git, SWIG, Perl — all via `winget` with pinned versions.
- **Multi-architecture** x86/x64/arm64 from one invocation, with the two-stage PGO workflow preserved.
- **Resumable**: `-StartAt` restarts from any step instead of re-running the whole pipeline after a late failure.

`-InstallPrerequisites` is the piece that matters most: running `build_llvm_release.ps1 -InstallPrerequisites -x64` on a stock Windows install is sufficient to produce a release. `llvm/utils/release/gcp-build/` is a Terraform template that exercises exactly this path — provision a stock Windows Server VM, clone, build, upload, stop — as a runnable example rather than a maintained CI system.

The choice of PowerShell over Python is a genuine judgment call; see [Appendix A](#appendix-a-why-powershell-not-python). Other smaller improvements over the `.bat` script are listed in [Appendix E](#appendix-e-smaller-improvements-over-the-bat-script).

### Part B: Unified driver build

Enable `LLVM_TOOL_LLVM_DRIVER_BUILD=ON`, producing a single `llvm.exe` that dispatches on `argv[0]` using the existing `llvm-driver` infrastructure. That mechanism has been available since LLVM 16 and is already used on other platforms; **no changes to it are required**.

The build always creates per-tool aliases in the build tree for `check-*` targets. Whether they are *also* staged for packaging is controlled by a new `LLVM_INSTALL_DRIVER_ALIASES` option (default `ON`). The release build sets it to **`OFF`**, so no alias is staged and CPack compresses `llvm.exe` once instead of 45 near-duplicate copies. The package instead carries `llvm.exe`, a generated manifest `llvm-driver-tools.txt` listing every alias name, and the script that materializes them.

This split matters because hard-linking inside the staging directory would not have helped on its own: CPack embeds each staged file *by path, not by inode*, so two directory entries sharing disk blocks still become two independent compressed blobs. Excluding them is what actually shrinks the package.

> Note: the hard-link mode and copy fallback in `install_symlink`/`llvm_get_link_or_copy` are a supplement this proposal carries, not pre-existing upstream behavior — upstream only ever produces a symlink or a copy. That code needs to land as part of this work.

### Part C: NSIS installer — hard links with stub fallback

The installer ships `llvm-link-mode.ps1`, which reads the manifest and, per alias, tries in order:

1. an **NTFS hard link** to `llvm.exe` — zero extra disk, no elevation needed, indistinguishable from a real file to every Windows API;
2. a **~4 KB stub** (`llvm-driver-stub.exe`) that re-launches `llvm.exe` under the alias name;
3. a **full copy**, so a tool is never missing.

`CPACK_NSIS_EXTRA_INSTALL_COMMANDS` runs `llvm-link-mode.ps1 auto` after install and the uninstall hook removes every alias in the manifest. The installer invokes **`powershell.exe`, not `pwsh.exe`** — Windows PowerShell 5.1 ships with every supported Windows version, so the script is deliberately kept 5.1-compatible. (The *build* script does require PowerShell 7, but that runs on a machine we control.)

The script is also usable standalone, to switch an existing install between hard links, symlinks, stubs and copies. See [Appendix B](#appendix-b-hard-links-when-they-fail-and-the-stub-design) for when hard links fail and how the stub works, and [Appendix C](#appendix-c-why-the-installer-always-runs-elevated) for why elevation is unavoidable and what it does and doesn't buy.

## Savings

Baseline: LLVM 23.0 x64, default install. Proposal: a driver-enabled x64 build of `main` with `LLVM_ENABLE_PROJECTS=clang;clang-tools-extra;lld;lldb;flang;mlir`, measured the same way.

| | `bin/` | Total install | Installer |
|---|---:|---:|---:|
| **Before** (LLVM 23, separate binaries) | 2.79 GB | 2.98 GB | 780 MB (WIX) |
| **After** (this proposal) | **1.94 GB** | **2.02 GB** | **458 MB** (NSIS) |
| Reduction | **~30%** | **~32%** | — |

Stub mode is indistinguishable at this scale: 45 stubs of 4,096 bytes add 0.18 MB.

**On the installer comparison.** The like-for-like baseline is LLVM 22.1.8 at 434 MB, since that was also NSIS; LLVM 23 switched to WIX, which conflates a generator change with two releases of growth. Against 22.1.8, this proposal lands at 458 MB — close to it despite additionally shipping Flang, MLIR and LLDB, none of which the driver deduplicates. Excluding the aliases from the package is what keeps it there.

Packaging time improves too: `makensis` compresses on a single thread, with no multithreading option at any level, so feeding it less data is the only lever available.

**Why ~30% and not more.** `bin/` holds more than driver tools. Of 96 executables in a driver-enabled install, only 45 are aliases of `llvm.exe`; those collapse from 45 × 122 MB to a single 122 MB binary. The other 50 — `clangd`, `clang-tidy`, `flang`, `bbc`, `clang-repl`, the LLDB tools — remain standalone, and Flang/MLIR/LLDB alone account for roughly 600 MB of what's left.

One practical caveat that will otherwise be mistaken for a regression: **Explorer and any tool that sums file sizes without deduplicating by file ID will report `bin/` at 7.29 GB, not 1.94 GB.** See [Appendix D](#appendix-d-measuring-the-real-size-of-bin).

## Future Work: Folding More Tools into `llvm.exe`

The 50 standalone executables left in `bin/` are **73% of the installer payload** — more than `llvm.exe` and all six DLLs combined. Folding them into the driver is the natural continuation of Part B. Each has been measured and triaged; the summary is:

- **13 tools are essentially free.** They register no file-scope `cl::opt` and link a strict subset of `llvm.exe`'s libraries, so folding them adds no new code. Work is renaming `main` and adding `GENERATE_DRIVER`, which `add_llvm_tool` and `add_clang_tool` already accept.
- **17 need a bounded option migration.** The one real obstacle across the whole set is that global `cl::opt` names must be unique per process — a duplicate is a `report_fatal_error` at static init. Notably, none of the tools already in `llvm.exe` registers a single global `cl::opt`, so this is a threshold the driver has not yet had to cross. `cl::sub` is the established in-tree escape (`llvm-pdbutil` already scopes 140 options that way).
- **7 are structurally harder** — `clang-repl` (JIT'd code binds against the executable's export table), `clangd` (51 global options), and the flang/MLIR group, for which `add_flang_tool`/`add_lldb_tool` have no `GENERATE_DRIVER` path at all.

**Roughly 47 MB of installer — 10% — needs no driver work whatsoever, only packaging changes.** `flang-new.exe` is a byte-identical 136 MB copy of `flang.exe` that NSIS embeds twice, because it is an alias of a non-driver tool and `File /r` is hard-link blind. And three tools ship by accident: `bbc` (90.6 MB) is a lit-test lowering driver with exactly one non-test reference in the tree, alongside `f18-parse-demo` and `yaml2macho-core`.

Taken together, the packaging fixes plus the free and bounded tiers would bring the installer down to **as little as 228 MB** and the install to **1,019 MB**. None of this is proposed here; it is noted so reviewers can see that the driver work has a much longer runway than the ~30% figure suggests.

## Drawbacks, Risks and Alternatives

**Everything shares one binary.** With the driver, a bad `llvm.exe` breaks every tool at once rather than one. That is inherent to a multicall design and is already accepted on other platforms.

**Hard links confuse disk-usage reporting.** Covered above and in Appendix D; the mitigation is documentation, not code.

**Hard links do not work everywhere.** FAT32/exFAT and some network shares cannot create them, and they cannot span volumes. The stub fallback handles this at ~1–5 ms of extra process-creation cost, only on those filesystems.

**An install-time script is a new moving part.** The installer now runs `powershell.exe` after copying files. If that step fails, aliases are missing — so the fallback chain deliberately ends in a full copy rather than an error. It is worth noting this is the main behavioral difference from a conventional installer.

**Alternatives considered:**

- **Symlinks instead of hard links.** They work across volumes, and the installer is always elevated so the privilege requirement is satisfied. `llvm-link-mode.ps1` already implements a `symlink` mode; `auto` simply doesn't try it. Adding it ahead of the stub would cover the cross-volume case. Reasonable follow-up, not required.
- **Ship full copies (status quo).** Simple, but that is the 2.79 GB being fixed.
- **Stage hard links and let CPack handle it.** Doesn't work — CPack embeds by path, not inode (Part B).
- **A single fat `llvm.dll` with thin stubs**, with the existing DLLs becoming `.def` forwarders. This is a substantially bigger win — the six DLLs are 331 MB and 96–99% duplicate against `llvm.exe` — and the export surface was measured at ~1,765 symbols, well under the 65,535 cap, so the usual Windows objection doesn't apply. But it is a materially different proposal and depends on the driver work landing first.
- **Keep the build script in Python** for consistency with the rest of `llvm/utils/release/`. See [Appendix A](#appendix-a-why-powershell-not-python).

## Compatibility

- **No behavioral change for end users.** All existing tool names (`clang.exe`, `lld-link.exe`, …) work identically.
- **The `.bat` wrapper preserves the existing entry point** for CI and automation.
- **Hard links are transparent** to all Windows APIs, `cmd.exe`, PowerShell and build systems — programs cannot distinguish one from an original file.
- **The stub's ~1–5 ms overhead** applies only on non-NTFS installs and is negligible against tool startup.

## Implementation Status

Fully implemented on the [`feat/new_windows_powershell_installer`](https://github.com/aganea/llvm-project/tree/feat/new_windows_powershell_installer) branch: the PowerShell build script, the `LLVM_INSTALL_DRIVER_ALIASES` CMake option, the alias manifest and `llvm-link-mode.ps1`, the NSIS wiring in `llvm/CMakeLists.txt`, the `llvm-driver-stub` executable, and the GCP Terraform template.

Rather than one large patch, this can land as a series of independently-reviewable PRs: the hard-link support in `install_symlink` first, then the driver-alias CMake plumbing, then `llvm-link-mode.ps1`, then the NSIS wiring, then the build script, with the GCP template last as the least essential piece.

---

# Appendix

## Appendix A: Why PowerShell, not Python?

Most of `llvm/utils/release/` is Python, so this deserves an answer.

**Size is a wash.** A Python port would land within ~10–15% of the current ~1,550 lines. Python drops the ~50-line PowerShell-5-vs-7 self-relaunch block; against that, PowerShell's comment-based help doubles as `argparse` setup *and* documentation for free, and the ~40 native-command call sites (`cmake`, `ninja`, `winget`, `7z`, `vswhere`) are about as terse either way.

**Where PowerShell wins.** The script's entire job is orchestrating Windows-native tooling — `vswhere.exe`, `VsDevCmd.bat`, `winget`, NSIS, the registry-adjacent parts of a Visual Studio install. Sourcing a `.bat` file's environment is a one-liner; `Add-Type` gives inline C# for the one bit of raw Win32 (`GetConsoleMode`/`SetConsoleMode`) with less ceremony than `ctypes`. Decisively, PowerShell 7 self-bootstraps from the Windows PowerShell 5.1 present on every Windows box, whereas **Python is not preinstalled anywhere** — a Python version's first prerequisite would be a Python interpreter installed by something else, which cuts directly against the clean-machine goal.

**Where Python would win.** Consistency with the rest of the tree, easier review for Python-literate contributors, unambiguous `subprocess.run([...])` argument lists that sidestep PowerShell's native-argument-quoting quirks, and a testing story (`pytest`) consistent with the tree — nothing else in LLVM uses Pester.

On balance this proposal keeps PowerShell, but it is a judgment call and reviewers who weigh it differently should say so.

## Appendix B: Hard links, when they fail, and the stub design

Hard links are directory entries pointing at the same file data, so 45 of them to `llvm.exe` consume the space of one file. They need no elevation on NTFS and are indistinguishable from regular files to every Windows API. They do require NTFS and cannot span volumes:

| Scenario | Likelihood | Fallback |
|---|---|---|
| FAT32/exFAT (USB drives, SD cards) | Rare | Stub |
| Network share (SMB) | Uncommon, server-dependent | Stub |
| Standard NTFS install (C:) | **Vast majority** | Hard link |

**The stub** (`llvm/tools/llvm-driver-stub/llvm-driver-stub.cpp`, gated on `LLVM_TOOL_LLVM_DRIVER_BUILD`) is ~3–4 KB with no CRT and no LLVM dependencies — built with `/NODEFAULTLIB` and a custom entry point, importing only from `kernel32.dll`. Invoked as e.g. `clang-cl.exe`, it finds its own filename, locates `llvm.exe` alongside it, and spawns it via `CreateProcessW` passing its own name as `argv[0]`. It handles pipes, redirection, Ctrl+C (shared console) and Unicode paths, at ~1–5 ms of extra process creation.

The stub earns its keep beyond install time: an unelevated user can run `llvm-link-mode.ps1 stub` against an existing install later — for instance to check a toolchain into version control, where ~3 KB stubs per tool are practical and 45 copies of a 122 MB binary are not.

## Appendix C: Why the installer always runs elevated

CPack's NSIS template hardcodes `RequestExecutionLevel admin` with no `CPACK_*` override; the only way around it is shipping a replacement template. That is less an oversight than a coupling — the template writes to `HKLM`, modifies the system `PATH`, and uses `SetShellVarContext all`. Those are machine-wide operations, and installing into `$PROGRAMFILES64` needs elevation regardless.

Given the install hook therefore always runs elevated:

- **It doesn't change the primary path.** Hard links never needed elevation, so admin rights don't make the common case any more likely to succeed.
- **It does make symlinks viable.** `SeCreateSymbolicLinkPrivilege` is available, and symlinks work across volumes where hard links don't — see Alternatives.
- **It doesn't remove the need for the stub.** FAT32/exFAT support neither hard links nor reparse points, so no privilege helps there.

## Appendix D: Measuring the real size of `bin/`

**Windows Explorer, `dir /s`, and `Get-ChildItem -Recurse | Measure-Object Length -Sum` all report `bin/` at its apparent size — 7.29 GB across 114 files — not the 1.94 GB it actually occupies across 69 unique files.** Each of the 45 aliases is a separate directory entry reporting `llvm.exe`'s full size, though all 46 names share one extent on disk.

Anyone eyeballing the folder after install can easily read that inflated total as evidence the installer *grew*. To see the real number, use something that deduplicates by file ID (WizTree, or `fsutil hardlink list <file>` to confirm a given name is a link rather than a copy).

## Appendix E: Smaller improvements over the `.bat` script

- **`-StartAt <step>`**: resume from any step (`libxml2`, `stage0`, `pgo`, `stage1`, `package`, `tarball`); `-StartAt ""` lists them. The `.bat` script had no resume at all — a late failure meant starting over or hand-editing the script.
- **`LLVM_NINJA_OVERRIDE`**: environment variable that swaps in a different `ninja` and/or prepends flags to every invocation, for plugging in an accelerated ninja without a fork.
- **CMake flags via an initial-cache file** (`-C`) rather than literal command-line `-D` flags. Stage1 alone accumulates dozens; passing them literally risks Windows' command-line length limit.
- **Build-directory delete confirmation**: prompts `[y/N]` instead of refusing and requiring a manual `rmdir`.
- **Console mode save/restore**: child tools can leave virtual-terminal-processing altered on exit, breaking arrow keys in the parent shell. The script saves and restores it.
- **Idempotent Visual Studio servicing**: uses `vs_installer.exe modify` to add missing components (ATL, MFC, DIA SDK) to an existing install rather than demanding a reinstall.
- **Centralized retry**: one `Invoke-WithRetry` helper reporting `[attempt N/3]`, replacing `cmd || cmd || cmd || exit /b 1` copy-pasted at ~15 call sites.
- **Comment-based help**: gives `-Help` and `Get-Help -Detailed` for free instead of a hand-written `:usage` label.
