# Mangrove Test Suite (MTS)

Host and guest tests are grouped by subsystem under `tests/`: `kernel/`,
`memory/`, `network/`, `storage/`, `terminal/`, `userspace/`, and `guest/`.
Filesystem coverage is split into `filesystems/{mgfs,fat32,exfat,vfs,stress,mfqs}/`;
reusable harness code lives in `shared/`. Public Make targets and their suite
membership are unchanged by this layout.

MTS is the umbrella for Mangrove's existing host regressions, native test
applications, model-based filesystem qualification, QEMU acceptance runners,
independent media checkers and specialist diagnostics. It organizes their
execution; each existing tool retains its own model, assertions and evidence.

## Routine and comprehensive commands

On Debian use GNU `make`; on macOS use Homebrew GNU `gmake`. Host portability
and toolchain details remain in [focused tests](tests.md). The aggregate guest
profiles currently require the Debian runner environment: several existing
media checks use `/usr/sbin` tool paths. Choosing firmware paths alone does not
establish macOS guest support.

| Command | Required work | Cost and exclusions |
| --- | --- | --- |
| `make test` | Existing host groups, in their established order, plus inexpensive MTS orchestration controls. Includes focused libc bounds regressions, MFQS fixed/quick/extended/replay/negative controls and host fsstress tests. | No guest boots. This is the full current host profile, not a reduced smoke profile. |
| `make test-guest` | Isolated forced UTC-trusted build; one root-MGFS create/second-boot verify/wrong-content campaign; one complete secondary-volume campaign per MGFS/FAT32/exFAT. | 27 boots, including secondary read-only, authorization, missing service/device/label and wrong-content controls. Not a quick command. No randomized guest stress, application UI, hotplug or injected recovery faults. |
| `make test-all` | Host suite; isolated forced build; comprehensive guest groups listed below. | Roughly 140 boots, potentially hours under TCG. Explicitly expensive; hardware/manual tools remain excluded. |

All aggregate groups run serially, including their QEMU campaigns. A guest
build uses `-B -j4`, `RTC_UTC_TRUST=1` and existing `binaries fstest fsstress
fsstress-host mkmgfs mgfsck` targets. Guests explicitly use a UTC RTC. No
canonical image target is invoked. Linux guest profiles default to KVM;
select TCG explicitly with `MTS_ACCEL=tcg` or `--accel tcg`. Other hosts retain
their existing TCG default. If KVM is selected but QEMU lacks KVM support or
`/dev/kvm` cannot be opened, the runner fails before guest setup with an
actionable diagnostic and does not fall back to TCG. In Codex, use the approved
elevated execution path when the ordinary sandbox hides `/dev/kvm`; do not
change host device permissions.
Individual Make targets accept their corresponding `*_ACCEL` variable, and
direct Python runners accept `--accel tcg` or `--accel kvm`.

```sh
ASAN_OPTIONS=detect_leaks=0 make test
make test-guest MTS_OUTPUT="$PWD/build/mts-guest-example"
make test-guest MTS_ACCEL=tcg       # Explicit TCG portability/debug run
make test-all MTS_OUTPUT="$PWD/build/mts-comprehensive-example"
make test-mts                    # Cheap orchestration controls only
```

`MTS_OUTPUT` must name a new directory and explicitly retains the complete
aggregate evidence. Without it, MTS uses a unique temporary workspace under
`build/test-workspaces`; completed output is removed after the final summary
is recorded to stdout. Use `MTS_RETAIN_ARTIFACTS=1` to keep the complete run
under `build/test-retained`. Aggregate runs set `TMPDIR` to their private
`tmp` subdirectory. Free-space admission is 8 GiB for host
or standard guest, 64 GiB for comprehensive execution. These are conservative
admission budgets, not measured peak usage or guarantees against other writers
exhausting the disk. Individual runners also enforce their existing budgets.
Do not run independent campaigns concurrently against the same build or media.

