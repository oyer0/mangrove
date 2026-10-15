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
events; approval policy remains `confirm`. No service capabilities are granted
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

Clean secondary unmount is distinct from global shutdown. ACPI poweroff still
provides no VFS-wide quiescence or clean shutdown of the system root. Host reads
wait for QEMU termination even after secondary unmount. This testing does not
establish power-loss durability, persistent recovery, SMP, hardware, active-I/O
hot removal, replacement races, multi-volume eject, service-restart resilience,
or exhaustive filesystem-layout/Unicode coverage. Existing namespace, safe-file
size and recovery limits are unchanged. Host sanitizers and MFQS remain separate
qualification evidence; leak detection is disabled in the required host suite.
