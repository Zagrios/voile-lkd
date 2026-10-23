# macOS 27 arm64 facts on the address space, page size and CPU features (groups A, B, E)

Document status: first draft of 2026-10-02, to be completed as measurements come in. Sections marked "to be completed" have no measurement yet.

## Context and machine

- Machine: Apple M5 Max, macOS 27.0.1 (build 26A434), kernel xnu-13432.1.9~1/RELEASE_ARM64_T6050, 36 GiB of RAM (hw.memsize 38654705664). MEASURED (sysctl).
- `hw.pagesize` = 16384 and `vm.pagesize` = 16384. MEASURED.
- Tools: Apple clang 21.0.0, macOS 27 SDK. MEASURED.
- The test programs are in `witnesses/darwin-memory/` (prefixes a_, b_, e_), the logs in `evidence/darwin-memory/`.

## Summary of findings

To be completed.

## A. Address space of an arm64 process

To be completed.

## B. Page size

To be completed.

## E. TSO and CPU features

To be completed.

## Implications for a 16 KiB-host FEXCore design

To be completed.

## Limits, untestable points and open questions

To be completed.

## Reproduction

To be completed.

<!-- working notes (removed at the end of the report) -->
- 2026-10-02 first findings: posix_spawnattr_set_4k_page_size_np exists in spawn.h (macOS 26.0), accepted for x86_64/Rosetta, EBADMACHO (88) for any arm64/arm64e. -pagezero_size < 4 GiB: ld accepts it, the kernel refuses (EBADMACHO) as long as pagezero + ASLR slide < 4 GiB. MH_PIE mandatory (EBADEXEC 85 otherwise). ASLR slide of the executable: 0..80 MiB in steps of 16 KiB.
- FEX: Source/Common/HostFeatures.cpp reads the ID registers with mrs (Linux emulates them); under macOS they will have to be built from sysctl.
- FEX/AGENTS.md and CLAUDE.md: "AI must not be used to generate code for contributions to this project."
