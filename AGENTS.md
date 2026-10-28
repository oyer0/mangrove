# Mangrove — Agent Guide

## Project

Mangrove is a from-scratch x86-64 UEFI operating system. It has its own UEFI
loader, the Pith kernel, native Ring 3 programs and services, and the loam C
standard library. It is not based on Linux or another operating-system kernel.
The project targets broad, practical C17 library support; it does not claim
complete hosted C17 or POSIX conformance. The root [README](README.md) gives a
short project introduction. The [documentation index](documentation/README.md)
is the catalog of current technical contracts and developer procedures.

**All implementation code incorporated into Mangrove must be original
Mangrove code.** Specifications, academic work, published algorithms, and
external implementations may be studied as research. Do not copy, vendor,
mechanically translate, or cosmetically rewrite third-party implementation
code. This applies to the kernel, drivers, filesystems, libc, and libm.
Independent host tools and test oracles, including compilers, QEMU, sanitizers,
MPFR, and MPC, are permitted; they must not become runtime dependencies unless
separately authorized.

Mangrove source is licensed GPL-3.0-or-later under its existing license and
SPDX declarations. Preserve notices. Do not introduce LGPL, permissive
relicensing, static-linking exceptions, or proprietary compatibility
exceptions without explicit user authorization. This governs Mangrove's code
and contributions; it does not claim to control how third parties use it.

## Repository map

| Path | Contents |
| --- | --- |
| `boot/` | UEFI loader, bootstrap, filesystem access, kernel loading, and handoff. Produces `build/EFI/BOOT/BOOTX64.EFI`. |
| `kernel/` | Pith kernel sources, kernel-private headers, x86-64 assembly, drivers, and embedded assets; the linked kernel is `build/Mangrove/pith.elf`. |
| `include/` | Shared ABI and platform headers used across kernel and userspace. `libc/include/mg/` contains public native userspace interfaces; `kernel/include/` is kernel-private. |
| `kernel/src/vfs.c`, `kernel/src/block.c`, `kernel/src/device.c` | VFS, block I/O, registration, and device discovery. Filesystem and storage code is under `kernel/src/storage/`, not a top-level filesystem directory. |
| `libc/` | loam headers, C runtime sources, `crt/crt0.s`, and math sources in `src/math/`; native builds produce static `libc.a` and `libm.a`. |
| `userspace/` | Native applications and services. `shoot/` is the shell; `reed/` is the editor; `common/` holds shared application helpers. |
| `userspace/{sprout,sessiond,logind,logd,networkd,deviced,volumed}/` | Native system services. `sproutctl/` builds the `/bin/sprout` client; `/core/sprout` is the supervisor. |
| `userspace/fstest/`, `userspace/fsstress/` | Native filesystem integration/supervision test app and deterministic model-based stress tool. Test binaries are staged explicitly, not installed in the normal image. |
| `userspace/linker/` | Native userspace linker script and ABI integration. |
| `tests/` | Host production-source fixtures, test oracles, MTS and QEMU guest runners. `tests/filesystems/mfqs/` contains the MFQS model and adapters. |
| `tools/`, `scripts/` | Image population, `tools/mkmgfs.c` formatter, `tools/mgfsck.c` independent MGFS checker, generated-table checks, development-image helpers, and specialist hardware scripts. |
| `Makefile` | Target builds, image targets, focused host groups, and MTS entry points. |
| `documentation/` | Current architecture, filesystem, application, service, build, and qualification documentation. Start at `documentation/README.md`. |
| `build/`, `.mangrove/` | Generated build/test artifacts and persistent developer images. Do not treat either as source. `.mangrove/` is persistent state. |

## Find the contract for a task

Read the linked current-state contract and then inspect its implementation,
callers, and tests. Use the documentation index for other subsystem pages.