The aggregate CLI is `python3 tests/guest/mts.py host|guest|all`. It also accepts
`--make`, `--output`, `--retain-artifacts`, `--accel`, `--ovmf-code` and
`--ovmf-vars`. Make supplies its existing `OVMF_CODE_SOURCE` and
`OVMF_VARS_SOURCE`. Required host tools
include GNU compilers/binutils and Python; guest/media groups additionally need
QEMU x86-64, OVMF, `sgdisk`, dosfstools, mtools and exfatprogs. On Debian media
tools may live under `/usr/sbin`, which MTS adds to its child search path.
Makefile toolchain admission still happens before the orchestration starts;
a parse-time toolchain failure has a Make diagnostic and no MTS report.

## Inventory and individual entry points

All test programs and host tooling below are development-only unless identified
as normal OS applications. `fstest`, `fsstress`, `nettest` and `hello` have
explicit build targets and are absent from the normal payload manifest.

| Tool / location | Execution and responsibility | Entry point |
| --- | --- | --- |
| `userspace/fstest/{main,volume,safe,stress}.c` | Native Ring 3 fixed integration scenarios and supervision: syscall results, volume authority/lifetimes, recovery inspection, Reed/Shoot workflows, child completion. Its stress module supervises a separate fsstress child. | `make fstest`; installed explicitly by guest runners. |
| `userspace/fsstress/` | Portable C17 planner/model/engine with native and POSIX adapters. Seeded lifecycle/content/namespace checking, compact traces, replay and persisted verification. Public native calls in the guest; ordinary host filesystem calls on Debian. | `make fsstress fsstress-host`, `make test-fsstress`; [stress guide](native-filesystem-stress.md). |
| `tests/filesystems/mfqs/` and `tests/filesystems/mfqs/mfqs_test.py` | MFQS: independent Python expected-content/namespace model, generated and fixed traces, evidence witnesses and complete production VFS/MGFS/FAT32/exFAT units with controlled host dependencies. Includes real teardown/remount and historical bad-driver controls, not a guest boot. | `make test-mfqs`; [MFQS guide](filesystem-qualification.md). |
| `tests/filesystems/stress/` and `tests/shared/fsstress_oracle.py` | Host C known-answer/model and actual POSIX-adapter tests, controlled transport failures, separate Python oracle and strict trace/result validation. | `make test-fsstress`. |
| `tests/{kernel,memory,network,storage,filesystems,userspace,terminal}/` and related C fixtures | Focused production-path host regressions, often ASan/UBSan and deterministic failure/concurrency injection. Some compile complete production units; others extract functions with controlled dependencies. | Subsystem targets below; [fixture boundaries](tests.md). |
| `tests/guest/*_evidence_test.py`, `tests/guest/mts_test.py` | Controlled negative tests of qualification infrastructure: reject malformed/missing evidence, wrong results, timeouts, omitted work or synthetic completion. These tests themselves are not guest execution. | `test-dev-tools`, `test-fsstress`, `test-mts`. |
| `tests/userspace/libc_string_test.py`, `tests/userspace/runtime_foundations_test.py`, `tests/userspace/runtime_stdio_test.py`, `tests/userspace/libm_accuracy_test.py`, `tests/userspace/libm_complex_accuracy_test.py`, `tests/userspace/libm_complex_probe.c`, `tests/userspace/tgmath_probe.c`, `tests/userspace/runtime_api_link_probe.c` and related probes | Host production-libc checks with private symbols: 49,716 bounded `strncat` cases and historical ASan control, string API references, strict header/ABI probes, independent format/conversion references, ASCII ctype, aligned allocation, allocator failure, errno, streams and termination callbacks. The original Mangrove math group checks 63,235 deterministic real results against MPFR and 27,689 complex results against MPC/MPFR, then repeats production sources under ASan/UBSan; type-generic dispatch plus unsupported-symbol, corrupted-result/quotient, per-family coverage and incomplete-record controls are included. A target-ABI executable links the implemented standard API surface against the actual libc and libm archives. The MPFR comparison requires x86-64 host execution and the MPFR and MPC shared libraries. Development-only; not a hosted C17 certification suite. | `make test-libc` (also in `make test`), `make test-math`. |
| `tools/mgfsck.c` | Independent, read-only host MGFS format, checksum, extent/allocation and namespace checker. Does not execute the runtime driver or repair media. | `make mgfsck`; run `build/mgfsck IMAGE`. |
| dosfstools, mtools, exfatprogs | External host `fsck.fat -n`, `mdir`/`mcopy`/`mtype`, `fsck.exfat -n`; independent formatting/extraction/checking. Not repository implementations or OS payloads. | Existing fixtures and guest runners. |
| `tools/mkmgfs.c`, `populate_mgfs.py`, `update_mgfs.py`, `copy_partition.py`, `image_payloads.py`; `tests/filesystems/mfqs/{images,mgfs_images}.py`, `tests/filesystems/exfat/exfat_names_image.py` | Host image construction/import/poisoning and independent Record/extent inspection. Provisioning support rather than runtime filesystem replacements. `guest_secondary_test.py` also contains the bounded read-only independent exFAT reader reused by guest runners. | `make mkmgfs`; invoked by existing image/qualification tools. |
| `tools/check_mgfs_version.py`, `generate_{exfat_upcase,fat32_uppercase}.py --check` | Host retained-image format admission and reproducibility/provenance checks for generated filename tables. Not complete filesystem checkers. | Existing image and namespace-test paths. |
| `tests/filesystems/fat32/fat_metadata_profile.py`, `tests/filesystems/mgfs/mgfs_stage_profile.py` | Optional host metadata-I/O, allocation and CPU measurements using temporary instrumented production sources and fixtures; support revision comparisons. | Direct Python invocation; see [FAT measurements](fat-metadata-performance.md) and [MGFS measurements](mgfs-traversal-performance.md). |
| `userspace/nettest/main.c`, `userspace/hello/main.c` | Opt-in native DNS/ICMP/UDP/TCP diagnostic and libc/allocator/anonymous-memory/syscall self-test application. Network testing requires real service/echo prerequisites; no automated general networking acceptance profile. | `make nettest hello`; manual guest use. |
| `tests/network/e1000_context_test.c`, `tests/network/e1000_tx_concurrency_test.c` | Production e1000 RX-context interleaving and multi-worker TX descriptor-ownership regressions. The spinlock implementation is a host adapter; these tests do not qualify hardware interrupts, DMA, teardown, or SMP device lifetime. | `make test-e1000`; included by `make test-net`. |
| `tests/network/tcp_stream_test.c` | Production TCP core with a deterministic peer: ACK-gated multi-segment send, retransmission, duplicate receive suppression, advertised-window updates, FIN/TIME_WAIT, reset, timeout, interruption and device-generation invalidation. The fixture verifies the protocol state machine, not wire interoperability or physical NIC behavior. | `make test-tcp`; included by `make test-net`. |
| `tests/network/icmp_state_test.c`, `tests/network/net_user_table_test.c` | ICMP request ownership and process-side echo admission, including sequence-scoped cancellation after timeout or device-generation loss so later requests can proceed. | `make test-net`. |
| `tests/network/ping_destination_test.c` | Production `ping` and libc network wrappers with a syscall adapter: numeric and DNS destinations (including digit-leading names), resolver failures, repeated attempts, and timeouts preserve destination bytes and handle cleanup. | `make test-net`. |
| `scripts/stress_kvm_boot.sh` | Specialist host KVM cold-boot probe of an existing USB image, retaining serial/framebuffer evidence. Uses its own image selection and boot interval; not native filesystem acceptance. | Explicit manual invocation only. |
| `scripts/physical_sandisk_test.sh`, `physical_sandisk_usbctl.sh` | Hardware-gated USB passthrough and narrowly identified physical-device bind/unbind helper. Requires separately authorized hardware and setup. | Never invoked by any MTS aggregate. |
| `scripts/update_dev_image.sh` and `tests/guest/dev_image_guard_test.py` | Normal image updater plus host guard regressions on disposable fixtures. The updater itself is not a test runner. | `make test-dev-tools`; normal image operations remain separate. |

