# Mangrove documentation

This index links to current system behavior, user guides, and development
procedures. Source code is authoritative for implementation details.

## User guides

- [Shoot shell](shell/README.md): overview and supported execution model.
  - [Command language](shell/language.md): quoting, variables, blocks, scripts,
    command execution, and output redirection.
  - [Interactive use](shell/interactive.md): line editing, completion,
    configuration, and prompts.
  - [Command history](shell/history.md): in-memory history, persistence, and
    concurrent-session behavior.
- [Reed editor](editor/README.md): editing, file handling, safe saving, and
  current text limits.
- [Native command reference](userspace/commands.md): installed commands,
  administrative utilities, services, and development-only programs.
- [Terminal behavior](architecture/terminal.md): console input, framebuffer
  rendering, terminal-control ABI, and compatibility limits.

## System architecture

- [Design principles](design-principles.md): engineering priorities, interface
  boundaries, and user control.
- [Boot flow](architecture/boot.md): UEFI root selection, kernel loading, and
  handoff.
- [Memory layout](architecture/memory.md): kernel regions, bootstrap mappings,
  and process address spaces.
- [Kernel allocator architecture proposal](architecture/kernel-allocator-proposal.md):
  a candidate design for small-object allocation; it does not describe the
  current implementation.
- [Processes and syscalls](architecture/processes-and-syscalls.md): process,
  handle, executable, CWD, pathname, and checked-I/O contracts.
- [SMP architecture](architecture/smp.md): CPU discovery, AP startup,
  per-CPU state, scheduling, and current synchronization boundaries.
- [IPC and supervision](architecture/ipc-services.md): endpoints, request
  context, service definitions, and Sprout policy.
- [Userland services](services/README.md): boot-to-userspace relationships,
  login, storage, networking, logging, and device policy.
- [Identity and authorization](security/identity-and-authorization.md):
  accounts, sessions, privileges, and PASS.
- [Network architecture](network/architecture.md): kernel networking,
  networkd policy, configuration, and client interfaces.
- [DHCP lifecycle](network/dhcp-lease.md): lease ownership and timers.
- [C runtime, time, and text](architecture/c-runtime.md): loam interfaces,
  ABI, supported behavior, and limitations.
- [Kernel timekeeping](kernel/timekeeping.md): monotonic and realtime clocks.

## Storage and filesystems

- [System image layout](storage/image-layout.md): GPT roles, root hierarchy,
  services, and image variants.
- [Storage lifecycle](storage/lifecycle.md): block identity, discovery,
  mounting, authorization, removal, and recovery ownership.
- [MGFS](filesystem/mgfs/README.md): native filesystem behavior and supported
  operations.
  - [MGFS on-disk format](filesystem/mgfs/format.md).
  - [MGFS volume labels](filesystem/mgfs/labels.md).
- [FAT32 names](filesystem/fat32/names.md): Unicode comparison and imported
  short-name policy.
- [FAT32 labels](filesystem/fat32/labels.md): label mutation and recovery.
- [FAT32 replacement and sync](filesystem/fat32/replacement.md): conditional
  replacement and flush limits.
- [exFAT](filesystem/exfat/README.md): supported layouts, operations, and
  resource limits.
  - [exFAT up-case table provenance](filesystem/exfat/upcase-provenance.md).

## Build and qualification

- [Build and image pipeline](development/build-and-images.md): targets,
  generated inputs, image population, and QEMU topology.
- [Linux setup](development/linux.md).
- [macOS setup](development/macos.md).
- [Mangrove Test Suite](development/test-suite.md): host and guest profiles,
  commands, evidence, and result states.
- [Focused test coverage](development/tests.md): host regressions and fixture
  boundaries.
- [Filesystem qualification](development/filesystem-qualification.md): MFQS
  model, campaigns, replay, and checker boundaries.
- [Native filesystem stress tool](development/native-filesystem-stress.md):
  host and guest `fsstress` use.
- [Native filesystem integration qualification](development/integration-qualification.md).
- [MGFS guest persistence](development/guest-filesystem.md).
- [Safe-file and application qualification](development/guest-safe-files.md).
- [Secondary-volume qualification](development/guest-secondary-volumes.md).
- [Device lifetime and recovery qualification](development/guest-device-lifetime.md).
- [MGFS directory traversal measurements](development/mgfs-traversal-performance.md).
- [FAT metadata measurements](development/fat-metadata-performance.md).