| Work area | Start here |
| --- | --- |
| UEFI boot, handoff, kernel initialization | [Boot flow](documentation/architecture/boot.md); `boot/src/`, `kernel/src/main.c`, `kernel/src/init.c`. |
| Physical/virtual memory, address spaces | [Memory layout](documentation/architecture/memory.md); `kernel/src/pmm.c`, `kernel/src/vmm.c`, `kernel/src/address_space.c`; `make test-heap`, `make test-vmm`. |
| Scheduling, interrupts, SMP | [SMP architecture](documentation/architecture/smp.md); `kernel/src/scheduler.c`, `kernel/src/smp.c`, `kernel/src/irq.c`, `kernel/src/mutex.c`, `kernel/src/idt.c`, `kernel/src/idt_asm.s`; inspect lock/interrupt context before changing synchronization. |
| Native processes, syscall ABI, paths, handles, CWD | [Processes and syscalls](documentation/architecture/processes-and-syscalls.md); `kernel/src/process.c`, `kernel/src/syscall.c`, `kernel/src/object.c`; `make test-sync`. |
| IPC and service lifecycle | [IPC and supervision](documentation/architecture/ipc-services.md) and [service overview](documentation/services/README.md); `kernel/src/ipc.c`, `kernel/src/service.c`, service programs under `userspace/`. |
| VFS, filesystem mutation, disk lifecycle and recovery | [Storage lifecycle](documentation/storage/lifecycle.md), then the relevant [MGFS](documentation/filesystem/mgfs/README.md), [FAT32 name](documentation/filesystem/fat32/names.md), [FAT32 replacement](documentation/filesystem/fat32/replacement.md), [FAT32 label](documentation/filesystem/fat32/labels.md), or [exFAT](documentation/filesystem/exfat/README.md) contracts; `kernel/src/vfs.c`, `kernel/src/storage/`; `make test-sync test-history test-storage`. |
| Disk management, formatting, identity, removal | [Storage lifecycle](documentation/storage/lifecycle.md) and [image layout](documentation/storage/image-layout.md); `kernel/src/device.c`, `kernel/src/block.c`, `kernel/src/storage/`; `userspace/volumed/`, `userspace/diskutil/`; `make test-storage`. |
| Networking and DHCP | [Network architecture](documentation/network/architecture.md) and [DHCP lifecycle](documentation/network/dhcp-lease.md); `kernel/src/net/`, `userspace/networkd/`, `userspace/netcfg/`; `make test` and relevant network guest procedures. |
| Authentication, identities, authorization, sessions | [Identity and authorization](documentation/security/identity-and-authorization.md); `kernel/src/identity.c`, `kernel/src/pass.c`, `kernel/src/session.c`; `userspace/logind/`, `userspace/sessiond/`; `make test-password`. |
| Shoot syntax, scripts, interactive editing | [Shoot overview](documentation/shell/README.md), [language](documentation/shell/language.md), [interactive use](documentation/shell/interactive.md), and [history](documentation/shell/history.md); `userspace/shoot/`. See the shell-test note below. |
| Reed editing and safe saving | [Reed guide](documentation/editor/README.md), [safe-file qualification](documentation/development/guest-safe-files.md), and [storage lifecycle](documentation/storage/lifecycle.md); `userspace/reed/`; `make test-reed`, and guest safe-file qualification when affected. |
| Console, terminal controls, text rendering | [Terminal behavior](documentation/architecture/terminal.md); `kernel/src/terminal.c`, `kernel/src/console.c`, `kernel/src/utf8.c`, `libc/src/line_editor.c`; `make test-terminal`. |
| loam, native ABI, startup and linking | [C runtime contract](documentation/architecture/c-runtime.md); `libc/include/`, `libc/src/`, `libc/crt/`, `userspace/linker/`; `make test-libc`, `make test-math`, and target-link probes. |
| Native command or service behavior | [Command reference](documentation/userspace/commands.md), [service overview](documentation/services/README.md), and the relevant IPC/network/storage contract; inspect the command's directory under `userspace/`. |
| QEMU or filesystem qualification failure | [Build and image pipeline](documentation/development/build-and-images.md), [MTS guide](documentation/development/test-suite.md), and relevant guest qualification page; `tests/guest/guest_*.py`, `tests/guest/mts.py`. |

There is no dedicated Make target for Shoot parser/script unit tests. For shell
language or completion behavior, inspect the production path and existing
`tests/userspace/shoot_v1.shoot` fixture and `tests/userspace/directory_enumeration_test.py`
completion coverage; use a real guest workflow when behavior depends on
booted userspace. Extend focused coverage when a change is otherwise untested.
The `make test-guest-safe` runner exercises actual Reed/Shoot workflows.