Normal applications such as Reed, Shoot, device/volume services, `ping`,
`resolve`, `fetch`, `lsdsk`, `lsusb`, `diskutil`, `logv`, `time`, `mem` and `tmon` provide
real application/diagnostic surfaces. Guest tests exercise the ordinary
applications and services; MTS does not install alternative implementations.

The host groups remain `test-time`, `test-terminal`, `test-clipboard`, `test-reed`, `test-shoot`,
`test-mgfs-version`, `test-password`, `test-random`, `test-vmm`, `test-idt`, `test-pmm`, `test-heap`,
`test-net`, `test-fsstress`,
`test-history`, `test-mfqs`, `test-sync`, `test-storage`, `test-dev-tools` and
`test-mts`. `test-history` includes MGFS allocation/namespace/recovery and
history regressions. `test-sync` includes VFS/syscall/process lifetime,
anchored locations, checked reads/lookup, recovery inspection and locks. `test-storage`
includes FAT32/exFAT mutations, labels, ownership, identity, cache/GPT/format,
USB/BOT/xHCI/AHCI and removal interleavings. `test-launch` remains the older
compatibility alias for VMM, heap and history tests.

Fault injection is part of the existing fixtures and runners, not a separate
general engine. Host fixtures control write/flush/read/allocation failures,
torn metadata and specific thread interleavings; `host_barrier.h`,
`host_malloc_wrap.h` and `host_build.py` supply shared test support. Guest
lifetime/stress runners coordinate QEMU blkdebug and QMP with actual native
readiness and recovery-inspection witnesses. MFQS and fsstress are the seeded
randomized tools; malformed-media and historical controls remain in focused
tests and their respective oracles.

