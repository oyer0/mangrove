# Native MGFS guest persistence qualification

`make test-guest-mgfs` explicitly builds an isolated system and runs two fresh
QEMU campaigns. It never invokes `make run`, opens `.mangrove/`, or installs
`fstest` in the normal image manifest. Dependencies are QEMU x86-64, OVMF,
`sgdisk`, `mkfs.fat`, mtools, and the normal GNU build tools. Debian defaults
use firmware under `/usr/share/OVMF/`. Run from the repository root:

```sh
make test-guest-mgfs \
  GUEST_MGFS_BUILD=/tmp/mangrove-guest-build \
  GUEST_MGFS_OUTPUT=/tmp/mangrove-guest-acceptance \
  GUEST_MGFS_ACCEL=tcg
```

The output directory must not exist. KVM is an explicit alternative when
available; TCG needs no `/dev/kvm`. For firmware overrides, one campaign, or
an exact run identifier, invoke `tests/guest/guest_mgfs_test.py --help` after building
`binaries fstest mkmgfs mgfsck` with the selected `BUILD_DIR`. A direct runner
build must set `RTC_UTC_TRUST=1`, for example:

```sh
make -B -j4 BUILD_DIR=build/guest-mgfs RTC_UTC_TRUST=1 \
  binaries fstest mkmgfs mgfsck
```

The runner's QEMU `-rtc base=utc,clock=vm` option does not set the kernel's
compile-time RTC trust policy. Without the trusted build, realtime is
intentionally unavailable and `fstest time` exits with status 43.

The focused runner can also qualify the e1000 receive path on two vCPUs:

```sh
make -B -j4 BUILD_DIR=build/network-guest RTC_UTC_TRUST=1 \
  binaries fstest mkmgfs mgfsck
python3 tests/guest/guest_mgfs_test.py --build-dir build/network-guest \
  --output build/network-guest-rx --accel tcg --cpus 2 \
  --network-receive-smoke --repetitions 2 --negative-control
```

This mode installs a test-only static address (`10.0.2.15/24`) and QEMU
user-network gateway (`10.0.2.2`) in the disposable image. It boots the normal
network services and, in a separate boot phase from the clock test, runs a
native 16-echo ICMP burst through the public network API. The first echo is
sent once and must resolve its neighbor within the API timeout; every echo
opens and closes an ICMP kernel object. The test requires the gateway address
and reply length, at least 16 RX/TX counter increments, and a persisted
`netinfo` RX count. This exercises packet reception through the guest driver
and network stack; it does not qualify DHCP, general network interoperability,
device removal, per-CPU interrupt distribution, or physical-NIC performance.

To exercise the packaged `/bin/ping` command with the normal DHCP configuration
and QEMU's two-vCPU e1000 device, use a disposable one-boot workflow:

```sh
python3 tests/guest/guest_mgfs_test.py --build-dir build/network-guest \
  --output build/network-ping-smoke --accel kvm --cpus 2 \
  --network-ping-smoke
```

The workflow waits for DHCP, pings `10.0.2.2` four times, runs the production
16-request ICMP object-lifecycle check, records interface, route, neighbor,
and DNS state, then runs `fetch http://example.com` from Shoot and verifies
that the downloaded `index.html` is present in the stopped image. The runner
stops QEMU after the visible completion marker and independently checks the
stopped MGFS image. It requires all gateway echo replies, a successful fetch,
successful repeated object creation/close, and the learned gateway neighbor.
This qualifies that exact QEMU user-network setup, not external ICMP service or
physical hardware.

To qualify the production `fetch` HTTPS integration against the same Q35/OVMF,
DHCP, two-vCPU guest setup, use the dedicated disposable workflow:

```sh
make test-guest-fetch-https \
  GUEST_FETCH_HTTPS_BUILD=/tmp/mangrove-fetch-https-build \
  GUEST_FETCH_HTTPS_OUTPUT=/tmp/mangrove-fetch-https-run \
  GUEST_FETCH_HTTPS_ACCEL=kvm
```

The target builds with `RTC_UTC_TRUST=1`, waits for DHCP, downloads the
Example Domain page through both HTTP and authenticated TLS 1.3 HTTPS, then
invokes bare `fetch 5x40.org` to exercise its HTTP-to-HTTPS redirect and the
site's cross-signed certificate chain. It checks each saved body after QEMU
has stopped and independently runs `mgfsck` over the resulting root image.
The output directory must be new. This checks the current remote chains and
CA snapshot in the QEMU environment; it is not physical-hardware qualification
or a TLS security certification.

## Executed path and evidence

The runner creates a disposable GPT image with a 64 MiB FAT32 ESP and a
64 MiB MGFS root. It installs normal system payloads, test-only `/bin/fstest`,
and phase-specific Shoot scripts through existing host image-builder
primitives. Network mode adds its own script and guest boot.
OVMF variables are private to each boot. The standard runner uses Q35, one
CPU, 512 MiB RAM, xHCI USB mass storage, a USB keyboard, and user-mode e1000
networking. Normal kernel initialization, services, developer autologin, and
Shoot remain active.