## Contracts to preserve

- **Original implementation and interfaces:** Do not import familiar Linux,
  BSD, or POSIX architecture by assumption. Mangrove's syscall ABI, object
  model, filesystem rules, and userland service policies are its own contracts.
  Preserve public ABI layouts, syscall numbers, and on-disk formats unless a
  demonstrated defect requires a compatible, fully audited correction.
- **Memory and privilege boundaries:** Validate syscall input and output
  buffers against the current process address space using established helpers.
  Preserve page ownership, DMA lifetime/quarantine, process teardown, handle
  rights, service capabilities, and authenticated authorization. Do not infer
  safety from a user pointer or caller-provided identity.
- **Scheduling and locks:** Identify interrupt, event-worker, and scheduler
  context. Follow established lock ordering and FIFO ownership rules. Never
  block a completion worker on progress that it must itself provide.
  [SMP documentation](documentation/architecture/smp.md) describes current
  synchronization limits; single-vCPU tests do not prove SMP correctness.
- **Filesystem identity and recovery:** Keep mount/registration identity,
  anchored directory locations, stale-handle behavior, mount reservations,
  mutation confidence, and recovery-pin ownership intact. A resolved mutation
  and an uncertain mutation are different states. Do not turn uncertainty into
  empty content, EOF, success, or automatic retry. Follow the storage and
  filesystem contracts before changing shared mutation paths; MGFS, FAT32, and
  exFAT guarantees are format-specific and must not be generalized casually.
- **Publication and durability:** Preserve mutation publication ordering and
  rollback boundaries. Runtime failure containment, confirmed unmount, and
  independent post-stop media checks do not imply global clean shutdown, crash
  atomicity, post-reboot repair, or power-loss durability.
- **Text and C runtime:** Keep path byte limits, UTF-8 validity, filesystem name
  identity, terminal cell width, and grapheme behavior distinct. Preserve
  Reed's conditional stale-save protection. loam provides a practical bounded
  C library, not hosted C17/POSIX completeness. Do not claim numerical or ABI
  guarantees that the implementation and tests do not establish.
- **Failure behavior:** Propagate exact errors and confirmed byte counts.
  Check length arithmetic, signed conversions, and allocation sizes. Do not
  silently truncate, substitute success, hide unsupported cases, or weaken a
  negative control to make a test pass.

## Engineering workflow

1. Establish `git status --short --branch` and inspect the current source and
   relevant documentation. Confirm the actual call path, ownership, and error
   behavior before changing a shared interface.
2. Treat external reviews and suspected defects as hypotheses. Reproduce the
   path or construct a production-code counterexample; distinguish reachable
   defects from unverified risks and optimization suggestions. Add focused
   regression evidence for a demonstrated bug.
3. Make the smallest coherent root-cause change. Update authoritative current
   technical documentation alongside behavior changes. Do not add task plans,
   milestone labels, speculative designs, or development diaries to technical
   docs.
4. Reuse prior qualification only when source, binary, configuration,
   assumptions, and workload still apply. State which evidence was reused and
   which checks ran now. Choose broader checks based on affected interfaces;
   do not repeatedly run expensive unrelated filesystem or guest campaigns.
5. Review the full diff, links, generated artifacts, and `git diff --check`.
   Report exactly what ran, failed, was skipped, or remained incomplete.

## Issue Tracking

- GitHub Issues are the canonical tracker for bugs and actionable unresolved
  engineering problems. Do not maintain a parallel known-issues register in
  repository documentation. Link relevant issues from technical documentation
  when useful.
- Do not create issues for speculative defects or isolated, unclassified test
  incidents without sufficient evidence. Keep confirmed findings distinct
  from hypotheses and incomplete qualification.
- Treat issues as permanent engineering records, not execution reports. Write
  in a concise, neutral, technically precise maintainer voice. Describe
  symptoms, impact, reproduction, evidence, root causes, fixes, verification,
  and technical limits. Avoid conversational narration, generic report
  structure, and repeated explanations. Do not include agent workflow,
  acknowledgements, token usage, GPG verification, local Git status, or push
  status. Reference commits only when technically useful. Do not rely on
  inaccessible local artifacts as sole evidence.