## Guest groups and evidence

The existing `tests/guest/guest_mgfs_test.py` supplies shared isolated boot, QMP,
framebuffer observation and native transcript primitives. Other runners reuse
it rather than installing another boot or filesystem path.
The root-MGFS runner accepts `--cpus 1|2` (default 1); two vCPUs exercise AP
startup and its per-CPU protection/descriptor setup, but do not constitute
comprehensive SMP qualification. `--no-nic` omits the QEMU virtual NIC while
leaving the production kernel unchanged; it can isolate storage and scheduler
workloads from network-device behavior and does not qualify networking.
After shutdown the runner reads `/sys/logs/boot.log` from the guest image and
requires the requested number of CPUs in Pith's final online record. This
proves AP startup for that run, not concurrent heap stress or general SMP
correctness.

| Runner / existing Make target | Comprehensive selection |
| --- | --- |
| `guest_mgfs_test.py --power-history-smoke` / `test-guest-shutdown` | Three bounded two-vCPU boots over one disposable MGFS image: active-shell shutdown, QEMU guest-reset request for reboot, then in-guest history readback and clean shutdown. Each stopped image receives independent MGFS checking. This does not establish a same-process firmware reboot or physical power-loss recovery. |
| `guest_logout_test.py` / `test-guest-logout` | One two-vCPU manual-login workflow exercises `exit`, `logout`, empty-prompt Ctrl+D, and nonzero shell exit across repeated sessions. It scrolls a marker out of view and checks QMP framebuffer captures contain only a fresh login prompt after each session. The disposable guest is stopped after the visual checks; this mode does not qualify storage durability. The terminal host fixture separately verifies that all retained cell-ring rows are cleared. |
| `guest_mgfs_test.py --network-ping-smoke` | A disposable two-vCPU Q35 guest verifies DHCP, then uses production `fstest network` to time out an echo to TEST-NET-3 and immediately receive a gateway echo on the same ICMP handle. It also checks 16 more echoes, the `ping` command, DNS, pathless `fetch http://example.com` through Shoot, and stopped-image MGFS integrity with the `make run` e1000 MAC. |
| `guest_mgfs_test.py` / `test-guest-mgfs` | Two root-MGFS create/second-boot verify/wrong-content campaigns; one additional relative-executable-launch campaign. |
| `guest_mgfs_test.py --random-smoke` / `test-guest-random` | One disposable two-vCPU boot exercises random readiness, a 2 MiB userspace request, four child-process requests, and independent stopped-image MGFS checking. This is API/runtime evidence, not entropy-source or cryptographic certification. |
| `guest_secondary_test.py` / `test-guest-secondary` | Two complete persistence/authority/read-only/negative-control campaigns per MGFS/FAT32/exFAT. |
| Same runner / `test-guest-hotplug` | Two MGFS idle campaigns plus wrong-replacement control; two retained-reference campaigns; two CWD/retained-reference campaigns per filesystem. |
| `guest_safe_test.py` / `test-guest-safe` | Two Reed/Shoot/native safe-file campaigns per filesystem, with read-only, wrong-content and replacement-flush controls. |
| `guest_safe_test.py --reed-no-conflict` / `test-guest-reed` | One two-vCPU MGFS application run on the production hybrid kernel; USB HID drives Shoot command completion, cursor insertion, long wrapped redirection, history draft restoration, command/path completion, output and Reed-file readback; Shoot launches a per-run Reed script, which exercises edit, undo, a Shift+U redo attempt, save, Save As and terminal restoration before shell exit and independent MGFS checking. The integrated run reads saved text through Shoot but does not relaunch Reed on the copy. Native redo is PASS only when the rendered edit is observed; otherwise it is reported INCOMPLETE and the remaining workflow continues. Native stale-save conflict remains outside this mode. |
| `guest_lifetime_test.py` / direct CLI; replacement subset in `test-guest-safe` | MGFS online data/metadata-read errors, write/flush uncertainty and active-I/O removal feasibility; no-fault data/metadata-read/write/flush controls; replacement staging write/flush, publication, cleanup and missing-fault controls. |
| `guest_stress_test.py` / `test-guest-stress` | All three filesystems, fixed zero plus generated recorded seed, 96 operations, two-boot persistence/replay, wrong-content controls and six additional MGFS model/status/prefix/incomplete/write/flush controls. |

