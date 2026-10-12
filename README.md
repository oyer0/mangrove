# Mangrove

Mangrove is a personal x86-64 operating system written from scratch in C.

It has its own UEFI bootloader, kernel, libc, userspace, shell, services, networking, filesystems, and system tools. The long term goal is a complete usable desktop operating system with its own design and conventions.

## Build and run

```sh
make -B binaries -j4
make fresh-image
make usb-image
make run
```

On macOS, use `gmake` for these commands.

More detailed documentation lives in [documentation/](documentation/).

- [Build and image pipeline](documentation/development/build-and-images.md)
- [Linux setup](documentation/development/linux.md)
- [macOS setup](documentation/development/macos.md)

## License

Mangrove's project-owned source is licensed under GPL-3.0-or-later. See [LICENSE](LICENSE).

Third-party data and assets keep their own license and provenance information.
