# Native device lifetime and recovery qualification

The native `fstest`, Shoot and the existing secondary-volume runner exercise
production USB → block/GPT → deviced/volumed → VFS → filesystem paths. Native
syscalls, process credentials, PASS and ordinary permission checks remain in
force. Host dependencies do not substitute filesystem operations in these
campaigns.

## Healthy replacement and directory identity

Build isolated binaries with the QEMU deployment's explicit RTC-as-UTC policy:

```sh
make -B -j4 BUILD_DIR=build/guest-lifetime RTC_UTC_TRUST=1 binaries fstest mkmgfs mgfsck
python3 tests/guest/guest_secondary_test.py \
  --build-dir build/guest-lifetime --output build/lifetime-hotplug \
  --backend all --hotplug --hotplug-cwd --repetitions 2 --accel tcg
```

Output directories must not exist. Each campaign uses disposable GPT/ESP/MGFS
system media, a separate 64 MiB filesystem partition, private OVMF variables,
Q35, one vCPU, 512 MiB RAM, xHCI and TCG. USB keyboard is on port 1, system
storage on port 2 and secondary storage on port 3. Never use developer images
or physical devices. The runner requires free disk capacity and bounds boots
to 30–300 seconds. Explicit QEMU block nodes retain the secondary backend when
only its USB frontend is deleted.

Required evidence includes unique device/parent identities and snapshot
pagination, actual automount, exact binary contents, CWD rename and ancestry,
mounted-root `..`, child inheritance, BUSY clean unmount, and confirmed process
cleanup. A native readiness handshake precedes idle removal; a matching QMP
DEVICE_DELETED event is necessary but insufficient. Native observations must
also establish offline discovery, namespace detachment and stale-reference
refusal. Root operations must continue. Reattachment requires new identities,
legitimate automount, exact fresh-handle content and refusal to rebind old CWDs.
An old-instance lifecycle request must not detach replacement media. Explicit
`chdir` permits recovery. Confirmed secondary unmount and the next boot use the
same image, without reformatting or repair.

`--hotplug-negative-control` adds the MGFS checker-clean wrong-replacement
control. Separate secondary persistence profiles retain wrong-payload,
write-protected, unauthorized lifecycle, missing service/device and wrong-label
controls. The wrong-replacement control is currently MGFS-specific; it is not
reported as executed on the other filesystems.

Only after QEMU exits do independent validators inspect media: `mgfsck` plus
Record/extent decoding, `fsck.fat` plus mtools, or `fsck.exfat` plus the bounded
independent exFAT directory/stream reader. Unsupported checker/reader layouts
fail qualification rather than receiving a synthetic PASS.

The root runner also accepts `--relative-launch`: Shoot performs native
`chdir` to `/temp` and launches an isolated, byte-identical `./fstest-relative`
through the shared command helper and native executable loader. That basename
is absent from `/bin`, so accidental `/bin` rewriting cannot pass. Its same-image create/verify and wrong-content
control retain the ordinary independent content and child-completion oracle.

## Online read-error qualification

```sh
python3 tests/guest/guest_lifetime_test.py --build-dir build/guest-lifetime \
  --output build/lifetime-read --kind read --accel tcg
python3 tests/guest/guest_lifetime_test.py --build-dir build/guest-lifetime \
  --output build/lifetime-read-control --kind read --negative-control --accel tcg
```

A persistent blkdebug read error targets the independently decoded first data
extent of an offline-provisioned 8,193-byte MGFS file, not superblock or Record
sectors. The actual target sector is recorded. No error may occur before the
native readiness handshake. Initial, repeated and fresh-handle reads must
return IO, while zero-length and genuine EOF reads return zero. Native process
loading from that unreadable file must report IO, not INVALID_EXEC, and release
the aborted child resources. Metadata remains
readable, no recovery owner is created, and authorized clean unmount succeeds.
The host requires actual failed QEMU read requests, zero secondary writes, an
unchanged complete secondary image and independent checker acceptance. The
no-fault control must fail specifically on a confirmed one-byte read. Host
checked-read regressions additionally verify confirmed partial counts and
positions on all three persistent filesystems.

## Online pathname metadata errors

