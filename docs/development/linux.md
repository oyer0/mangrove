# Linux development setup

Linux is Mangrove's supported full build environment. The examples below are
for Debian or Ubuntu and install the tools used by the current Makefile and
image scripts.

## Install dependencies

```sh
sudo apt-get update
sudo apt-get install \
  build-essential binutils \
  gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 \
  make qemu-system-x86 ovmf python3 \
  dosfstools mtools parted gptfdisk socat psmisc lsof
```

The host compiler and GNU binutils provide GCC, `ld.bfd`, `ar`, and
`objcopy`. The MinGW-w64 packages provide the UEFI commands
`x86_64-w64-mingw32-gcc` and `x86_64-w64-mingw32-ld`.

`parted` is used to create the GPT in the USB image. `gptfdisk` supplies
`sgdisk` for optional host-side GPT inspection. `socat` is useful for
headless QEMU monitor and capture workflows, while `psmisc` and `lsof` help
the persistent-image update script detect an image that is still in use.

## Verify the toolchain

Run the following before the first build:

```sh
command -v gcc ld.bfd ar objcopy
command -v x86_64-w64-mingw32-gcc x86_64-w64-mingw32-ld
command -v make python3 qemu-system-x86_64 mkfs.fat mmd mcopy parted
test -r /usr/share/OVMF/OVMF_CODE_4M.fd
test -r /usr/share/OVMF/OVMF_VARS_4M.fd
```

The Makefile checks the MinGW-w64 commands when invoked. The image and QEMU
preflight targets check the remaining tools and firmware paths and print
package hints when something is missing.

## Build and run

From the repository root:

```sh
make -B binaries -j4
make fresh-image
make usb-image
make run
```

`make run` currently requires KVM on Linux because the Makefile selects the
KVM accelerator and checks that `/dev/kvm` is readable and writable. Add your
account to the host's `kvm` group if required, then start a new login session.
Image creation and host-side tests do not require KVM. A manual QEMU TCG
invocation is possible for debugging, but there is no supported no-KVM Linux
override in the current Makefile.

The standard OVMF files are expected at:

```text
/usr/share/OVMF/OVMF_CODE_4M.fd
/usr/share/OVMF/OVMF_VARS_4M.fd
```

Useful validation commands are:

```sh
make exfat-upcase
make test-time
make test-terminal
make -B nettest
make check-image-deps
make check-usb-deps
make check-qemu-deps
```

## Outputs and cleanup

The loader, kernel, programs, and host tools are placed under `build/`.
`make usb-image` creates `build/Mangrove/MangroveUSB.img`. The persistent QEMU
disk and its staged root are under `.mangrove/`.

```sh
make clean
```

removes disposable build output and preserves the persistent development
state. Use `make fresh-image` when the persistent development disk itself
should be reset.
