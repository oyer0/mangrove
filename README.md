# Mangrove

Mangrove is a from-scratch x86-64 operating system written in C.

It has its own UEFI bootloader, kernel, libc, userspace, shell, services, networking, filesystems, and system tools. The long-term goal is a complete desktop operating system with its own design and conventions.

Mangrove is developed with a lot of help from AI coding tools.

## Build and run

```sh id="g2t7g4"
make -B binaries -j4
make fresh-image
make usb-image
make run
```

On macOS, use `gmake` for these commands.

More detailed documentation lives in [docs/](docs/).

- [Build and image pipeline](docs/development/build-and-images.md)
- [Linux setup](docs/development/linux.md)
- [macOS setup](docs/development/macos.md)

## License

Mangrove's project-owned source is licensed under GPL-3.0-or-later. See [LICENSE](LICENSE).

Third-party data and assets keep their own license and provenance information.
