# Mangrove

Mangrove is a from-scratch x86-64 operating system written in C.

It has its own UEFI bootloader, kernel, libc, userspace, shell, services, networking, filesystems, and system tools. The long-term goal is a complete desktop operating system built around Mangrove's own design instead of copying Linux or Windows conventions.

Mangrove is developed with substantial assistance from AI coding tools. I make the architecture and design decisions, review and test the changes, and decide what actually gets accepted into the project.

## Build and run

```sh
make -B binaries -j4
make fresh-image
make usb-image
make run
```

More detailed build and subsystem documentation lives in [docs/](docs/).

## License

Mangrove's project-owned source is licensed under GPL-3.0-or-later. See [LICENSE](LICENSE).

Third-party data and assets keep their own license and provenance information.

See [CONTRIBUTING.md](CONTRIBUTING.md) if you want to contribute.