- Keep comments focused on substantive technical findings and changes, not
  routine task completion. Apply these writing standards to issue descriptions,
  comments, updates, and closure messages.
- Search existing open and closed issues before creating one; update the
  canonical issue when new evidence concerns the same defect. Agents may
  investigate and prepare drafts, but must not publish, edit, comment on, or
  close remote issues without explicit task-specific authorization. Such
  authorization does not permit changes to unrelated issues, labels, repository
  settings, or branches.
- Do not close a bug merely because later tests pass. Closure requires an
  identified resolution, appropriate regression evidence, and a clear
  explanation. GitHub operations and Git pushes are separate permissions;
  agents may make requested local GPG-signed commits but must never push unless
  explicitly instructed.

### Build and image commands

The build uses GCC/GNU binutils and MinGW-w64 for UEFI. Linux uses host GNU
ELF tools; macOS uses Homebrew GNU Make (`gmake`), native Homebrew GCC for host
tests, and the `x86_64-elf` cross-toolchain. Image and QEMU dependencies vary
by host; follow [Linux setup](documentation/development/linux.md),
[macOS setup](documentation/development/macos.md), and the [build guide](documentation/development/build-and-images.md).

| Command | Effect |
| --- | --- |
| `make -j4 binaries` | Incrementally build target boot, kernel, libraries, services, and normal native commands. |
| `make -B -j4 binaries` | Force a target rebuild; use when stale objects or a clean ABI/build check matters. |
| `make <program>` (for example `make shoot` or `make reed`) | Build an individual target listed by the Makefile. `make help` lists everyday and specialist targets. |
| `make` / `make image` | Update the persistent developer image under `.mangrove/`; this writes developer state. |
| `make run` | Update and boot that persistent image in QEMU. It is not an isolated test image. |
| `make fresh-image` / `make fresh-run` | Recreate/reset the persistent developer image before use. This destroys its existing filesystem state; do not run casually. |
| `make usb-image` | Rebuild `build/Mangrove/MangroveUSB.img`. This writes a generated image; it does not flash physical media. |
| `make clean` | Remove generated `build/` artifacts; the Makefile preserves `.mangrove/`. |

Prefer the isolated guest runners over the persistent developer image for
qualification. Never flash, partition, or run physical-device scripts without
explicit authorization. Do not inspect an image while QEMU can still write to
it. Preserve failure media and logs needed for diagnosis.

### Tests and evidence

#### Artifact lifecycle

- Build, host-test, and guest-test scratch uses dedicated, identifiable
  workspaces. Prefer ignored, disk-backed `build/` storage by default and
  honor an explicitly configured `TMPDIR`.
- Successful workflows remove their own disposable files, directories,
  sockets, firmware variables, and guest images after owned processes exit and
  required post-run checks complete. Safe cleanup also runs on ordinary
  failures and interruptions.
- Failed workflows retain bounded summaries, logs, screenshots, and other
  useful diagnostics under an identifiable `build/` evidence directory.
  Retain large media only when needed to diagnose the failure or explicitly
  requested.
- Explicit `--output` paths must be new and are retained for the caller. Never
  recursively clean an existing or user-provided directory.
- Cleanup is limited to artifacts with verified runner ownership. Never remove
  arbitrary `/tmp` contents, another process's workspace, unknown files, or
  persistent `.mangrove/` images. Give concurrent runs unique workspaces.
- Before large tests, check free space on the selected filesystem. If `/tmp` is
  full, use the repository-owned disk-backed workspace; do not delete unrelated
  files. Report remaining storage limits before qualification.
- Bound and reap owned child processes before deleting their files. Use
  interruption cleanup where safe; leave a marked, inspectable workspace when
  a process may still be using it. Orphans must be identifiable without
  treating unknown files as disposable.
- When changing artifact infrastructure, verify actual cleanup after success,
  failure, and interruption. Do not claim cleanup is complete without checking
  the resulting workspace and evidence paths.

