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
an exact run identifier, invoke `tests/guest_mgfs_test.py --help` after building
`binaries fstest mkmgfs mgfsck` with the selected `BUILD_DIR`.

## Executed path and evidence

The runner creates a disposable GPT image with a 64 MiB FAT32 ESP and a
64 MiB MGFS root. It installs normal system payloads, test-only `/bin/fstest`,
and three Shoot scripts through existing host image-builder primitives.
OVMF variables are private to each boot. QEMU uses Q35, one CPU, 512 MiB RAM,
xHCI USB mass storage, a USB keyboard, and user-mode e1000 networking. Normal
kernel initialization, services, developer autologin, and Shoot remain active.

QMP stdio captures framebuffer evidence and injects a command only after the
existing console prompt is recognized from its exact font glyphs. Shoot
launches a real Ring 3 `fstest` child, redirects its stdout to MGFS, waits for
its exit status, closes its process handle, and writes a completion record
only after status zero. The child uses native file/directory/metadata syscalls;
there is no kernel-internal substitute for those operations.

`fstest create <run>` requires absence, creates `/temp/mgfs-<run>.bin`, and
writes exactly 8,193 deterministic binary bytes. The independently specified
pattern uses the 16-character run identifier and byte position. It checks exact
counts, seek/read at sector and block boundaries, EOF, close/reopen, all bytes,
file type/length/identity, and exactly one matching enumeration entry. A bounded
native process snapshot also requires a live `/core/deviced` system service,
covering its normal startup through the native application loader. The create
phase has 42 assertions; `verify` has 39 and never recreates or writes the test
file. The first differing byte and native errors are printed on failure.

After guest-initiated ACPI shutdown, QEMU remains stopped under `-no-shutdown`.
The runner captures QMP shutdown events and backing-device block statistics,
then quits QEMU and waits for process termination before reading media. It
extracts MGFS, runs the independent C `mgfsck`, and reads namespace/Record/data
through the separate Python image tooling. Exact payload, type, unique name,
identity, complete ordered transcript with independently specified return values,
and the waited-child completion record are mandatory. A PASS string alone
cannot qualify. The second boot uses the **same disk**, without repopulation,
repair, or reset; identity and contents must still match.

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

ACPI poweroff does **not** provide VFS-wide quiescence, sync or clean unmount.
Background services may write until guest execution stops. This qualification
establishes the observed persisted test payload after confirmed MGFS mutations,
process completion, QEMU stop and a fresh boot. It does not promise global
filesystem shutdown ordering, crash atomicity, reboot recovery, or physical
power-loss durability. Serial output is supplemental; ordinary console and
native results are captured through framebuffer and persisted transcripts.

TCG single-CPU results do not qualify KVM, SMP, real hardware, hot removal,
forced process termination during I/O, or power failure. Host ASan/UBSan runs
are separate evidence, with leak detection disabled. FAT32/exFAT guest campaigns,
longer randomized guest workloads and shared shutdown architecture remain
separate work. Existing safe-file size and native filename limits are unchanged.
