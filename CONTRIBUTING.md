# Contributing to Mangrove

Mangrove is a small, experimental operating-system project. Contributions are
welcome; focused changes that are easy to understand and review are especially
helpful. Start with the [documentation index](documentation/README.md) and the
subsystem guidance relevant to your change.

## Build and test

For a normal target build and whitespace check:

```sh
make -j4 binaries
git diff --check
```

Run focused tests for the code you changed. For example, terminal changes can
use `make test-terminal`, while editor and synchronization changes can use
`make test-reed` and `make test-sync`. See the
[focused testing guide](documentation/development/tests.md) for available
targets and their coverage.

When a change depends on booted-kernel behavior, a driver, or interaction among
native applications, run an appropriate guest test where practical. For
example, `make test-guest-reed` exercises a native Reed workflow. Guest tests
can take substantially longer than host tests. Linux guest tests use KVM by
default; select TCG explicitly with the target's `*_ACCEL=tcg` option when
needed. Report which checks you ran and disclose tests that were skipped,
unavailable, or incomplete. Do not describe unperformed checks as passing.

Keep changes narrowly scoped and consistent with Mangrove's existing
interfaces, ownership rules, and subsystem architecture. Update generated
files through their generator rather than editing generated output by hand.

## Commit messages

Use [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/)
for commit subjects, in the form `<type>[optional scope][!]: <description>`.
Use a concise, imperative description. Examples:

```text
feat(shoot): add command history
fix(reed): retain edits after a failed save
test(terminal): cover scroll boundaries
docs(memory): clarify slab backing ownership
```

## License and contribution provenance

Mangrove source is licensed under GPL-3.0-or-later. By contributing, you agree
to license your contribution under that license. Preserve existing license and
provenance notices, including those for third-party data and assets.

AI-assisted contributions are welcome. Understand, review, and test the
contribution before submitting it, and disclose AI assistance where required
by the contribution context.
