# Build and image pipeline

Mangrove is built in separate freestanding and host-toolchain stages. The
top-level `Makefile` builds the UEFI loader, Pith, libc, system services,
userspace commands, and host-side MGFS tools. Image scripts then populate the
EFI and MGFS partitions from those outputs.

## Toolchain split

The UEFI loader uses MinGW-w64 on both supported host platforms:
`x86_64-w64-mingw32-gcc` and `x86_64-w64-mingw32-ld`, producing a PE32+/COFF
EFI application.

Mangrove's ELF outputs use the host-appropriate GNU cross tools:

- Linux: `gcc`, `ld.bfd`, `ar`, and `objcopy`.
- macOS: `x86_64-elf-gcc`, `x86_64-elf-ld`, `x86_64-elf-ar`, and
  `x86_64-elf-objcopy`.

In both cases Pith, libc, and userspace are compiled and linked as ELF64.
Host MGFS tools and host tests use GCC; on macOS the Makefile selects the
versioned native GNU compiler installed by Homebrew.

The normal build has no Clang, LLVM, `ld.lld`, `lld-link`, `llvm-ar`, or
`llvm-objcopy` dependency. The MinGW-w64 UEFI commands are checked when the
Makefile is read and the build stops with an installation hint if they are
missing.

## Main targets

Run these from the repository root:

| Command | Result |
| --- | --- |
| `make -B binaries -j4` | Rebuild the loader, kernel, libc, userspace, services, and host tools. |
| `make fresh-image` | Recreate the persistent development image from a fresh root. |
| `make usb-image` | Build a fresh GPT USB image at `build/Mangrove/MangroveUSB.img`. |
| `make run` | Update and boot the persistent development image in QEMU. |
| `make exfat-upcase` | Regenerate and verify the embedded exFAT up-case data. |
| `make test-time` | Run the current host-side timekeeping tests. |
| `make test-terminal` | Run the current host-side terminal UTF-8 tests. |
| `make -B nettest` | Build the guest network test program. |
| `make mkmgfs` | Build the MGFS image-creation tool. |
| `make mgfsck` | Build the MGFS checker. |
| `make clean` | Remove disposable `build/` output while preserving `.mangrove/`. |

Use the available validation targets above and the guest/runtime network
commands documented by the current image.

## Generated outputs

The disposable `build/` tree contains per-program output directories and
shared build products. Important outputs include:

```text
build/EFI/BOOT/BOOTX64.EFI     UEFI loader
build/Mangrove/pith.elf        Pith kernel
build/Mangrove/Boot.img        ESP image
build/Mangrove/MangroveUSB.img fresh GPT USB image
build/mkmgfs                    host MGFS formatter
build/mgfsck                    host MGFS checker
```

The persistent development state is kept separately in:

```text
.mangrove/MangroveDev.img      GPT development disk used by QEMU
.mangrove/MangroveDevRoot.img  staged MGFS root payload
```

`make clean` removes `build/` but intentionally preserves `.mangrove/`.

## Image construction

`make fresh-image` updates the persistent development disk through
`scripts/update_dev_image.sh`. The script creates or updates a GPT disk with
an ESP and an MGFS root partition. It checks partition bounds and refuses to
update an image that is in use.

`make usb-image` runs the fresh-image path for a new output image and writes
the ESP and MGFS root into fixed GPT partition ranges in
`build/Mangrove/MangroveUSB.img`. It uses `parted` on Linux and `sgdisk` on
macOS for GPT creation.

The image population scripts install the loader on the ESP, `/boot/pith.elf`
and services under `/core`, commands under `/bin`, configuration under
`/conf`, and shared help and hardware data under `/share`. The image tools
also preserve the ownership rules for guest-created files when updating an
existing development root.

The embedded kernel font is generated from the repository's UNSCII source by
`tools/convert_unscii_hex.py`. The exFAT recommended up-case data is generated
by `tools/generate_exfat_upcase.py`; its source and provenance are documented
in [the exFAT provenance note](../filesystem/exfat/upcase-provenance.md).

## QEMU

`make run` uses Q35, 512 MiB of RAM, OVMF, an xHCI controller, USB mass
storage, a USB keyboard, user-mode networking, and an E1000 device. On Linux
the Makefile selects KVM with `-cpu host`. On Intel macOS it selects HVF; on
Apple Silicon it selects TCG with a warning because the guest is x86-64.

`make run` boots the persistent `.mangrove/MangroveDev.img`. The current
`run-usb` target is only an alias for `run`; it does not boot
`build/Mangrove/MangroveUSB.img`. Use the USB image with a separate QEMU
invocation or the physical-media test procedure.

The headless QEMU smoke boot used during development was performed on Linux.
It does not constitute macOS runtime validation; the macOS path is currently
statically validated only.

Extra QEMU arguments can be supplied with `QEMU_EXTRA_ARGS`, for example to
add disposable test devices. `scripts/stress_kvm_boot.sh` is a Linux/KVM
stress helper for an already-built USB image and is not part of the normal
image build.

## Validation and prerequisites

Check the platform-specific setup guides before building:

- [Linux setup](linux.md)
- [macOS setup](macos.md)

Useful preflight targets are `make check-image-deps`, `make check-usb-deps`,
and `make check-qemu-deps`. The last one checks the selected accelerator,
OVMF, and, on Linux, readable and writable `/dev/kvm`.

The authoritative implementation is in `Makefile`,
`scripts/make_image.sh`, `scripts/update_dev_image.sh`,
`tools/populate_mgfs.py`, `tools/update_mgfs.py`, and
`tools/copy_partition.py`.