```sh
python3 tests/guest/guest_lifetime_test.py --build-dir build/guest-lifetime \
  --output build/lifetime-lookup --kind lookup --accel tcg
python3 tests/guest/guest_lifetime_test.py --build-dir build/guest-lifetime \
  --output build/lifetime-lookup-control --kind lookup --negative-control --accel tcg
```

The MGFS case independently provisions a file beneath `/work`, then arms a
persistent read error at that directory's first data extent after native
volume discovery and writable-mount validation. QMP `blockdev-reopen` inserts
the existing blkdebug filter at the explicit readiness handshake; the native
process waits at that handshake until the drained graph change succeeds.
A verified 512 KiB native read displaces cached directory data before arming;
no privileged cache invalidation is used.
No filesystem metadata is modified to inject the error.

Native open, metadata, create, mkdir, remove, move and executable loading must
report IO rather than absence or successful mutation. The authorized recovery
snapshot must contain no owner for this pre-write failure, clean unmount must
succeed, and the host requires zero secondary writes, actual failed reads,
byte-identical complete media and independent checker acceptance. The no-fault
control must fail specifically because native open succeeds, not because
launch or discovery failed. Full production host regressions cover all three
filesystems, including failure during the second lookup in removal of an
active CWD and historical controls demonstrating its unsafe deletion.

## Genuine unresolved recovery ownership

```sh
python3 tests/guest/guest_lifetime_test.py --build-dir build/guest-lifetime \
  --output build/lifetime-write --kind write --accel tcg
python3 tests/guest/guest_lifetime_test.py --build-dir build/guest-lifetime \
  --output build/lifetime-flush --kind flush --accel tcg
python3 tests/guest/guest_lifetime_test.py --build-dir build/guest-lifetime \
  --output build/lifetime-missing-fault --kind write --negative-control --accel tcg
```

These bounded MGFS campaigns wrap only the isolated secondary backend with
QEMU blkdebug. The first secondary write/flush fails once, after the native
child's explicit readiness witness. QMP accounting must show no earlier
secondary mutation and exactly the intended failure. QMP uses isolated FIFOs;
diagnostics cannot corrupt its JSON stream.

The ordinary native write must return UNCERTAIN, not a confirmed byte count.
The authorized read-only inspection syscall must identify an actual unresolved
owner, selected partition and physical parent. With file/CWD references gone,
authorized clean unmount remains BUSY because the owner is retained. Physical
USB deletion must produce the matching event and native observations of the
same owner with offline-retained identities. Native process exit must not
release it. The parent proves continued root operations and completes through
Shoot's waited child path. Registration/range management exclusion is separately
covered by production block/VFS admission regressions; the guest does not gain
a formatting or recovery-discard capability.

The no-fault control must fail specifically because the same write confirms its
16-byte count. Missing execution, malformed rows, unknown outcomes, owner
identity changes, missing deletion, missing root progress, timeouts and missing
child completion cannot qualify. Parser negative controls are infrastructure
checks, not successful guest execution.

Failed media and original hashes are preserved. Checker acceptance after an
error does not resolve runtime ownership: a failed flush may leave apparently
valid media while completion remains unconfirmed. There is no automatic repair,
second boot of uncertain media, rollback or persistent recovery claim.

## Active-I/O feasibility boundary

```sh
python3 tests/guest/guest_lifetime_test.py --build-dir build/guest-lifetime \
  --output build/lifetime-active --kind active --accel tcg
```

The finite blkdebug case delays the first guest-origin MGFS dirty-superblock
write for two seconds and then fails it. The delay defines injected backend
latency, not a synchronization sleep. The runner triggers deletion from actual
SCSI write traces at the known partition superblock sector, with no successful
USB completion or cancellation yet recorded. It requires responsive QMP before
deletion, bounded deletion, the matched event, port-3 detach before successful
USB completion, native uncertainty, offline owner retention and root progress.
Cancellation is not interpreted as successful completion.

QEMU drains/cancels its block requests during USB frontend destruction. An
indefinitely suspended blkdebug request can consequently block synchronous
`device_del` and the QMP command needed to resume it; it is unsuitable for this
acceptance protocol. The finite case establishes real pending-I/O cancellation
and lifetime containment on this QEMU topology. It does not prove arbitrary
mid-sector electrical disappearance, unlimited backend hangs, every controller
reset, active snapshot/history interruption, or all hardware transport orderings.

