# Native secondary-volume guest qualification

`make test-guest-secondary` builds an isolated UTC-trusted system and runs two
fresh campaigns for each of MGFS, FAT32 and exFAT. It reuses the root-MGFS guest
runner, native `fstest`, Shoot, volume IPC clients and normal system services.
It does not invoke `make run`, open developer images, install test payloads in
normal release images, or substitute kernel-internal filesystem operations.

```sh
make test-guest-secondary \
  GUEST_SECONDARY_BUILD=/tmp/mangrove-secondary-build \
  GUEST_SECONDARY_OUTPUT="$PWD/build/guest-secondary-acceptance" \
  GUEST_SECONDARY_ACCEL=tcg
```

The output directory must not exist. Reserve 2 GiB of disk-backed artifact
capacity per campaign; the default six campaigns require 12 GiB free admission
capacity. Avoid small RAM-backed `/tmp` outputs. `GUEST_SECONDARY_REPETITIONS`
accepts 1–3; two is the acceptance default. The Python entry point accepts a
single `--backend`, explicit firmware paths and a `--run-id` for one campaign.
Dependencies are the GNU toolchain, QEMU x86-64, OVMF, `sgdisk`, dosfstools,
mtools, exfatprogs, and the repository's independent MGFS tools.

## Executed route

Each boot uses Q35, one vCPU, 512 MiB RAM, OVMF with private variable storage,
xHCI, USB keyboard, and normal kernel/service/session initialization. The
existing disposable ESP/MGFS system disk uses USB port 2; a separate GPT disk
with one 64 MiB filesystem partition uses port 3. The secondary GPT role is
`QUAL_DATA`, never a system-root or ESP role. Profile labels are `QUAL_MGFS`,
`QUAL_FAT` and `QUAL_EXFAT`.

The native process observes the volume IPC snapshot and deviced's generation-
checked device snapshot, and validates filesystem, label, distinct instance and
parent identities, GPT leaf relationship, USB parent, mountpoint and read-only
state. Instance IDs and device display names are observed, not hardcoded.
Volumed owns mount policy and the restricted mount syscalls. `fstest` uses the
existing client for lifecycle requests; a normal application cannot call the
privileged mount, unmount or suppression syscalls directly.

MGFS receives an offline-provisioned `/work` owned by the development user;
ordinary permissions remain authoritative. FAT32 uses a formatted shared-volume
workspace. exFAT creates its workspace through native mkdir. All profiles use
the same file operations and independently specified 8,193-byte binary pattern.
Only fixture provisioning differs. Required exFAT timestamps use the centralized
clock: the isolated forced build sets `RTC_UTC_TRUST=1`, QEMU uses
`-rtc base=utc,clock=vm`, and the native test requires available realtime with
whole-second precision. This assertion does not apply to physical hardware.

## Lifecycle and authority evidence

A create boot verifies ordinary creation, exact counts/content, boundary seeks,
EOF, metadata, enumeration, close/reopen and directory creation/removal. It then:

1. Denies a real trusted confirmation and verifies the mounted file remains.
2. Requires BUSY unmount with an open file, directory cursor, and process CWD.
3. Restores CWD and waits for a real child that exits with intentionally open
   file/directory handles; subsequent unmount must prove cleanup released them.
4. Authorizes unmount through PASS, requires namespace detachment and no
   automatic remount, then explicitly remounts through volumed.
5. Reopens and compares the complete payload, then confirms final unmount.

The next boot uses the same secondary disk, without repopulation or repair.
Its verify process never recreates or writes the expected file and ends with
confirmed secondary unmount. Trusted prompts are answered using QMP keyboard
events; the runner requires the native console's echoed answer before submitting
the confirmation line. QMP injection acknowledgement alone is insufficient.
Approval policy remains `confirm`. No service capabilities are granted
to the test application.

Additional real boots test write-protected QEMU media, a separately provisioned
regular user, an unexpected label, absent volumed, absent secondary disk, and
one deliberately corrupted payload byte. Read-only admission must be visible in
both service snapshots, mutations must fail, and the full disk hash must remain
unchanged. The regular user's request targets an actually mounted volume and
must return PRIVILEGE_REQUIRED. A mount informational success is not treated as
authorization evidence. The corrupted payload must fail at byte zero, exit 43,
prevent Shoot's success continuation, and remain corrupted after verification.

Native writable open on a read-only mount currently returns IO: the open syscall
collapses the VFS admission error when object creation fails. Create/mkdir return
ACCESS_DENIED. The test requires these exact existing results and no writable
handle; it does not treat a failed mutation as successful creation or teardown.

## Independent media checks and reporting

Only after QEMU has stopped and its process has exited does the host extract
partitions. Mandatory evidence includes native operation transcripts, waited
child completion, actual mount/identity observations, GPT geometry, filesystem
identity, unique expected namespace, exact payload and seed bytes, and checker
success:

