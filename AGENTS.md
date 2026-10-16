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
- Document current implemented behavior, public and internal technical interfaces, constraints, guarantees, limitations, and actual developer procedures. Update affected technical documentation in the same implementation change as the behavior.
- Do not commit future plans, internal stage/slice or milestone tracking, Codex prompts, task checklists, speculative designs, or development-history narratives as technical documentation. Do not describe planned features as implemented. Document unsupported behavior where it clarifies an existing interface. Create a public roadmap or future-design document only when explicitly requested, and keep it separate from current technical documentation.
- Keep current-state documentation accurate and organized without duplicating information across files.
- Reuse existing qualification evidence when its source revision, binaries, configuration, assumptions, and workload remain applicable. Distinguish reused evidence from newly run checks and avoid redundant expensive campaigns.
- Qualification claims require credible execution evidence: actual guest process completion for guest tests, independent verification where applicable, and accurate PASS/FAIL/SKIPPED/INCOMPLETE outcomes. A printed success message alone is insufficient.
- Do not perform real-device flashing, partitioning, or hardware experiments unless explicitly requested.

## Git and communication

- Use conventional commit subjects in the form `subsystem: short imperative description`, with a lowercase prefix, no trailing period, and preferably fewer than 72 characters. Add a concise body only when it provides useful technical context. Avoid stage/slice labels and development-progress language.
- After authorized work is complete and its applicable review and verification pass, create a GPG-signed commit. Verify the signature and confirm the working tree is clean. Never push unless explicitly requested. Do not rewrite history or discard unrelated work.
- Keep the final report focused on behavior, affected files, verification, and material limitations or decisions. Avoid unrelated changes, redundant tool output, and excessive progress narration.
- Follow explicit scope and review gates. Task-specific instructions determine the requested deliverable, exact checks, limits, and whether to stop for review; do not add those temporary details to this file.
- Make routine engineering decisions autonomously and implement root-cause corrections, including justified cross-subsystem changes. Preserve explicit review gates; stop for a genuine unresolved product decision or an unsafe or unauthorized action.