Aggregate profiles invoke these existing Python runners directly after one
shared isolated build, avoiding repeated forced builds. Individual Make targets
retain their campaign defaults. Their output variables default empty, so guest
images use temporary workspaces; setting a matching `*_OUTPUT` variable keeps
the caller-selected output tree.
Comprehensive selection is explicit in `tests/guest/mts.py`; standard guest selection
does not silently include the comprehensive-only rows.

Runners require actual native child completion and operation/status/identity
evidence. Only after QEMU has exited do they extract media and use independent
MGFS checking/Record decoding, FAT32 checking/mtools, or exFAT checking/bounded
stream decoding. Unsupported reader layouts fail qualification. Confirmed
secondary unmount and persisted cold-boot readback are distinct from system
shutdown. The production poweroff/reboot path requests bounded application and
leaf-service cleanup, then synchronizes mounted filesystems before the ACPI
transition. These guest campaigns do not exercise those power transitions or
establish hardware power-loss recovery. Lifetime runs that report unresolved
media uncertainty explicitly preserve that selected image without automatic
repair or reboot recovery.

The October 22 integrated run exposed a scheduler ownership race before
preemption, now covered by `scheduler_current_test.py`; the technical record
is [Issue #1](https://github.com/oyer0/mangrove/issues/1). On corrected source,
the production-default two-vCPU TCG create/verify/read-only workflow completed
with child status zero and clean independent MGFS checks in every phase. An
indexed-only one-vCPU campaign also completed create, verify, stdio, time and
math phases. See [application qualification](guest-safe-files.md) for exact
interactive witnesses and the preserved interrupted-write observation. The
full interactive report remains INCOMPLETE for unobserved native Reed redo;
stale-save conflict still requires a supported coordination channel. These
results do not constitute Stage 5 completion or hardware qualification.

## Reporting, failures and bounds

Each aggregate writes `summary.json`, one `<group>.log`, and existing runner
evidence directories below its owned output while running. The summary records profile,
revision/working-tree provenance, command, whether it actually started,
duration, return code, result/reason
and report path. It is published before execution and atomically updated after
each group, so missing final completion remains INCOMPLETE. Existing runner
reports, native transcripts, traces, checkers, screenshots and media remain the
authoritative detailed evidence while the run is active. MTS checks zero command exit and the requested
runner report status, campaign count/backend membership and completed phases;
it delegates syscall/content/oracle validation to those unmodified runners.

- **PASS**: all required profile groups completed. Guest command exit alone or
  a printed PASS does not suffice without a well-formed existing runner report.
- **FAIL**: an executed command/test failed. A predeclared lifetime negative
  control may have runner status EXPECTED_FAILURE only after its own precise
  assertions pass and its command exits zero; MTS records that original status.
- **SKIPPED**: a group is explicitly outside the selected profile, manual or
  hardware-gated. The summary states why. Optional exclusions do not qualify
  their capabilities and do not invalidate the selected profile.
- **INCOMPLETE**: required work lacked prerequisites, capacity, time,
  completion evidence or was interrupted. A required skip cannot become PASS.

The Python aggregate exits 0 for PASS, 1 for FAIL and 2 for INCOMPLETE. GNU
Make maps a failed recipe to its own nonzero exit; use the printed final
summary to distinguish failure from incomplete qualification. A temporary
MTS output is removed after its result is recorded to stdout. On failure,
bounded reports and concise logs are copied to a unique `build/test-failures`
directory; generated images are discarded unless a runner marks their exact
contents as necessary.

Required missing-tool/skip notices in host logs also prevent aggregate PASS.
MFQS's existing `INCOMPLETE broader qualification:` line describes future
coverage outside its supported host profile; it is retained as a limitation,
not mistaken for a missing required backend. Actual missing MFQS backend
evidence remains INCOMPLETE. Assertions and sanitizer failures remain failures.

MTS continues independent groups after ordinary failures. A failed guest build
prevents dependent guest execution; interruption prevents starting further
required work. Deadlines are 30 minutes per ordinary host group, 60 minutes for
MFQS/storage, 15 minutes for the build and two hours per guest group. Existing
shorter per-boot/native deadlines still apply. A monitored 64 MiB group-log
limit stops excessive output; a producer can emit beyond it between checks,
and oversized logs never qualify. Handled deadlines and interruptions terminate
and reap owned QEMU process sessions before cleanup. Independent post-run image
checks complete before a successful image is removed. SIGKILL or host failure
may prevent cleanup; confirm no owned QEMU process remains before inspecting
media and treat such a run as incomplete. Explicit output paths are never
pruned. Pass `--retain-artifacts` or `MTS_RETAIN_ARTIFACTS=1` to keep complete
generated artifacts for debugging. Default failed runs keep bounded diagnostic
bundles in `build/test-failures` (at most 12 owned bundles and 1 GiB total);
each bundle limits copied logs/reports to 32 MiB and may retain at most two
explicitly selected media files, capped at 256 MiB total. Older bundles are
pruned only when they carry the runner's ownership marker. Opt-in retained
workspaces and caller-selected output directories are not rotated or pruned.

ASan leak detection defaults to disabled (`detect_leaks=0`) unless the caller
explicitly supplies `ASAN_OPTIONS`; the summary records the value. Host
controlled dependencies, single-vCPU QEMU evidence, real hardware, SMP, macOS,
power-loss durability, persistent repair, Unicode/large-volume scaling and
exhaustive concurrent/fault schedules remain separate evidence boundaries.
An MTS PASS is scoped to the selected profile and these documented contracts.
