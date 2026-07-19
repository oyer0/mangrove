# Mangrove

Mangrove is a personal Unix-like operating system written from scratch in C.

## Philosophy

- Write as much of the operating system as practical.
- Prefer simplicity over unnecessary abstraction.
- Build for a single primary user.
- Remain free software under the GNU GPLv3.

## Goals

- Custom UEFI bootloader
- Monolithic kernel
- POSIX-compatible userland
- Terminal-first interface
- Built-in pane management
- Native mail client
- Native Matrix client
- Media playback
- Basic web browsing
- Self-hosting

## Current status

Mangrove is in active development.

The bootloader can initialize the UEFI environment, load the kernel into memory, and prepare the system for transferring control to it.

The next milestone is booting into the kernel.
