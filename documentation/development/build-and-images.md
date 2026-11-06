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
| `make usb-image` | Build a fresh GPT USB image at `$(BUILD_DIR)/Mangrove/MangroveUSB.img` (`build/` by default). |
| `make BUILD_DIR=build/x250-diagnostic usb-image` | Build into an isolated output tree without replacing the default `build/` artifacts. |
| `make run` | Build the QEMU development image with a trusted UTC RTC, then boot the persistent image with two vCPUs by default. |
| `QEMU_SMP=1 make run` | Boot with one vCPU for single-core regression testing. |
| `QEMU_SMP=4 make run` | Boot with four vCPUs for a larger SMP test. |
| `make exfat-upcase` | Regenerate and verify the embedded exFAT up-case data. |
| `make test-time` | Run the current host-side timekeeping tests. |
| `make test-unicode` | Run ASan/UBSan tests for the shared UTF-8 codec, C multibyte/wide conversions, scalar-safe line-editor edits, and terminal table text metrics. |
| `make test-terminal` | Run the current host-side terminal UTF-8 tests. |
| `make test-random` | Run sanitizer-backed host tests for the kernel CSPRNG, syscall validation, and loam wrapper. |
| `make test-guest-random` | Run the bounded isolated native CSPRNG smoke test with two QEMU vCPUs. |
| `make test-guest-threads` | Run three isolated two-vCPU KVM worker-lifecycle tests, including SMP execution, synchronization, worker exit, and resource reclamation. |
| `make test-guest-mgfs` | Run isolated [native MGFS guest persistence qualification](guest-filesystem.md). |
| `make -B nettest` | Build the guest network test program. |
| `make mkmgfs` | Build the MGFS image-creation tool. |
| `make mgfsck` | Build the MGFS checker. |
| `make clean` | Remove disposable `build/` output while preserving `.mangrove/`. |

Use the available validation targets above and the guest/runtime network
commands documented by the current image.

### Opt-in boot diagnostics

`X250_BOOT_TRACE=1` enables the opt-in bounded hardware boot trace and the
extra firmware storage-discovery report. It retains recent wrapped messages
and appends the selected discovery/failure report to the normal terminal cell
ring, using its Unscii font, colors, scrolling, and scrollback. The report is
written in one terminal presentation batch after a required bring-up failure
or before PID 1 launch. It includes GOP dimensions and USB-controller PCI command registers;
Intel xHCI configuration DWORDs D0/D4/D8/DC are reported read-only to investigate
companion-controller routing. For boot-log latency investigation it also reports
whether a prior log existed and its size, plus USB block-request and BOT command
counts, sizes, failures, and phase timings separately for file preparation and
snapshot writing. Footer version 2 also records bounded timing aggregates for
block-I/O gate and VFS operation-lock acquisitions, recovery-pin handoff, and
MGFS ordinary-mutation begin/body/complete phases. Each aggregate reports a
sample count, total elapsed milliseconds, and maximum sample; timings use
Pith's monotonic millisecond clock. Lock counts are timed acquisition attempts,
including immediate acquisitions, and do not mean every attempt blocked. The
phase counters are collected separately for file preparation and payload
writing. Categories can be nested, so their
totals must not be added together. USB callback and BOT timings exclude time
spent before entering those measured callbacks, including any block-gate or
VFS wait. These measurements are observational and do not change the storage
path. The trace also records initial, requested, and actual GOP modes,
SetMode/restore status, and a bounded mode inventory. Normal builds perform
the validated mode selection described in [boot flow](../architecture/boot.md)
but omit this extra inventory and report.

On a successful trace-enabled boot, Pith saves the captured kernel log and
trace to `/sys/logs/boot.log`. Before saving, it moves the preceding
`boot.log` to `/sys/logs/boot.previous.log`; the next trace-enabled boot
replaces that one older generation. The record includes a bounded diagnostic
footer with preparation and payload-write timings, USB block/BOT counters,
phase aggregates, and CRCs. A valid completion footer means the final footer
write completed;
the duration of that final write itself cannot be included in the same write.
The trace text is frozen after file preparation and before the payload write;
the footer and host-generated summary add payload completion timings and the
final save state without recursively rewriting the trace. Later kernel events
are outside that captured trace boundary.
If the trace ring overflowed, a write failed, or boot stopped before a valid
footer was written, extraction reports `INCOMPLETE`. A failure before MGFS is
mounted cannot be persisted by this mechanism. These records are written only
by trace-enabled builds; normal builds keep the existing boot-log behavior and
produce no boot-trace output or diagnostic footer.

