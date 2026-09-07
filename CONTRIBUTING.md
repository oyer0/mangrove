# Contributing to Mangrove

Mangrove is a small operating-system project. Contributions should be easy to
review, build, test, and understand.

## Before submitting

Run the checks relevant to the change. For general changes, start with:

```sh
make -B binaries -j4
git diff --check
```

Run the applicable host tests and guest/QEMU validation for behavior that
crosses the kernel, image, or userspace boundary. Storage and image changes
should also use disposable media or images and verify the resulting state.

Do not treat a successful host build as sufficient validation for a runtime
change.

## Changes and commits

Keep changes focused and avoid unrelated refactoring. Explain important
behavioral, ABI, image-layout, and compatibility decisions in the commit
message or accompanying documentation. Keep intermediate commits buildable
where practical, and do not commit build outputs or local test artifacts.

Generated files must be updated through their documented generator. Do not
edit generated data manually.

## Provenance and notices

Preserve existing notices and provenance records for third-party data and
assets. In particular, do not apply Mangrove's license to the hardware ID
databases, the UNSCII font data, or specification-derived exFAT data. Keep
their separate licensing and generation information with those assets.

## AI-assisted work

AI-assisted contributions are welcome. Contributors remain responsible for
understanding, reviewing, testing, and validating everything they submit,
including generated or AI-assisted code. Contributions must be submitted
under the project's GPL-3.0-or-later license.

There is no separate contribution agreement or CLA at present.
