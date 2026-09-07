# Identity, sessions, privileges, and PASS

Mangrove separates persistent account identity, process credentials, system
service capabilities, login authentication, and authorization of a protected
operation. There is no generic root or sudo mode.

## Accounts and process credentials

The canonical account database is `/sys/accounts/users`. The kernel parses it
into an identity registry with bounded usernames, UIDs, roles, home paths, and
password records. UID zero is reserved for kernel/system-owned resources. The
preinstalled developer account uses UID 1000, and newly allocated account UIDs
start at 1001. Current human roles are regular and administrator.

A process carries only stable credentials: UID, role, and a service-privilege
mask. Normal children inherit effective human credentials. Role authority is
resolved against the live identity registry, so demotion affects existing
processes. Trusted services receive only the capabilities in the kernel
service definition. Human processes cannot acquire service capability bits.

Administrators may hold ordinary management privileges for users, network,
configuration, services, devices, and storage. Session management is reserved
for the designated services. Being system-owned does not itself bypass a
privilege check.

## Login and sessions

Password authentication is a kernel identity/PASS operation; account password
records are not exposed to logind or sessiond. Logind is the session frontend,
while sessiond is the backend that creates and ends kernel session records.
The kernel verifies their service identities and the authenticated origin of
cross-service requests.

A session has a monotonic boot-local ID, owner, user identity, lifecycle state,
activity state, and shell PID. The current table holds at most eight active
records. A launched shell and its children receive the session's credentials,
home/current-directory context, and session ID. Ending a session terminates
its member processes and releases the record.

Autologin policy is read from `/conf/session/config`. Eligibility is a
boot-scoped one-shot token consumed by logind; restarting a service cannot
silently repeat autologin after logout or shell failure.

## PASS authorization

PASS is the kernel-owned boundary for protected actions. The required
privilege and trusted description are selected by kernel code, not supplied by
the requesting client. For an IPC operation, PASS claims the kernel-recorded
request context and authorizes the original caller.

`/conf/security/config` selects the current provider:

- `password`: verify the current human administrator's password;
- `confirm`: present a trusted yes/no prompt;
- `scripts`: allow the current direct-admin script policy;
- `none`: allow after the privilege check.

A missing or malformed policy falls back to `confirm`. System services may
perform autonomous work only when their explicit service capability satisfies
the requested privilege; they do not trigger a human prompt.

PASS authorization is distinct from command-specific destructive confirmation.
For example, `diskutil` requires `MANAGE_STORAGE` and PASS once when the
process enters its management session. The kernel marks only that current
`diskutil` process. Individual format and GPT mutations arm one bounded
operation for that process and still require their exact `FORMAT`,
`INITIALIZE`, `CREATE`, or `DELETE` confirmation in userspace. The session
authorization cannot be transferred and disappears when the process exits.

Standalone mount, unmount, eject, network, account, and service-control paths
authorize their logical request through their trusted service or kernel
boundary. Client-supplied role or requester fields never grant authority.

## Authoritative code

- `kernel/include/identity.h`, `kernel/src/identity.c`, and
  `/sys/accounts/users`
- `kernel/include/pass.h`, `kernel/src/pass.c`, and `/conf/security/config`
- `kernel/include/session.h`, `kernel/src/session.c`, and
  `/conf/session/config`
- `kernel/src/service.c`, `kernel/src/syscall.c`, and
  `userspace/diskutil/main.c`