The trace retains at most 128 wrapped lines. An exported trace is marked incomplete if the ring overflowed or
the export buffer could not hold every retained line. The host extractor
reports a valid save footer independently from trace completeness: a completed
payload can contain an incomplete trace. Footer version 1 remains readable but
does not contain phase aggregates; newer unsupported footer versions are
reported as incomplete rather than as absent.

Use the two-boot runner before a physical run to verify the current build and
the persistence/extraction path under OVMF/KVM:

```sh
python3 tests/guest/guest_boot_diagnostics_test.py \
  --image "$BUILD_DIR/Mangrove/MangroveUSB.img" \
  --build-dir "$BUILD_DIR" --accel kvm \
  --ovmf-code /path/to/OVMF_CODE.4m.fd \
  --ovmf-vars /path/to/OVMF_VARS.4m.fd \
  --output "$RUN/kvm-boot-diagnostics"
```

The runner boots the disposable image twice, executes a distinct Shoot
command after each boot, compares the first record byte-for-byte with the
second boot's `boot.previous.log`, extracts both records, and runs the
independent `mgfsck` checker after each guest has stopped. It never reads an
image while QEMU can write it. A passing KVM run qualifies this workflow only;
it does not establish physical hardware behavior.

After a physical boot, shut down or return to the host and detach the USB from
any guest before extraction. Select the whole removable device yourself, then
use the read-only extractor with a new output directory:

```sh
python3 tools/extract_boot_diagnostics.py \
  --source /dev/disk/by-id/REPLACE_WITH_OPERATOR_SELECTED_WHOLE_USB_DEVICE \
  --output "$RUN/evidence/boot-diagnostics" \
  --mgfsck "$BUILD_DIR/mgfsck"
```

The source is opened read-only. The extractor validates the primary and backup
GPT, copies the bounded MGFS root partition into its new output directory,
runs `mgfsck` independently, and exports `boot.log` and
`boot.previous.log`, when present, as raw files plus snapshot/trace text and an
`extraction.json` status report. Inspect and redact exported text before
attaching it to a qualification report. Extract before a third diagnostic
boot if both preceding runs must be retained: only the most recent two records
are kept.

Firmware checkpoint halt markers use the separate `BOOT_HANDOFF_CHECKPOINT`
option; its default `0` does not halt. The trace is disabled by default.

`make test-ovmf-discovery` exercises the Arch and Debian OVMF path layouts and
the explicit source override behavior without using host firmware files.

## Generated outputs

The disposable build tree, selected by `BUILD_DIR` and defaulting to `build/`,
contains per-program output directories and shared products. Important outputs
include:

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

The QEMU development-image targets (`make`, `make image`, `make run`,
`make fresh-image`, and `make fresh-run`) use `RTC_UTC_TRUST=1` because their
Q35/OVMF launch uses `-rtc base=utc,clock=host` by default. Set
`QEMU_RTC_CLOCK=vm` for deterministic virtual-time runs; this controls the
guest RTC, not the kernel monotonic source. Ordinary `make binaries` and the
hardware `make usb-image` path keep the untrusted RTC default. A build-local
RTC policy stamp makes changes to `RTC_UTC_TRUST`
invalidate kernel C objects in incremental builds; use separate `BUILD_DIR`
values when building both policies concurrently.

`make clean` removes `build/` but intentionally preserves `.mangrove/`.
Incremental image commands require MGFS 1.2. An older or unrecognized root
image is preserved and rejected before extraction, copying, or updating.
Back up any data you need, then explicitly recreate the development image with
`make fresh-image`. This command replaces the persistent development images.

## Image construction

`make fresh-image` updates the persistent development disk through
`scripts/update_dev_image.sh`. The script creates or updates a GPT disk with
an ESP and an MGFS root partition. It checks partition bounds and refuses to
update an image that is in use.
It also checks the existing raw or GPT root image format before changing it.

Both image scripts accept `--build-dir directory`; the `BUILD_DIR` environment
variable provides the same override for direct script use. Their input
binaries and generated ESP images are resolved within that tree, while the
repository's `tools/` and `share/` resources remain source inputs.

`make usb-image` runs the fresh-image path for a new output image and writes
the ESP and MGFS root into fixed GPT partition ranges in
`$(BUILD_DIR)/Mangrove/MangroveUSB.img`. It uses `parted` on Linux and `sgdisk` on
macOS for GPT creation.