Before each QEMU launch, the runner synchronizes the completed boot disk,
private firmware-variable copy, and any secondary/replacement images with host
`fsync`. Closing their writers alone leaves initial image contents dirty in
the host cache; the guest's first SCSI cache synchronization would otherwise
have to commit that preparation too. Under concurrent image activity this
has exceeded Pith's device deadline and retired a healthy root disk. Sync
errors prevent launch; non-regular backing is rejected. The operation does
not change image bytes or replace independent filesystem checks.
`image-sync.json` and the returned `host_image_sync` fields record byte counts
and preparation durations separately from the guest's timed phase. This does
not extend kernel timeouts, bypass flushes, retry I/O, or establish physical
hardware/power-loss guarantees. See [issue #2](https://github.com/oyer0/mangrove/issues/2)
for the captured failure and controlled comparisons.

The QEMU debug console is retained as bounded `debugcon.log` evidence, in
addition to serial and framebuffer output. Kernel panic entry emits
`PITH_PANIC <reason>` before terminal operations. The runner recognizes this
marker while polling and after QEMU exits, attempts a final framebuffer
capture, and preserves the original reason if capture fails. It also stops
on an explicit PID1 construction failure instead of waiting for a shell
prompt until timeout. Owned QEMU processes are reaped before artifact cleanup;
failed runs retain small diagnostics under the normal artifact policy.
`make test-dev-tools` exercises these failure paths with synthetic processes;
this is runner evidence, not a simulated successful guest or a scheduler fix.

QMP stdio captures framebuffer evidence and injects a command only after the
existing console prompt is recognized from its exact font glyphs. Shoot
launches a real Ring 3 `fstest` child, redirects its stdout to MGFS, waits for
its exit status, closes its process handle, and writes a completion record
only after status zero. The child uses native file/directory/metadata syscalls;
there is no kernel-internal substitute for those operations.

For a focused native Shoot language run, build with trusted UTC enabled and
use `--shoot-v1`. This mode boots one disposable image, enters the shell launch
command through the emulated USB keyboard, and runs the version-controlled
`tests/userspace/shoot_v1.shoot` script as a Ring 3 child. The runner checks the exact
script report, a completion marker written only after the child succeeds, the
requested CPU startup witness, and the stopped root image with `mgfsck` before
removing its temporary workspace:

```sh
make -B -j4 BUILD_DIR=build/shoot-guest RTC_UTC_TRUST=1 \
  binaries fstest mkmgfs mgfsck
python3 tests/guest/guest_mgfs_test.py --build-dir build/shoot-guest \
  --shoot-v1 --no-nic --cpus 2 --accel tcg --repetitions 1
```

The host `test-shoot` fixture executes the production parser and interpreter
with controlled process/filesystem adapters; this guest mode adds native
process, filesystem, keyboard-launch, and image-integrity coverage.

## Shutdown and reboot history smoke

`make test-guest-shutdown` runs a bounded two-vCPU QEMU lifecycle using one
disposable MGFS image. A live Shoot session launches a script that requests
ACPI shutdown; the stopped image is checked independently for the session's
saved history. The same image is then booted for `/bin/reboot`; QEMU's
`-no-reboot` mode captures the guest-reset event and stops after the reset
request. A following boot copies the saved history from the guest filesystem
and shuts down cleanly. The runner compares that readback with the persisted
file and validates the exact history records. This exercises native cleanup,
the shutdown/reboot API and image persistence under QEMU; it is not a physical
power-loss or bare-metal reboot-recovery test. Linux defaults to KVM; set
`GUEST_SHUTDOWN_ACCEL=tcg` for explicit TCG execution.

`fstest create <run>` requires absence, creates `/temp/mgfs-<run>.bin`, and
writes exactly 8,193 deterministic binary bytes. The independently specified
pattern uses the 16-character run identifier and byte position. It checks exact
counts, seek/read at sector and block boundaries, EOF, close/reopen, all bytes,
file type/length/identity, and exactly one matching enumeration entry. A bounded
native process snapshot also requires a live `/core/deviced` system service,
covering its normal startup through the native application loader. The workload
also checks the 65,535-byte pathname boundary, rejects a 65,536-byte path and
malformed UTF-8, permits directory creation/removal with a trailing slash, and refuses to
open, create or remove a regular file through a trailing-slash spelling. The
create phase has 57 assertions; `verify` has 46 and never recreates or writes
the test file. The first differing byte and native errors are printed on failure.

The `time` phase runs `fstest time` as a Ring 3 child. It checks the C Unix
epoch boundary, `time`, `timespec_get`, UTC calendar conversion, `mktime`,
`strftime`, process CPU ticks, monotonic elapsed time and wall-clock progress.
Its transcript is persisted to MGFS. After QEMU stops, the runner checks the
transcript against host UTC and verifies the calendar independently. With
QEMU's `clock=vm` under TCG, guest virtual time can differ from host elapsed
time; the accepted upper bound is the configured guest-phase timeout, and the
measured offset is recorded. This checks the RTC's UTC date and C conversion,
not clock accuracy under TCG. The CMOS source reports whole seconds and zero
nanoseconds. This does not qualify physical RTC hardware.

After guest-initiated ACPI shutdown, QEMU remains stopped under `-no-shutdown`.
The runner captures QMP shutdown events and backing-device block statistics,
then quits QEMU and waits for process termination before reading media. It
extracts MGFS, runs the independent C `mgfsck`, and reads namespace/Record/data
through the separate Python image tooling. Exact payload, type, unique name,
identity, complete ordered transcript with independently specified return values,
and the waited-child completion record are mandatory. A PASS string alone
cannot qualify. The second boot uses the **same disk**, without repopulation,
repair, or reset; identity and contents must still match.

With `--network-receive-smoke`, the runner adds a separate `network` boot phase
after the ordinary create, verify, stdio and math phases; it omits `time` from
that campaign so an unrelated clock-test failure cannot suppress RX evidence.
The network child writes a bounded transcript, `netinfo interfaces` records
RX/TX counters, and Shoot records child completion before requesting shutdown.
The stopped MGFS image is checked independently and the validator rejects a
missing phase marker, incomplete transcript, wrong gateway, fewer than 16
replies, insufficient packet-counter deltas, absent interface RX, or missing
child completion. A two-vCPU QEMU TCG run proves both CPUs online, but does not
prove device interrupts or allocator work were simultaneously serviced by both
CPUs.

The optional negative control changes one data byte on stopped disposable
media, preserving checker-valid metadata. A third real verify process must
report offset zero, exit 43, leave content unchanged, and prevent Shoot's
success continuation. The runner stops that guest via QMP after observing the
nonzero child status. Host evidence tests additionally reject missing launch,
timeout, empty execution, incomplete transcripts and incorrect operations or
return values. These infrastructure tests use controlled process/QMP dependencies;
they do not count as guest execution.

## Bounds and measured evidence

The runner accepts one through four fresh campaigns and a 30–300 second limit
per boot (180 by default). Each child has two 8,193-byte static data buffers, a 32-record process
inspection page, and 1,024-entry directory/process traversal bounds. Each disk is 135,283,200 bytes. A campaign
including the negative control retained about 482 MB of images, firmware,
framebuffers, logs and reports. QMP lines are limited to 64 KiB and event history
to 1,024 records; serial and QEMU diagnostics are monitored against an 8 MiB
limit. Failed media and evidence remain in the selected output directory.
`report.json` records run IDs, image/payload hashes, timing, QEMU block counters,
runner CPU/RSS, and explicit PASS or FAIL. Build failure and missing tools fail
qualification; no backend is silently substituted.

On Debian x86-64 with QEMU 10.0.13 and TCG, two fresh measured campaigns took
15.34–16.05 seconds for create and 10.12–10.17 seconds for second-boot verify.
Create recorded 964–981 backing read requests, 295–303 writes and 67–69 flushes;
verify recorded 917 reads, 254 writes and 60 flushes. These QEMU counters include
boot, services, scripts, history and transcripts; they are not isolated filesystem
operation counts or hardware command counts. The two campaigns measured 26.43 seconds of runner CPU and
154,832,896 bytes peak runner RSS; guest heap usage was not instrumented.
Both wrong-content controls exited
43 and all independent checker runs reported zero errors/warnings.

## Completion boundary

The poweroff/reboot implementation now quiesces applications and leaf services
and synchronizes mounted filesystems before its ACPI request. The filesystem
campaigns documented here do not exercise that lifecycle. They establish the
observed persisted test payload after confirmed MGFS mutations, process
completion, QEMU stop and a fresh boot; they do not promise crash atomicity,
general reboot recovery, or physical power-loss durability. Serial output is
supplemental; ordinary console and native results are captured through
framebuffer and persisted transcripts.

TCG single-CPU results do not qualify KVM, SMP, real hardware, hot removal,
forced process termination during I/O, or power failure. Host ASan/UBSan runs
are separate evidence, with leak detection disabled.
[Secondary-volume campaigns](guest-secondary-volumes.md) separately exercise
MGFS/FAT32/exFAT automount, authorization, clean unmount/remount and persistence.
The bounded [shutdown/history guest](guest-filesystem.md#shutdown-and-reboot-history-smoke)
separately exercises the global power transition. Longer randomized workloads
remain separate. Existing safe-file size and native filename limits are unchanged.
