# Build and image pipeline

The top-level Makefile builds the UEFI loader, Pith kernel, host MGFS tools,
libc, system services, and standalone userspace programs. The image scripts
then place those outputs into the role-based system disk described in
[the storage image layout](../storage/image-layout.md).

## Main targets

| Target | Result |
| --- | --- |
| `make` or `make image` | Incrementally update the persistent development disk |
| `make fresh` or `make fresh-image` | Recreate the persistent development disk |
| `make usb` or `make usb-image` | Build a fresh `build/Mangrove/MangroveUSB.img` |
| `make binaries` | Build loader, kernel, libc, services, commands, and host tools |
| `make run` | Update and boot the persistent development disk in QEMU |
| `make run-usb` | Boot the fresh USB image in QEMU |
| `make exfat-upcase` | Regenerate and verify the embedded exFAT up-case include |

Specialist targets build individual programs, host tools, fonts, tests, or
image variants. Host dependency checks fail with a diagnostic when required
firmware, image tools, QEMU acceleration, or KVM access is unavailable.

## Compilation boundaries

The UEFI loader is a freestanding PE/COFF application built for the Windows
x86-64 ABI. Pith and userspace are freestanding x86-64 ELF images with
separate linker scripts. Kernel C is compiled in the kernel code model without
red-zone, SIMD, or floating-point use. Libc has kernel-shared utility objects
and a userspace archive containing the syscall bridge and runtime support.

Boot and kernel sources are discovered beneath their canonical `src/` trees.
Standalone userspace targets and their required common objects are listed
explicitly, making the image payload an intentional manifest rather than a
copy of every build artifact.

## Generated build inputs

The kernel PSF font is generated from the repository's UNSCII hex source by
`tools/convert_unscii_hex.py` and linked as a binary object. The embedded exFAT
recommended up-case data is generated deterministically from
`tools/exfat_upcase_table.txt`; its provenance and verification are documented
in [the exFAT provenance note](../filesystem/exfat/upcase-provenance.md).

PCI and USB ID databases under `share/hardware/`, command help records under
`share/help/`, and their indexes are copied into `/share` by the MGFS
population tools. They are runtime data, not compiled into each command.

## Root payload construction

`scripts/make_image.sh` creates a sparse 64 MiB FAT32 ESP containing
`EFI/BOOT/BOOTX64.EFI`. For a fresh root it runs `mkmgfs` with 16,384 MGFS
blocks, then `tools/populate_mgfs.py`. For an existing root it runs
`tools/update_mgfs.py`. Both tools consume the same ordered payload manifest;
the update path replaces system-owned records while preserving guest-owned
files and persistent state according to its record ownership rules.

The population tools install the kernel at `/boot/pith.elf`, services under
`/core`, commands under `/bin`, configuration defaults under `/conf`, and
shared help and hardware data under `/share`. Account and user-home records
are created as MGFS metadata and payload, not by mounting a host directory.

## Persistent development image

`scripts/update_dev_image.sh` extracts `MANGROVE_ROOT` from
`.mangrove/MangroveDev.img`, updates the staging root image, and writes that
exact partition range back through `tools/copy_partition.py`. The helper
validates disk size, GPT geometry, partition bounds, overlap, and a unique root
role before copying. An in-use image is rejected rather than modified beneath
QEMU.

On a new disk the script creates the fixed GPT and copies the complete ESP and
root payloads. On an existing disk it updates the EFI loader in place so
unrelated ESP files survive, and writes only the root partition extent.

## Fresh USB image and QEMU

The `flash-image`/`usb-image` path always creates a fresh MGFS root and a fresh
GPT output at `build/Mangrove/MangroveUSB.img`, then copies the ESP and root
images into their fixed partition extents.

The normal QEMU machine is Q35 with 512 MiB RAM, UTC RTC, OVMF, user-mode
networking with E1000, and an xHCI controller. The development or USB disk is
presented as USB mass storage, and a USB keyboard is attached separately.
Linux uses KVM with the host CPU; supported macOS hosts use HVF, with TCG as
the Apple Silicon fallback. Extra test devices may be supplied through the
explicit `QEMU_EXTRA_ARGS` build variable.

`scripts/stress_kvm_boot.sh` boots an already-built USB image headlessly and
keeps per-run serial logs and framebuffer captures. It does not rebuild or
repopulate the image as part of each probe.

## Authoritative code

- `Makefile`
- `scripts/make_image.sh` and `scripts/update_dev_image.sh`
- `tools/populate_mgfs.py`, `tools/update_mgfs.py`, and
  `tools/copy_partition.py`
- `tools/generate_exfat_upcase.py`, `tools/convert_unscii_hex.py`, and
  `scripts/stress_kvm_boot.sh`
