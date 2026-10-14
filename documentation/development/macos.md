# macOS development setup

Mangrove includes a macOS build path using Homebrew's target-prefixed GNU
cross-toolchain. The path is intended to work on both Intel and Apple Silicon
macOS. Mangrove's kernel and userspace must be ELF, so the build uses
`x86_64-elf-*` tools rather than native Darwin GCC.

The forced `gmake -B -j4 binaries` build works on Apple Silicon macOS with
Homebrew GNU GCC 16.2.0. The normal host suite currently has an ARM limitation:
`tests/system_clock_test.c` includes the kernel's x86 `rdtsc` sampler, which
native ARM GCC cannot compile. A temporary host fixture can mock only that
hardware sampler to verify the remaining clock logic; such a run is an adapted
suite, not an unmodified `gmake test` pass. Target x86 sampling still builds.

Independent exFAT image checks require regular-file-capable tools. Apple's
`fsck_exfat` expects a device interface and is not a regular-file replacement
for `fsck.exfat`. Temporary builds of exfat-utils can check formatter output,
but their CLI differs from exfatprogs. Imported-name fixtures encode the fixed
2000-01-01 UTC date and discover the root end marker before adding entries;
neither depends on the formatter's initial label-entry placement. Report tool
adaptations, unavailable optional checks and checker failures explicitly. Native host fixtures
and target compilation do not validate hardware boot or QEMU guest behavior.
The zero-directory external-image encoder also uses valid 2000-01-01 UTC
timestamps; independent checks run before and after production mutations.
Intel macOS uses its platform-selected path but has no equivalent recorded
native verification here.

The UEFI loader remains a separate MinGW-w64 PE/COFF build. Host-side MGFS
tools and tests use Homebrew's versioned native GNU GCC.

## Install available tooling

With Homebrew, install the complete toolchain and image/QEMU dependencies with:

```sh
brew install \
  gcc x86_64-elf-gcc x86_64-elf-binutils mingw-w64 \
  make qemu dosfstools mtools gptfdisk python socat
```

Homebrew provides `x86_64-elf-gcc` and `x86_64-elf-binutils` as maintained
core formulae; no third-party tap or project bootstrap script is required.
The `x86_64-elf-gcc` formula supplies the ELF compiler and depends on the
matching cross binutils. The `x86_64-elf-binutils` formula supplies `ld`,
`ld.bfd`, `ar`, and `objcopy` with the `x86_64-elf-` prefix. The `mingw-w64`
formula supplies the UEFI commands, and `qemu` supplies the OVMF firmware.

Homebrew's native `gcc` formula installs a versioned command such as
`gcc-16`; the exact suffix depends on the installed formula version. The
Makefile finds that versioned compiler under `brew --prefix gcc` for host
tools and tests. It never falls back to Apple's `gcc`/`cc` aliases.

Verify the available commands and firmware location:

```sh
brew --prefix qemu
brew --prefix x86_64-elf-gcc
brew --prefix x86_64-elf-binutils
command -v gmake python3 qemu-system-x86_64 sgdisk mkfs.fat mmd mcopy
command -v x86_64-elf-gcc x86_64-elf-ld x86_64-elf-ar x86_64-elf-objcopy
command -v x86_64-w64-mingw32-gcc x86_64-w64-mingw32-ld
find "$(brew --prefix gcc)/bin" -maxdepth 1 -name 'gcc-[0-9]*' -print
find "$(brew --prefix qemu)/share/qemu" -maxdepth 1 \
  \( -name 'edk2-x86_64-code.fd' -o -name 'edk2-i386-vars.fd' \) -print
```

`gmake` is the Homebrew GNU Make command. The repository's Makefile invokes
`make`, so use `gmake` explicitly if the system `make` is not GNU Make.

## Build and image targets

Use Homebrew's GNU Make command if the system `make` is not GNU Make:

```sh
gmake -B binaries -j4
gmake fresh-image
gmake usb-image
gmake run
```

`gmake binaries` builds the UEFI loader, ELF kernel, libc, userspace, services,
and host MGFS tools. `gmake fresh-image` creates the persistent GPT
development disk, while `gmake usb-image` creates
`build/Mangrove/MangroveUSB.img`. The macOS image path uses `sgdisk`; Linux
uses `parted`.

Useful validation commands are:

```sh
gmake exfat-upcase
gmake test-time
gmake test-terminal
gmake -B nettest
gmake check-image-deps
gmake check-usb-deps
gmake check-qemu-deps
```

The Makefile selects the cross tools automatically. Manual `ELF_CC`, `ELF_LD`,
or related overrides should not be necessary when the Homebrew formulae are
installed.

## QEMU behavior

The Makefile's Darwin QEMU path uses OVMF from the Homebrew QEMU prefix. On
Intel Macs it selects HVF for the x86-64 guest. On Apple Silicon it selects
TCG for the x86-64 guest; no KVM path is available on macOS. These are the
configured platform paths; selecting an accelerator does not establish guest
correctness or hardware boot support. QEMU itself can be checked with:

```sh
qemu-system-x86_64 -accel help
```

The `run-usb` target is currently an alias of `run`, so it does not select
`build/Mangrove/MangroveUSB.img` automatically.

## Shell and script differences

The image scripts use POSIX shell, Python, `dd`, FAT tools, and GPT tooling.
macOS provides many basic shell utilities, but GNU Make is named `gmake`, and
the GPT/image path uses Homebrew `sgdisk`. Linux-only helpers such as KVM,
`parted`, and the Linux OVMF paths do not apply on macOS. No persistent macOS
permission or kernel-module setup is required for the documented tooling;
Hypervisor Framework availability is checked by the QEMU preflight when HVF
is selected.
