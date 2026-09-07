# Mangrove

Mangrove is a from-scratch x86-64 operating system written in C. It includes
a UEFI bootloader, the Pith kernel, a freestanding libc, userspace commands
and services, and host tools for building and inspecting system images.

Current storage support includes GPT images, MGFS, FAT32, and exFAT removable
volumes, with filesystem lifecycle policy handled by `volumed` and inspection
provided by `lsdsk` and `diskutil`.

Mangrove is developed with substantial assistance from AI coding tools.
Architecture, project direction, review, testing, integration, and final
acceptance remain human-led.

## Build and run

```sh
make -B binaries -j4
make fresh-image
make usb-image
make run
```

The detailed build, image, and host-dependency requirements are documented in
[docs/development/build-and-images.md](docs/development/build-and-images.md).
Subsystem contracts and format documentation are indexed in
[docs/README.md](docs/README.md).

## License

Project-owned Mangrove source is licensed under the GNU General Public License
version 3 only. See [LICENSE](LICENSE). Third-party data and generated assets
retain the separate provenance and license notices stored beside them.

Contribution guidance is in [CONTRIBUTING.md](CONTRIBUTING.md).