- MGFS: independent `mgfsck` plus Python Record/extent decoding.
- FAT32: `fsck.fat -n`, mtools namespace listing and exact extracted contents.
- exFAT: `fsck.exfat -n` plus a separate read-only directory/stream/data decoder.
  Its declared bounds are a 64 MiB, 512-byte-sector, one-FAT image, cluster sizes
  512–32,768 bytes, 4,096 directory slots and 16 pathname levels. It handles
  contiguous and FAT-linked streams, checks chain bounds and entry checksums,
  and rejects unsupported layouts. It never services runtime filesystem I/O.

The runner fails on launch, timeout, missing service/device, incorrect identity,
wrong return value, incomplete execution or missing checker/content evidence.
Synthetic evidence-validator tests deliberately reject malformed outcomes; they
are infrastructure checks and do not count as guest boots. Failed images,
framebuffers, firmware variables and transcripts are retained. Each campaign
records binary/image hashes, run identifier, per-boot QEMU read/write/flush
counters, decisions, CPU/RSS and artifact bytes in `report.json`.

Each campaign has eight boots. The per-boot timeout is bounded to 30–300 seconds
(default 180). QMP lines/events and diagnostic limits are inherited from the
[root-MGFS runner](guest-filesystem.md). Payload buffers total 16,386 bytes;
the bounded static device observation array has 128 entries. Guest heap peaks
are not instrumented. QEMU counters include discovery, mount and filesystem
activity; they are not physical hardware command counts.

## Measured acceptance evidence

On Debian x86-64 with QEMU TCG, two fresh campaigns per filesystem completed
48 secondary-volume boots. Both independent root-MGFS baseline campaigns also
passed, including their wrong-content controls. Every secondary campaign passed
its four expected child-failure controls, byte-identical read-only checks,
regular-user denial and complete create/remount/second-boot readback.

| Filesystem | Create boot seconds | Verify boot seconds | Secondary create reads/writes/flushes | Secondary verify reads/writes/flushes |
| --- | --- | --- | --- | --- |
| MGFS | 22.81–23.15 | 12.61–12.79 | 153 / 25 / 4 | 72 / 0 / 0 |
| FAT32 | 24.09–24.97 | 13.88–14.92 | 133 / 117 / 11 | 33 / 0 / 1 |
| exFAT | 23.28–25.17 | 12.68–13.80 | 232 / 124 / 47 | 94 / 0 / 1 |

Secondary request counts matched between both campaigns. The complete run took
721.29 seconds wall time and 242.60 seconds runner CPU, with 224,440,320 bytes
peak runner RSS and 10,419,129,618 artifact bytes. These are integrated guest
measurements, not isolated filesystem or hardware latency guarantees.

Snapshot serialization adds one 32-byte static mutex and no heap allocation.
The bounded block-capture vectors request 512 stack bytes. The measured target
prologues reserve 672 bytes for refresh and 1,104 bytes for block enumeration,
excluding nested probe frames and callers. Identity checks add registry lookups,
not media reads. Existing filesystem probes still determine I/O and lock hold
time. Host production-function regressions use controlled scheduler/IRQ and
discovery providers, production FIFO mutex logic, deterministic blocked probes,
stale pagination, removal/replacement and 512 contended refreshes. A historical
negative control demonstrates admission of the original unsafe nested refresh.
The separate production mutex regression covers 40,000 contended handoffs.

## Snapshot synchronization and limitations

Kernel device inspection serializes the whole refresh/copy lifetime with a FIFO
mutex, including presentation-number assignment and completed-generation
publication. Probes can block, so no spinlock spans them. Readers of an old
generation receive RETRY; failed admission leaves outputs unchanged. Captured
block identities are rechecked after probes, preventing replacement metadata
from being associated with a retired instance. Presentation-number exhaustion
reports zero rather than recycling another instance's number.

The gate does not freeze hardware, filesystem contents or all registries at one
instant. Snapshots are observations, never management authorization; every mount
and destructive operation must still revalidate immutable identity and admission.
The existing limits remain: 32 block registrations, 384 generic device records,
128 retained disk presentation numbers and 32 volumed leaf records. Native
client observation buffers are bounded too; this campaign does not qualify
capacity exhaustion as complete discovery of a larger machine.

## Physical USB removal qualification

```sh
make test-guest-hotplug GUEST_SECONDARY_ACCEL=tcg
```

This reuses the same native application, boot builder, QMP runner, permission
policy and independent MGFS decoder. Two fresh idle MGFS campaigns and two
fresh retained-reference campaigns each perform physical unplug, reattachment,
confirmed secondary unmount and a persistence boot using the same image. An
additional fresh boot deliberately attaches checker-clean replacement media
whose expected file differs at byte zero. That child must exit 43, omit its
success continuation and leave the incorrect bytes unchanged. Output directories
must be new; allow disk-backed space for the runner's 2 GiB-per-campaign admission
budget, including the negative control.

