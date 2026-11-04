# Boot flow and kernel handoff

Mangrove boots as an x86-64 UEFI application. The loader, Pith kernel, and
shared handoff definitions are built together; `include/bootinfo.h` is the
authoritative C ABI between them.

## Root selection and kernel loading

The loader starts from its UEFI image handle, obtains the block device that
contains the loaded EFI application, and enumerates whole-disk Block I/O
handles. It currently accepts 512-byte logical sectors and GPT media. A boot
candidate must contain exactly one partition named `MANGROVE_ROOT` and one EFI
System Partition named `MANGROVE_ESP`. When UEFI reports the loaded image as a
GPT partition, the role-associated ESP must be that partition. More than one
matching disk is treated as ambiguous and boot is refused.

Every Block I/O read uses an `EFI_BOOT_SERVICES_DATA` pool buffer aligned to
the media's `IoAlign`, then copies successful reads into the loader's scratch
buffer. This keeps firmware/DMA-facing storage buffers in firmware-allocated
memory while preserving the loader's static scratch-buffer ownership.

The root partition contains MGFS. The loader mounts it with its dedicated,
read-only MGFS reader and opens `/boot/pith.elf`. It validates a 64-bit,
little-endian x86-64 executable ELF image, loads each `PT_LOAD` segment at the
preferred physical offset recorded by the image within one firmware-selected
contiguous range, while retaining the linked high-half virtual addresses. The
selected physical base is passed in `BOOT_INFO`; bootstrap and permanent
kernel page tables map each segment using the same relocation offset and
preserve the segment write and execute permissions.
Segment-load failures identify whether alignment validation, physical page
allocation, file seeking, or segment reading failed; allocation diagnostics
include the requested physical range.
UEFI Simple File System exposes files only on firmware-supported filesystems;
it is not a raw-sector interface and cannot substitute for validated GPT and
MGFS reads. UEFI Disk I/O is byte-oriented but is layered on Block I/O, so it
does not provide an independent storage fallback.

Root-partition discovery errors identify missing versus ambiguous matching
disks in the opt-in `X250_BOOT_TRACE=1` firmware diagnostic build. MGFS
admission failures are reported separately from discovery failures; the boot
status remains the exact EFI status returned by the failing operation. The
trace build prints a bounded summary designed to fit the firmware text screen
without scrolling. It includes loaded-ESP protocol status, the device-path
partition range, enumerated partition handles' reported GPT role and
device-path partition number/range, parent-path match, and LBA 0 prefix, plus
whole-disk geometry and LBA 0/LBA 1 results. For candidate disks it also
compares Disk I/O LBA 1 and the parent read at the loaded ESP's starting LBA.
These diagnostic probes do not alter GPT selection or integrity validation
and are limited to four whole-disk and four partition handles. Production
builds omit the extra probes and detailed report.

## Firmware state collected

Before leaving boot services, the loader collects:

- the final UEFI memory map and descriptor metadata;
- GOP framebuffer physical address, size, dimensions, and scan-line pitch;
- the ACPI 2.0 RSDP, falling back to the ACPI 1.0 table when necessary;
- a 16-page physical handoff stack;
- the physical address of the bootstrap PML4.

All persistent loader allocations are complete before the final memory-map
snapshot. If `ExitBootServices` requires a refreshed map, the loader publishes
the map that accompanied the successful call.

Hardware handoff diagnostics are opt-in through
`BOOT_HANDOFF_CHECKPOINT=1..8`; the default `0` has no checkpoint halts.
Checkpoints halt at the completed-bootstrap/pre-`ExitBootServices` boundary,
after successful `ExitBootServices`, after the bootstrap CR3 load, after
handoff-stack installation, at kernel entry, after the relocated kernel stack
is selected, at first C entry, and after Pith initializes its console. The
checkpoint is shown as a colored 16-by-16 block at the bottom-right of the
framebuffer; checkpoint 1 also prints its stage in firmware text. Each
checkpoint image deliberately halts at only its selected stage, so a reset
before the marker indicates that the selected boundary was not reached.
Handoff-stack allocation errors are printed and halted rather than returned
silently to the firmware boot manager.

## `BOOT_INFO` contract

`BOOT_INFO` begins with its structure size and carries the memory map,
framebuffer information, RSDP, handoff-stack physical range, and bootstrap
PML4. The framebuffer has both a physical field and an initial pointer because
the kernel later replaces the firmware mapping with its permanent ioremap
alias. The live handoff-stack range lets physical-memory initialization reserve
those pages.

The loader installs the bootstrap CR3 and stack with interrupts disabled, then
jumps to the ELF entry with a pointer to `BOOT_INFO`. Kernel assembly switches
immediately to the kernel image stack before entering `kmain_high`. Early
kernel code copies the handoff record, converts physical firmware pointers to
the direct map, and establishes the BSP's kernel GS base before terminal or
spinlock use. It then initializes memory management, builds the permanent
kernel page tables, and replaces the transitional mappings. GDT setup reloads
the BSP's segment state and re-establishes the kernel GS base.

Before that construction, the BSP establishes its PAT policy without changing
firmware MTRRs or any PAT selector used by the loader. The permanent GOP
mapping has a framebuffer-specific WC eligibility check and UC fallback. Cache
and local global/non-global TLB draining precede activation of the new root;
the old framebuffer alias is not retained. AP cache-policy validation precedes
online admission. See [memory layout](memory.md) for the exact restrictions.

Before constructing page tables with non-executable entries, the loader checks
the BSP's extended CPUID NX feature and sets `EFER.NXE`; boot stops if the CPU
cannot enforce those entries. The handoff sets `CR0.WP` before activating the
bootstrap root, so supervisor writes also respect read-only page permissions.
Pith verifies and establishes the same state during per-CPU initialization.

On normal startup, Pith prints the shared `MANGROVE_NAME` and
`MANGROVE_VERSION` on one line. Successful initialization phases are silent;
warnings, failures, and fatal diagnostics remain visible. The screen is cleared
before control enters PID 1.

## Authoritative code

- `boot/src/main.c`, `boot/src/filesystem.c`, and `boot/src/bootstrap.c`
- `boot/src/handoff.s` and `kernel/src/entry.s`
- `include/bootinfo.h` and `include/address_layout.h`
- `kernel/src/main.c`