The normal output is a compact 264225-sector image (135,283,200 bytes),
independent of destination capacity. Use the manually invoked
[USB flashing utility](usb-flashing.md) to write this image and relocate GPT
metadata on the USB. Plain `dd` of a compact GPT image onto larger media leaves
an invalid backup location. Building a full-capacity file for routine flashing
also makes the write scale unnecessarily with device capacity.

`USB_IMAGE_SECTORS` remains available for isolated image tests:

```sh
make BUILD_DIR=build/usb-capacity USB_IMAGE_SECTORS=30629376 usb-image
```

This optional path creates a sparse regular file and never accesses physical
media. ESP/root ranges stay fixed. See [destination capacity](../storage/image-layout.md#destination-capacity)
for the GPT validation contract. `make test-usb-image-capacity` checks sizing;
`make test-usb-flash` exercises real file-backed flashing, simulated Linux
admission, confirmation, failure handling and scratch cleanup without root.
`make test-gpt-image GPT_TEST_IMAGE=path/to/image` exercises production block/GPT
code against actual image metadata and corruption/read-failure controls.

The image population scripts install the loader on the ESP, `/boot/pith.elf`
and services under `/core`, commands under `/bin`, configuration under
`/conf`, and shared help and hardware data under `/share`. The image tools
also preserve the ownership rules for guest-created files when updating an
existing development root.

The incremental MGFS updater treats the Record bitmap as authoritative. A
released Record slot can retain old table bytes; before clearing such a stale
entry, the updater validates its checksum and mapping and inventories the
currently allocated Records. An old data-block reference is accepted only if
the allocation bitmap marks that block allocated and exactly one valid live
Record owns it. This permits ordinary block reuse without confusing the old
Record bytes for current ownership. Allocated blocks without a live owner,
malformed records, and allocated stale extent-list blocks remain hard errors.
Reconciliation clears only stale Record-table bytes; it does not free or
rewrite data blocks. Updates are published only after validation succeeds.

Live Record IDs are validated against the superblock's `next_record_id`, not
the number of table slots. Updates preserve large-ID user objects, and manual
installation may assign new 64-bit IDs even after the ID watermark exceeds
table capacity. Free-slot availability and exhaustion of the 64-bit identity
space remain separate checks; neither permits reuse of an old identity.

The kernel embeds native Unscii 8x16 PSF2 glyphs generated from the
repository's source bitmap by `tools/convert_unscii_hex.py`. The native 8x8
source is checked by converter tests but is not embedded in Pith. The system
console does not scale glyphs or select a different variant based on
framebuffer dimensions. The exFAT recommended
up-case data is generated
by `tools/generate_exfat_upcase.py`; its source and provenance are documented
in [the exFAT provenance note](../filesystem/exfat/upcase-provenance.md).

When an older image already contains both `/share/help/info` and
`/share/help/about`, the updater distinguishes the manual reader by its
`name=info` and `usage=info PROGRAM` fields and refreshes that record in place.
It still rejects an ambiguous legacy `info` record that conflicts with an
existing `about` record.

## QEMU

`make run` uses Q35, 512 MiB of RAM, OVMF, an xHCI controller, USB mass
storage, a USB keyboard, user-mode networking, and an E1000 device. It passes
`-smp $(QEMU_SMP)` and defaults to two vCPUs. Use `QEMU_SMP=1` for the
single-core regression path or `QEMU_SMP=4` for a larger SMP run. The same
setting is used by `fresh-run`. On Linux the Makefile selects KVM with
`-cpu host`. On Intel macOS it selects HVF; on Apple Silicon it selects TCG
with a warning because the guest is x86-64.

`make run` boots the persistent `.mangrove/MangroveDev.img`. The current
`run-usb` target is only an alias for `run`; it does not boot
`$(BUILD_DIR)/Mangrove/MangroveUSB.img`. Use the USB image with a separate QEMU
invocation or the physical-media test procedure.

Host fixtures and target compilation do not validate QEMU guest execution.
Run disposable guest workloads for the selected platform and accelerator;
Linux/KVM results do not establish macOS/HVF or TCG behavior.

Extra QEMU arguments can be supplied with `QEMU_EXTRA_ARGS`, for example to
add disposable test devices. Select its CPU count with `QEMU_SMP`; for example,
`QEMU_SMP=4 ./scripts/stress_kvm_boot.sh 20`. The script is a Linux/KVM stress
helper for an already-built USB image and is not part of the normal image
build.

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
