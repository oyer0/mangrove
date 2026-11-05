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

## Native USB root discovery

Pith enables and verifies PCI memory access and bus mastering before initializing
xHCI. Firmware Block I/O is unavailable after handoff; a USB root therefore
requires native USB enumeration, a supported mass-storage interface, successful
BOT inquiry/readiness/capacity commands, block registration, and GPT role
discovery before root selection. An internal SATA disk can satisfy the
storage callback while the USB root is absent. Root selection still requires
exactly one registered `MANGROVE_ROOT` partition with a valid MGFS probe.

The xHCI boot quiescence gate rejects outstanding port work, enumeration
admission, commands, transfers, and event work. Both READY and FAILED port
states are terminal for that gate. Before accepting quiescence, the boot caller
requests a fresh event-worker pass and waits for its acknowledgement after
event dispatch and port work. This also drains events queued while MSI-X is
masked, even if the first CCS scan and software work flags were empty. Only
the worker consumes the Event Ring. A missing acknowledgement or unfinished
work fails the bounded boot deadline. Controller callback success means the
current work drained; it does not guarantee that later attachments have arrived
or that a USB mass-storage disk was found. Bootstrap interrupt acknowledgement
does not clear PORTSC change flags: the event owner acknowledges those only
after dispatching their connection state.

Before selecting root, the bootstrap thread grants native xHCI discovery up to
ten seconds to publish a GPT partition with the exact required root role. It
returns as soon as that role is present and a fresh owner pass has drained
outstanding setup. With no registered disk, the storage callback first grants
up to ten seconds for any block registration before rejecting its required
dependency. An unrelated disk satisfies storage admission and then root
discovery must still find the exact required role. Each wait has one fixed
deadline; an idle pass, an unrelated disk, or an enumeration failure does not
extend it or constitute root readiness. MGFS probing, duplicate-role rejection,
and mount validation still run after this fence and retain their exact errors.

The wait parks the bootstrap thread through `scheduler_sleep`, with no block
I/O or VFS locks held; its readiness predicate reads registration metadata only.
The event worker continues to own event consumption, port setup, USB transport,
and GPT publication. That worker cannot call the wait on itself. Requested
boot passes also reconcile previously unhandled CCS levels, including an attach
whose CSC has already cleared, without resetting READY/FAILED ports, replacing
live device slots, or duplicating queued/admitted setup. These bounded passes
allow boot discovery progress even when a device interrupt is absent. They do
not replace runtime interrupt routing or retry a failed enumeration.

CCS alone does not permit enumeration. The reset entry accepts a powered USB2
Polling/disabled link or an enabled U0 link. Inactive, suspended, unpowered,
resetting, or otherwise unsupported states fail explicitly without speculative
recovery writes. In particular, PORTSC `0x6c1` means CCS=1, PED=0, PP=1,
PLS=6 (Inactive), speed ID=1, with no change bits; USB2 awaiting reset maps to
PLS=7 (Polling), not Inactive. See xHCI section 5.4.8 and table 5-28 in the
[xHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf).
The opt-in trace reports per-port raw state, PLS, PP, setup status/reason, USB
mass-storage count, and root-role inventory after the discovery opportunity,
distinguishing absent ports, unusable links, failed setup, and unrelated disks.
USB GPT scanning also reports its exact validation result before block
publication: `VALID`, `READ_FAILED`, `INVALID`, `ABSENT`, or `UNSUPPORTED`.
This diagnostic adds no reads or retries; USB registration can succeed without
partitions, while the required root role remains unavailable. The flashed disk's
GPT must satisfy the [destination contract](../storage/image-layout.md#destination-capacity);
the host flashing utility relocates compact-image metadata before native boot.

### Intel USB port routing

On Broadwell/Wildcat Point-LP xHCI (`8086:9cb1`), controller ownership and USB
pin routing are separate operations. Pith sets the legacy OS Owned semaphore
even when BIOS Owned is already clear, completes the handoff/reset, then
configures permitted routes before starting the controller and probing ports.

Intel datasheet [330837-002](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/core-m-processor-platform-i-o-datasheet.pdf),
table 1-4 and sections 13.1.33–13.1.36, define the policy. PCI D0 selects xHCI
for USB2 pins; D4 authorizes which selectors software may change. D8 enables
SuperSpeed visibility; DC authorizes which enables software may change. The
implemented fields cover eight switchable USB2 ports and four SuperSpeed ports.
The driver ORs only firmware-authorized bits in those fields into the existing
selectors, enabling SuperSpeed visibility before transferring USB2 pins. It
preserves all other bits and never writes either permission register. Already
configured routes cause no writes. Inaccessible configuration or a failed
write readback prevents controller start; no retry or forced permission change
is performed. Other controller IDs receive no configuration accesses through
this routing helper.

Firmware-denied ports retain their existing owner. Pith has no native EHCI
driver, so an EHCI-only device cannot supply the native root filesystem.
`X250_BOOT_TRACE=1` reports permitted masks and before/after selectors alongside
normal discovery results. A successful routing readback does not prove device
enumeration, mass-storage readiness, or a mounted root filesystem.

### Port visibility diagnostics

The opt-in X250 trace records timestamped, read-only port samples after reset,
after Intel routing, after controller start, after boot event draining, and
before the final required bring-up failure report. The final sample uses the
existing failure-presentation interval; it adds no wait, port reset, power
write, enumeration retry, or filesystem retry. The report includes USBCMD and
USBSTS, CCS and PP bitmaps, Supported Protocol port-number ranges, CAPLENGTH,
and the first PORTSC physical address. Raw per-port values are shown as
`route/start/final`, including disconnected ports, for up to 32 register sets.
Normal enumeration still scans all advertised ports.

PORTSC numbering follows xHCI section 5.4.8: operational base plus
`0x400 + 0x10 * (port - 1)`. Supported Protocol ranges identify which numbered
register sets describe USB2/USB3; they do not relocate the registers. Intel
330837-002 sections 13.2.2.10 and 13.2.2.12 specify PP as read-only one on this
controller. A routing readback alone does not establish electrical attachment;
the samples distinguish unpowered/inaccessible registers, immediate versus
later attachment, and controller halt/error state.