## Host regressions, resources and remaining boundaries

`make test-sync` includes complete production VFS/MGFS location tests,
production checked reads and actual torn-write uncertainty on MGFS/FAT32/exFAT,
the complete ELF loader with controlled paging and exact segment verification,
and the native recovery-inspection helper with the actual privilege predicate
and registry. Transport, allocation, scheduler, user mappings and identity
providers are controlled host dependencies. The mounted-descendant rename test
uses production initramfs for the RAM mount and production MGFS for persistent
renames. The multithreaded MGFS handoff regression exercises FIFO operation
progress and uncertain teardown independently of guest single-vCPU campaigns.

Location objects request 320 bytes each on x86-64; live visited directory cache
entries remain until unmount, while unleased files and removed leaf directories
are pruned. A rename affecting mounted descendants requests a bounded 8,440-byte
plan before mutation; ordinary renames need none. Superblocks request 160 bytes,
recovery registry records 80 bytes and MGFS ordinary transactions 12,920 bytes.
The 32-pin sum of MGFS ordinary transaction and registry structures is 416,000
bytes, excluding mount state and directory locations. A retained pin also
retains its mount and filesystem state, so these structure
sizes are not a complete per-pin heap bound. The registry limit remains 32.
Inspection output is at most 1,024 bytes, with 1,280 bytes of internal owner
staging plus bounded request/local fields on x86-64;
no media scan or recovery mutation is performed by inspection. Guest heap peaks
are not instrumented. Driver/QEMU I/O, wall/CPU time, runner RSS, hashes and
artifact sizes are recorded by campaign reports where available.

The device-lifetime campaigns do not exercise global shutdown. The production
poweroff/reboot path now quiesces applications and leaf services and
synchronizes mounted filesystems. Confirmed secondary unmount, ordinary
mutation flushes, image readback and a second boot are separate pieces of
evidence. Host reads wait for QEMU termination. Leak detection is disabled
in the required sanitizer runs. SMP, physical hardware, reboot recovery,
power-loss durability, persistent repair, exhaustive fault schedules, arbitrary
unrelated-backend alias detection and full-volume scalability remain unqualified.
The native 65,535-byte pathname admission bound, filesystem-specific component
limits, FAT32's on-disk 32-bit file-size ceiling, namespace-proof budgets,
FAT32 metadata-I/O amplification and exFAT retained-memory bounds remain as
documented in filesystem and qualification references.

## Measured guest evidence

On Debian x86-64/QEMU TCG, two fresh CWD/retained-reference campaigns per
filesystem completed hot removal, replacement and a second persistence boot.
The six campaigns took 667.32 seconds wall time, 285.87 seconds runner CPU,
225,816,576 bytes peak runner RSS and 4,894,539,965 artifact bytes.

| Filesystem | Hotplug boot seconds | Persistence boot seconds | Secondary requests before removal, reads/writes/flushes | After reattachment | Persistence boot |
| --- | --- | --- | --- | --- | --- |
| MGFS | 58.63–60.28 | 41.54–43.23 | 115 / 43 / 7 | 71 / 0 / 0 | 72 / 0 / 0 |
| FAT32 | 60.15–60.97 | 40.93–42.86 | 149 / 133 / 19 | 33 / 0 / 1 | 33 / 0 / 1 |
| exFAT | 61.98–65.57 | 42.69–48.23 | 250 / 159 / 65 | 62 / 0 / 1 | 94 / 0 / 1 |

Each native hotplug log requires 84 volume outcomes for MGFS/FAT32 or 88 for
exFAT workspace initialization, plus separate exact-content phases. Counters
include discovery, metadata and service traffic; frontend deletion resets its
counters, so the pre-deletion snapshot is recorded separately. These are QEMU
requests, not isolated driver costs or hardware command counts.

Controlled write and flush failure/removal campaigns completed in 58.42 and
57.51 seconds respectively; finite active-I/O cancellation/removal completed in
62.68 seconds. Each retained approximately 745–746 MB of disposable artifacts.
The online-read and native-loader error campaign completed in 50.42 seconds
with eight actual failed read requests, zero writes and a byte-identical image. Checker
and native evidence are recorded separately; clean checker output never clears
an unresolved in-memory owner.
