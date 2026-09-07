# Contributing to Mangrove

Mangrove is still a small, experimental project, so contributions should stay simple and easy to review.

Before submitting a change, make sure it builds and test the parts you touched. For most changes:

```sh
make -B binaries -j4
git diff --check
```

If the change affects runtime behavior, also test it in Mangrove rather than relying only on a successful host build.

Keep commits focused and avoid unrelated cleanup. Generated files should be updated through their generator instead of edited by hand.

Please preserve the existing license and provenance notices for third-party data and assets.

AI-assisted contributions are fine. Just make sure you understand, review, and test what you submit.

By contributing, you agree to license your contribution under GPL-3.0-or-later.
