# IPC, system services, and supervision

Mangrove's kernel supplies bounded named IPC endpoints and authenticated
request context. System daemons own policy and long-running coordination;
Sprout owns their process lifecycle. Public protocol structures live under
`libc/include/mg/` and contain copied data, never kernel pointers.

## IPC contract

The generic IPC envelope is version 1 and exactly 1024 bytes, with a
1008-byte payload. Service names are shorter than 32 bytes and use a bounded
lowercase identifier syntax. The kernel currently bounds the registry to 8
endpoints, 32 concurrent request records, and a queue depth of 8 per endpoint.
One synchronous request may be outstanding per client process.

A client resolves an endpoint name to a generation-tagged handle and submits a
versioned request. The kernel copies the payload, records the caller's PID,
session, service identity, effective credentials, and process name, then
blocks the client until reply or endpoint failure. The receiver gets a
kernel-created request-context handle. Protected service operations claim
that context once; payload fields cannot substitute a UID, role, or service
identity.

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

Only PID 1 may ask the kernel to spawn one of these definitions. A service
cannot select a different executable or grant itself capabilities through the
syscall payload.

## Sprout policy

Sprout supervises sessiond, logind, logd, networkd, deviced, and volumed.
Sessiond and logind are essential and cannot be stopped or restarted through
the normal control protocol. All supervised services use restart-on-failure;
Sprout first retries after 500 ms and applies a 5-second backoff after repeated
failures. Networkd is the only service with reload support.

Sprout's `running` state means the supervised process exists and has not
exited. Endpoint registration remains a mandatory responsibility of each
daemon rather than a separate generic readiness handshake. A daemon that
cannot establish its endpoint exits, allowing the process state to converge
to failed and restart.

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