| Command | Coverage |
| --- | --- |
| `make test` | MTS host profile, including subsystem fixtures, sanitizers where configured, MFQS, and host `fsstress` tests. |
| `make test-guest` | Standard isolated QEMU guest profile; Linux defaults to KVM, with TCG selected explicitly using `MTS_ACCEL=tcg`; other hosts retain TCG. |
| `make test-all` | Comprehensive host and guest profile; expensive (about 140 guest boots in the current profile). Run only when requested or warranted by impact. |
| `make test-mts` | MTS orchestration/evidence negative controls; not the host or guest product suite. |
| Focused examples: `make test-sync`, `make test-storage`, `make test-history`, `make test-reed`, `make test-terminal`, `make test-libc`, `make test-math`, `make test-mfqs`, `make test-fsstress` | Run the group relevant to the change; consult the Makefile and [MTS inventory](documentation/development/test-suite.md) for actual scope. Some groups are expensive. |
| `make test-guest-mgfs`, `make test-guest-safe`, `make test-guest-secondary`, `make test-guest-hotplug`, `make test-guest-stress` | Focused but costly native QEMU qualifications. They use disposable images; see the relevant guest guides before running. |

Use `MTS_OUTPUT="$PWD/build/mts-<purpose>"` to retain aggregate evidence in
disk-backed build storage. The MTS guide records exact profiles, output
artifacts, deadlines, and PASS/FAIL/SKIPPED/INCOMPLETE semantics. `make test`
is host-side evidence; only guest runners execute a booted native process.
QEMU KVM and TCG do not establish physical-hardware, SMP, or power-loss guarantees.
A second boot verifies only the persistence workload a runner exercised; it
does not establish general crash recovery. Keep host fixtures, sanitizers,
target ABI/link checks, guest process completion, and independent image
checking as distinct evidence.

On Linux, native QEMU guest runners and Make targets default to KVM. TCG is an
explicit opt-in through `--accel tcg` or the target's `*_ACCEL=tcg` Make
variable; never silently fall back. If KVM is unavailable, fail with an
actionable `/dev/kvm` diagnostic. When Codex's ordinary sandbox hides the
device, use its approved elevated execution path; do not weaken host device
permissions. Other hosts retain their existing accelerator default.

## Documentation and Git

Before planning a major feature or new stage, read the local `ROADMAP.md` and
use its milestones, dependencies, and completion criteria when scoping the
work. Update it when stage status, priorities, milestones, or architectural
decisions materially change. Preserve the full development horizon through
the native desktop stage; keep statuses truthful, proposals clearly
provisional, and the roadmap concise and navigable. `ROADMAP.md` is a
local-only development file: keep it untracked and excluded through
`.git/info/exclude`; never stage or publish it. `AGENTS.md` is durable,
versioned contributor guidance; update it when repository-wide workflow
policy changes materially.

Use the [documentation index](documentation/README.md) as the full catalog.
Document current implemented behavior, interfaces, constraints, guarantees,
limitations, and developer procedures. Update affected technical docs in the
same change as behavior. Unsupported behavior may be documented to explain an
existing interface. Do not duplicate a contract when a link to its owner is
clearer. Keep links and source references accurate. Use the names Mangrove,
Pith, Shoot, Reed, and lowercase loam.

Routine engineering decisions should be made autonomously from requirements,
source, tests, and established principles. Do not pause for ordinary
implementation choices. Ask when a genuine product decision is unresolved or
an action is destructive, irreversible, externally published, or outside
authorization. Do not delete user data or existing qualification artifacts to
free space without authorization.

Before a commit, review the full diff, run applicable focused and broader
verification, check docs, and run `git diff --check`. All commits created by
development agents must follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/):
`<type>[optional scope][!]: <description>`. Use `feat` for features and `fix`
for bug fixes; other suitable types include `docs`, `test`, `refactor`, `perf`,
`build`, `ci`, `chore`, and `style`. Prefer lowercase types, meaningful scopes,
and concise imperative descriptions. Keep commits focused and logically
separated. Add a body when the rationale, implementation, or compatibility
implications need explanation. Mark breaking changes with `!` and/or a
`BREAKING CHANGE:` footer as specified. Examples: `fix(scheduler): prevent
stale CPU-local thread lookup`, `test(reed): verify native redo behavior`,
`docs(vfs): clarify safe-save guarantees`. Create GPG-signed commits when the
task calls for completion; verify signatures and the final worktree state.
Never push unless explicitly requested. Permission to commit is not permission
to publish.
