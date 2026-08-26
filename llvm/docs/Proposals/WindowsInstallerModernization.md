# [RFC] Modernize LLVM's Windows Release Build and Packaging

> **Status:** Draft for community discussion.
>
> **Core prototype:** [`feat/new_windows_powershell_installer`](https://github.com/aganea/llvm-project/tree/feat/new_windows_powershell_installer).

## Summary

LLVM's official Windows release process has two problems.
The build is driven by an old batch script that assumes a preconfigured machine; and the resulting installer contains many executable binaries that duplicate the same statically linked LLVM code.

This RFC proposes two workstreams:

1. replace the implementation of `build_llvm_release.bat` with a PowerShell 7 script while keeping the batch file as its entry point
2. enable the existing `llvm-driver` for official Windows packages, package its target once, and materialize the established binaries at installation time.

The second workstream also proposes returning the official installer from the current WiX MSI to an NSIS executable installer. That artifact-format change is an explicit part of the proposal.

The PowerShell work is independent.
The driver, alias-staging, and installer form one packaging design: enabling the driver alone does not stop CPack from packaging every alias binary, while omitting aliases without recreating them (after installation) leaves commands missing.

This proposal preserves existing command names and ordinary command-line behavior.
It does not claim that the installer format, filesystem identity, apparent directory size, or fallback process layout are unchanged.

## Current state, this proposal, and possible follow-ons

| Area | Current official release | This RFC | Later, not proposed here |
|---|---|---|---|
| Release automation | Batch script; prerequisites supplied by the build machine | PowerShell 7 implementation; batch bootstrap; pinned prerequisites; resumable PGO build | Hosted automation or reusable VM images could build on this work |
| Executable layout | Driver mode disabled; statically linked tools and aliases packaged separately | Existing driver mode enabled; 45 names backed by one `llvm.exe` | Fold additional eligible tools into `llvm.exe` after owner review |
| Alias packaging | CPack stages every executable binary | Package `llvm.exe` once plus a manifest; install hard link → forwarding executable → full copy | Apply the same scheme to additional aliases |
| Installer | WiX MSI, adopted after NSIS package crossed its size limit | Return to NSIS after reducing the staged payload | No further backend change proposed |
| Portable archive | Aliases are staged and their contents are repeated by the current tar command | Keep every name, but encode aliases as standard tar hard-link entries | Apply the same encoding to additional aliases |

The observed package outcomes and projections are:

| Packaging state | Evidence and scope | `bin/` on disk | Installed total | Installer |
|---|---|---:|---:|---:|
| LLVM 23.1.0 reference package | Measured historical release; separate executables; WiX | 2.79 GiB | 2.98 GiB | 780 MiB |
| **This RFC's prototype** | **Measured; existing 45 driver aliases; NSIS** | **1.94 GiB** | **2.02 GiB** | **458 MiB** |
| Packaging cleanup plus 11 additional tool folds (PoC in branch) | Measured follow-on; not proposed here | 1.32 GiB | 1.40 GiB | 332 MiB |
| Packaging cleanup plus the analyzed low- and bounded-work tiers | Modeled follow-on; not proposed here | ~0.91 GiB | ~1.00 GiB | ~237 MiB |
| Every remaining standalone tool folded into `llvm.exe` | Not estimated; several tools are structurally harder | — | — | — |

Installed files deduplicate hard links by file ID.

The LLVM 23.1.0 package and the prototype use different revisions and installer generators. They show the release-to-prototype outcome, not a controlled measurement of any one change. A causal comparison still needs the same revision, project set, flags, and generator with alias staging enabled and disabled.

The 332 MiB result is measured on the same x64 configuration as the RFC prototype.
The ~237 MiB result is an approximation and does not represent folding every remaining tool.

On the staged tree used for the 332 MiB NSIS result, storing hard links as tar references reduced the portable archive from 2.73 GiB to 525 MiB.

## Decisions requested

This RFC asks for consensus on:

1. Enable `LLVM_TOOL_LLVM_DRIVER_BUILD=ON` for official Windows packages.
2. Omit `llvm.exe` binary aliases from the official installer payload, while retaining them as tar hard-link entries in the portable archive.
3. Recreate the aliases at install-time using hard-link, falling back to a stub or full-copy on failure.
4. Restore the official installer to NSIS/EXE.
5. Adopt a PowerShell-based release workflow while preserving a batch entry point.

The first four decisions define the package layout.
The PowerShell workflow can be accepted independently.

This RFC does not propose folding the remaining standalone tools, removing or renaming tools, introducing a "fat `llvm.dll`", or operating the GCP example as an LLVM service.

## Motivation

The current `build_llvm_release.bat` assumes Visual Studio, CMake, Ninja, Python, WiX, and other tools are already installed. Their versions and the machine preparation live outside the script. The script also has limited parameterization and no supported way to resume after a late failure in the multi-hour PGO pipeline.

With current `LLVM_TOOL_LLVM_DRIVER_BUILD=OFF`, tools are built as separate statically linked executables even when most of their code is identical (measured 95% to 99% duplicated binary code accross the installation). The reference installation also contains roughly 820 MiB of byte-identical executable copies among names such as `clang`, `clang++`, `clang-cl`, the `lld` aliases, and the `llvm-ar` aliases.

LLVM already has a multicall `llvm-driver` that dispatches from `argv[0]`. The existing configuration covers 45 aliases in the package measured here and requires no change to the dispatch mechanism.

Hard-linking aliases in CPack's staging directory is insufficient. CPack embeds files by path rather than file identity, so each staged alias is still compressed as a separate package entry. The aliases must be excluded from the installer payload and materialized after extraction.

LLVM moved the official Windows installer from NSIS to WiX in [`#200734`](https://github.com/llvm/llvm-project/pull/200734) after the NSIS package crossed its 2 GiB limit. This proposal deliberately revisits that choice because the reduced payload packages successfully with NSIS and the prototype uses NSIS hooks to create and remove aliases. Keeping WiX is possible, but requires equivalent alias materialization and lifecycle testing. Its cabinet compression should also be measured with the same reduced payload and an explicit high-compression setting; the current WiX 780 MiB installer (currently MSZIP default) versus the NSIS isntaller 458 MiB (solid LZMA) figures do not isolate the installer format.

## Proposed changes

### Part A: Release-build automation

Replace the batch implementation with `build_llvm_release.ps1` and keep a small `build_llvm_release.bat` bootstrap. The wrapper locates PowerShell 7, installs it through `winget` when available, and forwards the command line.

The PowerShell script adds structured architecture and version parameters, opt-in prerequisite installation, the existing two-stage PGO build, and `-StartAt` restart points. A clean-machine path assumes Windows with `winget`, network access, and permission to install build tools.

A provided `llvm/utils/release/gcp-build/` prototype exercises that path on a fresh Windows Server VM. It is an example, not a proposed hosted service, and can be omitted without affecting the rest of the RFC.

PowerShell is proposed because this script primarily orchestrates Windows-native tools such as `vswhere`, `VsDevCmd.bat`, `winget`, Visual Studio, and NSIS. Python would be more familiar to much of the LLVM community and has clearer native-process argument handling. PowerShell 7 is not preinstalled either, so bootstrap availability is not decisive. If the expected long-term maintainers strongly prefer Python, ownership should outweigh the implementation convenience of PowerShell.

### Part B: Unified-driver packaging

The official release enables `LLVM_TOOL_LLVM_DRIVER_BUILD=ON`. Build-tree aliases remain available for tests and developer workflows.

A new `LLVM_INSTALL_DRIVER_ALIASES` option controls whether aliases are staged for installation. It defaults to `ON` so downstream installs retain their current behavior. The official installer sets it to `OFF` and carries one `llvm.exe` plus a generated `llvm-driver-tools.txt` manifest.

Portable archives leave the option `ON` because extracting an archive cannot run an install hook. The release script passes `-snh` to 7-Zip, so one name carries the contents and the other aliases are standard tar hard-link references. Extraction recreates every command name without storing or compressing `llvm.exe` once per path, provided that the extractor and destination filesystem support hard links.

The prototype also makes generic Windows CMake alias helpers prefer hard links over copies. That broader downstream change is not required by this RFC and could be separately gated or omitted before landing.

### Part C: Install-time alias materialization

The NSIS installer runs `llvm-link-mode.ps1 auto` after extraction. For each name in the manifest it attempts:

1. an NTFS hard link to `llvm.exe`;
2. a small `llvm-driver-stub.exe` forwarding executable stub; and
3. a full copy of `llvm.exe`.

A hard link adds no second copy of the contents, but shares file identity with `llvm.exe`. A 4096-bytes forwarding executable stub launches the adjacent `llvm.exe` with the alias name as `argv[0]` and adds an observable process hop. The full copy is used if neither of the first two forms can be materialized.

The per-alias fallback cannot recover if the script itself fails to run or aborts partway through the manifest. The final installer must check the script result and apply the failure behavior agreed in this RFC.

The installer invokes Windows PowerShell 5.1, not PowerShell 7. The install-time bootstrap script is deliberately kept 5.1-compatible.

### Part D: Installer backend

The prototype selects `CPACK_GENERATOR=NSIS` and produces `LLVM-*.exe`. Adoption therefore requires updating the release workflow, signing and upload steps, unattended-install documentation, and the transition from an existing WiX installation.

If MSI compatibility is required, the alternative is to retain WiX and implement the same manifest-driven creation and removal in a custom action. That path may recover much of the payload difference, but a standard MSI cabinet cannot use the solid LZMA configuration used by NSIS.

## Compatibility and project impact

For normal invocation, every tool name supplied by the current package remains present and dispatches to the corresponding entry point. Arguments, standard streams, and exit status are intended to remain unchanged.
The `build_llvm_release.bat` path also remains available.

The observable differences are:

- hard-linked names share file identity and contents;
- tools that sum file sizes by path report an inflated apparent directory size;
- stub mode adds a process visible to debuggers and process monitors; and
- MSI-to-EXE might affect unattended installation, repair, upgrades, enterprise deployment, and artifact-detection automation.

Under the proposed scope, downstream CMake installs retain their current alias behavior unless they explicitly set `LLVM_INSTALL_DRIVER_ALIASES=OFF`. Portable release archives continue to include every alias, represented as tar hard-link entries.

The proposal affects `llvm/utils/release`, the Windows release workflow, CMake install helpers, `llvm-driver` packaging, and the owners of the official Windows artifact.

## Risks and alternatives

- **Use Python:** this improves consistency and reviewer familiarity; the tradeoff is discussed in Part A.
- **Stage hard links:** this does not reduce the package because CPack embeds each path independently.
- **Shared failure domain:** a broken `llvm.exe` affects every tool it contains, so release testing must invoke every manifest alias.
- **Install-time failure:** the installer gains a PowerShell hook and must report partial failure rather than silently succeeding; whether it must roll back is an open question.
- **Filesystem reporting:** hard links make path-based size totals misleading and require release-note documentation.
- **Archive extraction:** tar hard-link entries require support from the extractor and destination filesystem; no copy fallback is encoded in the archive.
- **Retain WiX:** this preserves MSI deployment semantics, but requires alias materialization and an apples-to-apples test of high cabinet compression.
- **Use full copies:** this is simple but retains the duplication being addressed.
- **Use symbolic links:** the script supports them, but they are more visibly different and subject to filesystem and reparse-point policy.

## Prototype and landing

The core prototype is available at [`feat/new_windows_powershell_installer`](https://github.com/aganea/llvm-project/tree/feat/new_windows_powershell_installer). The branch also contains the follow-on packaging experiments shown in the early table. Those later experiments are not part of the requested design.

The measured RFC package is an x64, Release, two-stage PGO build with:

```text
LLVM_ENABLE_PROJECTS=clang;clang-tools-extra;lld;lldb;flang;mlir
LLVM_ENABLE_RUNTIMES=compiler-rt;openmp
LLVM_TOOL_LLVM_DRIVER_BUILD=ON
LLVM_INSTALL_DRIVER_ALIASES=OFF
CPACK_GENERATOR=NSIS
```

The pieces can land incrementally: CMake link support, alias staging and manifest generation, the forwarding executable stub and script, installer integration, and the release workflow. Until the release flips its configuration, rollback is simply to retain staged aliases and WiX.

## Open questions

1. Is PowerShell 7 acceptable to the expected maintainers, or should the release script be Python?
2. How should an NSIS installer handle an existing WiX/MSI installation: refuse, remove it, or allow a documented side-by-side transition? (also applies to current NSIS to Wix transition for 23.1.0 release)
3. Should `auto` try a symbolic link before the forwarding executable?
4. Should any alias-materialization failure abort and roll back installation?

## Future work

After this RFC's driver configuration, 50 executables in the LLVM installation remain standalone. The early table covers a measured 11-tool set follow-on and the analyzed low- and bounded-work tiers, not every remaining tool. Additional folds into llvm-driver should be proposed separately with the relevant tool owners.
