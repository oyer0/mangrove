# Kernel and process memory layout

Mangrove uses a fixed x86-64 high-half kernel layout shared by the loader and
kernel through `include/address_layout.h`.

| Region | Start | End or size | Purpose |
| --- | --- | --- | --- |
| User addresses | `0x0000000000000000` | below `0x0000800000000000` | Per-process mappings |
| Anonymous mappings | `0x0000400000000000` | below `0x00007fff00000000` | Process `memory_map` allocations |
| User stack | below `0x00007ffffff00000` | 8 pages initially | Executable stack |
| Physical direct map | `0xffff800000000000` | 64 TiB | Ordinary mapped RAM |
| Kernel heap | `0xffffc00000000000` | `0xffffc08000000000` | PMM-backed heap pages |
| MMIO ioremap | `0xffffc08000000000` | `0xffffc10000000000` | Device mappings |
| Kernel image | `0xffffffff80000000` | linked image extent | Pith text, data, and BSS |

The kernel image is loaded at physical address `0x00100000` and linked at
`0xffffffff80000000`. The fixed 1 MiB placement fits the current QEMU/OVMF
map; the previous 2 MiB placement crossed an ACPI NVS reservation after the
kernel grew. This is not a universal UEFI guarantee; a future relocatable or
firmware-selected placement would be more robust. Direct-map conversions are
valid only for physical addresses below the 64 TiB map limit. MMIO is
deliberately excluded from that map and receives separate cache-controlled
ioremap addresses.

## Bootstrap and permanent mappings

The UEFI loader creates a temporary low identity mapping for the firmware
handoff and a high direct map for supported RAM descriptors. It also maps the
kernel ELF segments with their individual permissions. This bootstrap CR3
exists only to cross the firmware-to-kernel boundary.

Pith builds a new master kernel page-table root. Kernel text is executable and
read-only; read-only data is non-executable; writable data and BSS are
non-executable. The direct map, heap, ioremap branch, and kernel image remain
supervisor-only.

## Process address spaces

Each process receives a fresh lower half and references only the fixed,
supervisor-only high-half branches from the master kernel root. User page-table
and leaf-frame ownership is tracked separately from x86 permission bits so
destroying a process cannot reclaim shared kernel tables. Address-space
validation rejects lower-half aliases to kernel mappings and any user bit in a
shared kernel branch.

ELF `PT_LOAD` pages are mapped according to their write and execute flags. The
loader rejects segments below page one, beyond the user limit, outside the
file, or with invalid ranges or alignment. Anonymous mappings are page-rounded
and placed in the dedicated anonymous region. Syscalls validate every user
buffer against the current user mappings before dereferencing it.

## Authoritative code

- `include/address_layout.h`
- `kernel/include/address_space.h` and `kernel/include/vmm.h`
- `boot/src/bootstrap.c`
- `kernel/src/address_space.c`, `kernel/src/vmm.c`, and
  `kernel/src/elf_loader.c`
