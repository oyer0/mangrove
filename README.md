# Mangrove

Mangrove is my personal operating system, written from scratch in C.

## Philosophy

A few ideas guide the project:

- Write as much of the operating system as practical.
- Prefer simplicity over unnecessary abstraction.
- Build a coherent system rather than a collection of loosely connected components.
- Keep it free software under the GPLv3.

## Vision

I'm building Mangrove with the goal of creating a complete desktop operating system featuring:

- A custom UEFI bootloader.
- A monolithic kernel.
- A POSIX-compatible userspace.

Rather than assembling an operating system from existing projects, I want Mangrove to feel like a single, cohesive system with software designed just for it.

## Status

Mangrove is a long-term personal project and is under active development.

Right now I'm focused on the kernel and hardware drivers before moving on to userspace and the graphical desktop.

## Build dependencies

On macOS, install the native image-building tools with Homebrew:

```sh
brew install llvm qemu dosfstools mtools gptfdisk
```

Homebrew's LLVM is keg-only, so add it to your build environment if its tools
are not already on `PATH`:

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"
```

`make usb-image` uses `sgdisk` from `gptfdisk` on macOS. Linux continues to use
`parted`; on Debian/Ubuntu the image-specific dependencies can be installed with:

```sh
sudo apt-get install parted dosfstools mtools python3
```

VM runs use KVM on Linux and HVF on Intel macOS. On Apple Silicon, the x86_64
guest runs with QEMU TCG software emulation because HVF cannot accelerate this
guest architecture.
