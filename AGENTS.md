# Mangrove — Agent Guidelines

## Project context

Mangrove is a standalone x86-64 operating system with its own UEFI bootloader, kernel, filesystem, native userspace, and tools. It is not a Linux distribution. Read the relevant source and `documentation/` before proposing or implementing changes; the repository is the source of truth.

## Working approach

- Work in small, reviewable increments. Follow the requested scope and stop at explicit review gates.
- Investigate existing behavior and explain the cause of a problem before changing code.
- Prefer the simplest change consistent with existing architecture, naming, and conventions. Reuse existing facilities instead of creating parallel mechanisms.
- Do not add placeholder implementations, empty stubs, silent fallbacks, or speculative abstractions to make a task appear complete.
- Keep changes focused. Do not combine unrelated cleanup, formatting, or refactoring with a functional fix.
- When requirements or correctness guarantees are unclear, identify the trade-off and ask before making an architectural decision.

## System boundaries and correctness

- Respect the distinction between UEFI bootloader, kernel, filesystem, VFS, libc, native userspace, and host-side development tools.
- Preserve ABI and on-disk format compatibility unless a breaking change is explicitly approved.
- For concurrency, storage, and memory changes, reason about ownership, reference lifetimes, lock ordering, failure paths, and teardown.
- Distinguish confirmed success, definite failure, and uncertain outcomes where persistent state or device I/O is involved.
- Do not claim atomicity, crash consistency, durability, or hardware support without evidence that the implementation and tests provide it.
- Avoid introducing dependencies on the host OS into Mangrove target binaries.

## Builds and tests

- Use the build instructions in `documentation/development/` and the targets defined in `Makefile`.
- On Linux use `make`; on macOS use Homebrew GNU `gmake`. Follow the existing GCC and GNU binutils toolchain selection; do not introduce Clang/LLVM without a project requirement.
- Run focused regressions for the affected behavior. For implementation changes, also run `make test` and `make -B -j4 binaries` (substitute `gmake` on macOS), unless the task explicitly narrows verification.
- Run `git diff --check` and inspect the final diff for unintended changes.
- Prefer tests that exercise production functions and failure paths; clearly identify mocked dependencies, simulations, and untested integration behavior.
- Report tests that could not run or required environment workarounds. Never describe skipped checks as passing.
- Do not perform real-device flashing, partitioning, or hardware boot experiments unless explicitly requested.

## Git and reporting

- Do not commit, push, amend, rebase, or rewrite history unless explicitly authorized.
- Do not overwrite unrelated work or discard uncommitted changes.
- At completion, summarize the behavior changed, exact files affected, checks run and their outcomes, remaining limitations, and any follow-up decisions needed.
- For investigation-only tasks, do not modify repository files.
