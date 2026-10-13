# Mangrove — Agent Guidelines

## Project context

Mangrove is a standalone x86-64 operating system with its own UEFI bootloader, kernel, filesystems, native userspace, and tools. The repository and current `documentation/` are the source of truth. Read the relevant code and documentation before changing behavior; avoid repeating exploration when recent evidence already answers the question.

## Engineering principles

- Prioritize correctness, reliability, security, performance, resource efficiency, and maintainability. Investigate and fix root causes; do not conceal defects with shortcuts or silent fallbacks.
- For risky changes, first review the architecture, invariants, and failure states. If a fundamental safety or correctness blocker remains, stop before unsafe implementation and report the issue and viable options.
- Preserve established interfaces and on-disk formats, ownership and lifetime rules, lock ordering, concurrency behavior, and failure/recovery guarantees. Treat uncertainty explicitly.
- Distinguish runtime failure containment from crash consistency and verified durability. Claim only guarantees supported by implementation and evidence.
- Prefer focused, reviewable changes and existing mechanisms. Add abstractions or refactoring only when they solve a demonstrated need. Keep unrelated cleanup out of functional changes.
- Track temporary limits and deferred prerequisites in current technical documentation so they can be revisited; do not turn implementation restrictions into assumed permanent design.

## Tests and documentation

- Select verification for the change's risk and scope. Prefer regressions that execute production functions and cover adverse conditions, including fault injection where persistent state or concurrency is involved. Use sanitizers, builds, interoperability checks, and guest tests when relevant and available.
- Follow `documentation/development/` and the Makefile. On Linux use `make`; on macOS use Homebrew GNU `gmake`. Preserve the established GCC and GNU binutils toolchain unless the task requires otherwise.
- Run the focused checks requested for the task, plus `make test` and `make -B -j4 binaries` for implementation changes when applicable. Run `git diff --check` and inspect the final diff. Report exactly what ran, what failed or was unavailable, and which dependencies were mocked. Never imply unperformed verification passed.
- Keep documentation accurate to current behavior and public contracts. Do not add internal stage, slice, agent, or Codex history to technical documentation.
- Do not perform real-device flashing, partitioning, or hardware experiments unless explicitly requested.

## Git and communication

- After authorized work is complete and its applicable review and verification pass, create a GPG-signed commit. Verify the signature and confirm the working tree is clean. Never push unless explicitly requested. Do not rewrite history or discard unrelated work.
- Keep the final report focused on behavior, affected files, verification, and material limitations or decisions. Avoid unrelated changes, redundant tool output, and excessive progress narration.
- Follow explicit scope and review gates. Task-specific instructions determine the requested deliverable, exact checks, limits, and whether to stop for review; do not add those temporary details to this file.
