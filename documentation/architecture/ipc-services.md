# IPC, system services, and supervision

Mangrove's kernel supplies named IPC endpoints and authenticated request
context. System daemons own policy and long-running coordination;
Sprout owns their process lifecycle. Public protocol structures live under
`libc/include/mg/` and contain copied data, never kernel pointers.
For the boot-to-userspace role map, see the [userland services overview](../services/README.md).

## IPC contract

The generic IPC envelope is version 1 and exactly 1024 bytes, with a
1008-byte payload. Service names are shorter than 32 bytes and use a bounded
lowercase identifier syntax. The endpoint registry is a linked list of
kernel-authorized service endpoints; it has no separate endpoint-count table
limit, and only services declared by the kernel can register names. A request
record is allocated for each synchronous caller and freed after both client
and receiver context ownership end. Request deliveries use a per-endpoint
FIFO and are not capped by a small global request table or fixed queue. The
caller receives `NO_MEMORY` if the kernel cannot allocate a record. One
synchronous request may be outstanding per client process, preserving
per-client backpressure and FIFO service order. Capacity is bounded by the
kernel heap and live process resources rather than a separate IPC table or
message-count ceiling. There is no priority scheduling; requests are admitted
in endpoint FIFO order. Allocation failure rejects a new request before it is
published. Each pending request consumes kernel heap for its fixed request
and reply envelopes until reply, cancellation, or endpoint exit.

Wait registration, cancellation and scheduler event publication share the IPC
lock. A wake latches for a running or ready thread and enqueues a blocked
thread, closing cross-CPU reply/enqueue races before parking. Every receive
return removes its own registration, including timeout/dequeue and overflow
paths, before its syscall lifetime ends. An endpoint retains no returned
receiver's scheduler pointer. Competing blocked receivers return `BUSY`.

Kernel event publication may run in interrupt context and cannot allocate.
Each endpoint therefore keeps a fixed eight-event ring. If it fills, new
events are coalesced into an explicit overflow indication so the service can
rebuild from authoritative state. Request and event queues are merged by
their sequence numbers during receive, preserving their order while allowing
request capacity to grow. This event ring is the remaining intentional
fixed-capacity queue; it is not a limit on request concurrency.

A client resolves an endpoint name to a generation-tagged handle and submits a
versioned request. The kernel copies the payload, records the caller's PID,
session, service identity, effective credentials, and process name, then
blocks the client until reply or endpoint failure. Request records are
allocated before taking the IPC spinlock, published only after initialization,
and zeroed/reclaimed after releasing the lock. Cancellation, endpoint exit,
and process exit unlink queued work and preserve the request until any active
receiver handoff has finished. Active requests are linked to their endpoint;
process exit follows the process's single outstanding-request pointer instead
of scanning all requests, and endpoint exit only fails work on that endpoint.
The receiver gets a kernel-created
request-context handle. Protected service operations claim that context once;
payload fields cannot substitute a UID, role, or service identity.

Kernel events use the same receive queue. When events overrun a queue, the
service receives an overflow delivery and must rebuild from an authoritative
snapshot rather than infer state from missed events.

Endpoint registration is restricted to the kernel-defined service that owns
the name. A duplicate live name is rejected. Process exit unregisters its
endpoint, fails pending requests, and leaves existing client handles referring
only to a dead object. Services treat failure to register or receive on their
mandatory endpoint as fatal so Sprout can observe and restart the process.

## Kernel service definitions

The kernel fixes each service executable, capability set, and optional
endpoint:

| Service | Executable | Endpoint | Service privileges |
| --- | --- | --- | --- |
| Sprout | `/core/sprout` | `sprout` | manage services |
| sessiond | `/core/sessiond` | `session` | manage sessions |
| logind | `/core/logind` | none | manage sessions |
| networkd | `/core/networkd` | `network` | manage network and configuration |
| deviced | `/core/deviced` | `device` | manage devices |
| volumed | `/core/volumed` | `volume` | manage devices and storage |
| logd | `/core/logd` | `log` | none |
| clipd | `/core/clipd` | `clipboard` | read-only session liveness query |

Only PID 1 may ask the kernel to spawn one of these definitions. A service
cannot select a different executable or grant itself capabilities through the
syscall payload.

## Sprout policy

Sprout supervises sessiond, logind, logd, networkd, deviced, volumed, and clipd.
Sessiond and logind are essential and cannot be stopped or restarted through
the normal control protocol. All supervised services use restart-on-failure;
Sprout first retries after 500 ms and applies a 5-second backoff after repeated
failures. Networkd is the only service with reload support.

Sprout's `running` state means the supervised process exists and has not
exited. Endpoint registration remains a mandatory responsibility of each
daemon rather than a separate generic readiness handshake. A daemon that
cannot establish its endpoint exits, allowing the process state to converge
to failed and restart.

The clipboard uses seven of the eight endpoint slots. Its format, transfer and
resource contract is specified in [Session clipboard](clipboard.md).

## Protocol ownership

Subsystem protocols have their own version and bounded request/response
records inside the generic IPC envelope. Current endpoint names are defined
once by the kernel service table and consumed through their corresponding
public headers. Inspection protocols paginate snapshots; mutating protocols
resolve current kernel state and authenticated request context before acting.

## Authoritative code

- `libc/include/mg/ipc.h` and subsystem service headers
- `kernel/include/ipc.h`, `kernel/src/ipc.c`, and `kernel/src/service.c`
- `userspace/sprout/main.c`
- the main loops of `userspace/sessiond`, `userspace/networkd`,
  `userspace/deviced`, `userspace/volumed`, and `userspace/logd`
