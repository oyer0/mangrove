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

The root partition contains MGFS. The loader mounts it with its dedicated,
read-only MGFS reader and opens `/boot/pith.elf`. It validates a 64-bit,
little-endian x86-64 executable ELF image, loads each `PT_LOAD` segment at the
physical and high-half virtual addresses recorded by the image, and preserves
the segment write and execute permissions in the bootstrap page tables.

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
the direct map, initializes memory management, builds the permanent kernel page
tables, and replaces the transitional mappings.

## Authoritative code

- `boot/src/main.c`, `boot/src/filesystem.c`, and `boot/src/bootstrap.c`
- `boot/src/handoff.s` and `kernel/src/entry.s`
- `include/bootinfo.h` and `include/address_layout.h`
- `kernel/src/main.c`
