# Userland services

After the kernel has initialized storage and mounted the system MGFS root, it
starts `/core/sprout` as the first userspace process. Sprout starts and
supervises the remaining fixed service definitions. The service executable,
identity, capabilities, and endpoint name are selected by the kernel; a
request payload cannot grant a service different authority. The kernel's
bounded IPC interface copies versioned request records and authenticates the
requesting process.

## Service roles

| Service | Role |
| --- | --- |
| `sprout` | Starts and supervises the fixed service set, reports lifecycle state, and handles authorized service-control requests. `/bin/sprout` is its user command client. |
| `sessiond` | Creates, queries, lists, and ends session records for logind. |
| `logind` | Authenticates users or selects configured autologin, creates a session, and launches Shoot with that session identity. |
| `logd` | Accepts bounded log records, adopts early kernel records, and manages persistent system-log files under `/sys/logs`. |
| `networkd` | Reconciles interface/link events, configured addressing, DHCP lease state, and authorized network configuration requests. |
| `deviced` | Subscribes to device events and serves coherent paginated device snapshots; after event overflow it rebuilds from the kernel snapshot. |
| `clipd` | Owns session-isolated volatile clipboard items and serves atomic, chunked UTF-8 transfers. See [clipboard](../architecture/clipboard.md). |
| `volumed` | Tracks registered block devices, filesystem identity and labels, mounts eligible secondary volumes under `/vol`, and handles authorized mount, unmount, and eject requests. |

The root MGFS and its associated EFI system partition are mounted during kernel
initialization. Other discovered volumes are left to volumed. Removable-media
eject means a clean unmount plus runtime automount suppression; it is distinct
from physical USB unplug. Detailed registration, identity, recovery, and
mounting rules are in [storage lifecycle](../storage/lifecycle.md).

## Login and supervision

Logind performs manual authentication or uses the administrator-selected
autologin account, then asks sessiond to create the session and starts Shoot
with the resulting identity. The session's CWD, handles, and process authority
follow the normal process/VFS rules. Account, session, privilege, and PASS
behavior is specified in
[identity and authorization](../security/identity-and-authorization.md).
When Shoot exits, including on empty-input EOF, logind waits for sessiond to
retire the session's remaining processes and then clears the terminal before
accepting another login. This clears the visible framebuffer and terminal cell
ring. Unexpected shell termination is sent to logd instead of being printed
over the next login prompt. If session retirement or terminal reset cannot be
confirmed, logind remains at that boundary and does not expose a new session.

Sprout supervises sessiond, logind, logd, networkd, deviced, volumed, and clipd. Its
current policy restarts failed services after a short delay, with a longer
backoff after repeated failures. Sessiond and logind are essential and cannot
be stopped through normal service control. Networkd supports reload; the other
managed daemons do not expose a reload operation. A reported running state
means the process is alive; endpoint registration remains the daemon's own
readiness responsibility.

## Boundaries and detailed contracts

The kernel supplies IPC endpoints, authenticated request context, service
identity/capabilities, PASS authorization hooks, and process supervision
primitives. Userland daemons implement policy and coordination. The service
protocol and restart contract is documented in
[IPC and supervision](../architecture/ipc-services.md); subsystem policy is
documented under [network](../network/architecture.md),
[storage](../storage/lifecycle.md), and
[identity](../security/identity-and-authorization.md).

The privileged interactive `diskutil` client uses kernel storage-management
syscalls for formatting, labels, and GPT changes; these operations do not pass
through volumed's mount-management IPC.

Service source lives in `userspace/{sprout,sessiond,logind,logd,networkd,deviced,volumed,clipd}/`.
Kernel service definitions and capabilities are in `kernel/src/service.c`;
the initial root and userspace setup is in `kernel/src/init.c`.