The secondary uses explicit QEMU file/raw block nodes and a USB frontend on
xHCI port 3. Native `fstest` completes its writes and closes active secondary
I/O before displaying a run-specific readiness marker through its duplex
console input handle. Stdout remains a structured log on the system root. It
then blocks for console input. The host deletes only the secondary frontend,
waits for `DEVICE_DELETED` naming that exact frontend, and releases the native
handshake. A child-device event or the command acknowledgement is insufficient.
The native child requires old block identities to disappear, volumed to remove
the cached volume, namespace/open to return NOT_FOUND, and normal root content
and service checks to succeed. Its second handshake authorizes frontend
reattachment using the retained backend. Fresh device/partition IDs, automount
and exact readback are mandatory. No image is inspected while QEMU owns it.

The retained-reference profile holds a read/write file and directory cursor
across physical loss, after proving an authorized unmount returns BUSY without
detaching the mount. Read, write, seek and enumeration through the old references
must return DEVICE_GONE; after reattachment, old reads/writes/enumeration still
must fail, and new handles must read the expected bytes. Explicit stale-handle
close and the waited native child must complete. These are idle references,
not removal during an active transfer. Synthetic parser controls reject missing
or mismatched deletion events, missing observations, duplicate/stale identity,
wrong outcomes/content, incomplete execution and inappropriate stale-reference
success. Synthetic controls are not guest execution evidence.

The scheduler-backed FIFO block request permits runtime enumeration to coexist
with ordinary system-disk traffic without blocking the event worker behind its
own completion work. Host tests execute production mutex, block and controller
admission functions with controlled scheduling/transport dependencies, including
cancellation, nested gate ownership, generation changes and repeated contention.
They do not simulate filesystem semantics or qualify real SMP hardware.

Deferred admission uses the existing thread waiter and adds no request heap or
scheduler queue. The kernel FIFO mutex remains 32 bytes; the controller singleton
grows by 16 bytes, including its selected port/generation and reservation state.
Disconnect inspection is bounded to the controller's root ports (at most 255).
Normal block operations retain their existing path; connected-port discovery
holds one grant through its existing probes, without additional sector scans.

Debian x86-64 TCG acceptance passed two fresh campaigns per hotplug profile,
each with a second persistence boot, plus the real wrong-replacement control.
Two additional accounting campaigns captured the secondary's QEMU requests
before frontend deletion, which resets frontend counters:

| Profile | Initial reads/writes/flushes | After reattachment | Persistence boot |
| --- | --- | --- | --- |
| Idle | 84 / 15 / 2 | 70 / 0 / 0 | 72 / 0 / 0 |
| Retained file/cursor | 85 / 15 / 2 | 70 / 0 / 0 | 72 / 0 / 0 |

Those accounting campaigns took 62.72 and 63.19 seconds wall time, 27.59 and
27.19 seconds runner CPU, at most 225,173,504 bytes runner RSS and about 816 MB
of artifacts each. These include two full boots, discovery and checker work;
they are not isolated driver costs or guest heap peaks. Native logs enforce
20 idle or 33 retained-reference volume outcomes plus three exact-content
phases during removal/reattachment. Independent checks run only after exit.

Physical-removal qualification also supports FAT32 and exFAT with the same
native operation workload and independent host validators. `--hotplug-cwd`
adds identity-bound CWD, moved ancestry, mounted-root `..`, inherited child CWD,
stale relative operations, absolute-path recovery and explicit rebind checks.
It implies retained file/cursor coverage. A stale-instance unmount request must
refuse the replacement volume and leave its data accessible. exFAT requires
the explicit UTC-trusted build and verifies the actual native clock state.

See [device lifetime qualification](guest-device-lifetime.md) for controlled
MGFS failure/removal, authorized owner observations and the bounded active-I/O
feasibility case. These are distinct from healthy idle/reference campaigns.
Rapid replacement races, arbitrary controller failures and multi-volume eject
remain unqualified.

Clean secondary unmount is distinct from global shutdown. The production
poweroff/reboot path now quiesces applications and leaf services and syncs
mounted filesystems, but these campaigns do not exercise it. Host reads wait
for QEMU termination even after secondary unmount. This testing does not
establish power-loss durability, persistent recovery, SMP, hardware,
service-restart resilience, or exhaustive filesystem-layout/Unicode coverage.
Existing namespace, safe-file size and recovery limits are unchanged. Host
sanitizers and MFQS remain separate qualification evidence; leak detection is
disabled in the required host suite.

[Native safe-file and application qualification](guest-safe-files.md) adds
coherent snapshots, conditional replacement, exclusive creation and real
Reed/Shoot workflows using the same disposable topology and service policy.

The opt-in [native C stress qualification](native-filesystem-stress.md) reuses
this discovery, authorization, boot and independent-media infrastructure for
seeded modeled workloads and exact trace replay. It does not replace the
authorization, BUSY-reference, read-only or hot-removal baselines.
