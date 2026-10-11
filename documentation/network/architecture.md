# Network architecture

Mangrove keeps packet transport and active network state in the kernel while
placing persistent configuration and lease policy in `networkd`.

```text
NIC drivers and kernel network stack -> packets, protocols, sockets, active state
networkd                            -> configuration and DHCP lifecycle policy
netinfo and netcfg                  -> thin inspection/administration clients
```

## Kernel mechanism

The kernel owns network-device registration, generation-safe liveness,
Ethernet, ARP, IPv4, ICMP, UDP, TCP, DNS, DHCP wire exchanges, routes,
neighbors, and the active IPv4 configuration. E1000 and RTL8168 are the
current PCI NIC drivers. A low-priority lifecycle worker reconciles PCI and
these drivers every 500 ms because the current machine profile has no portable
PCI hotplug interrupt source.

Network devices have boot-local instance generations. Datagram and ICMP work
revalidates the exact device while waiting or transmitting, so removal cannot
make an old object operate on a replacement. Current resource limits are
bounded: at most four registered network devices, four queued datagrams per
datagram object, one active resolver wire transaction, and one active TCP core
connection.

Ordinary applications reach protocol operations through the network syscall
and generation-tagged kernel object handles. They do not send packet-policy
requests through `networkd`. The libc API in `mg/net.h` exposes status,
resolution, ICMP, datagram, and stream operations with explicit Mangrove
result codes and timeouts.

## `networkd` policy

`networkd` owns the `network` IPC endpoint and reads
`/conf/network/config`. The current configuration modes are `dhcp` and
`manual`; an optional canonical interface name may select the target. Manual
configuration supplies address/prefix and optional gateway and DNS values.

At startup or reload, networkd parses the bounded configuration and asks the
kernel to apply it. DHCP packet construction and receipt remain kernel
mechanism, while networkd schedules acquisition, renewal, rebinding,
expiration, and retry policy using monotonic deadlines. The detailed lease
state machine is documented in [dhcp-lease.md](dhcp-lease.md).

Kernel device and link events wake networkd through its IPC receive loop.
Event overflow triggers authoritative state reconciliation. A link-down event
does not busy-poll; current deadlines continue to bound lease expiry and later
work.

## Inspection and mutation

`netinfo` uses the versioned network service protocol for current interfaces,
routes, neighbors, connections, and configuration state. `netcfg` sends the
same protocol's mutation operations. Shared request and display code lives in
`userspace/common/network_client.c`.

For a mutating request, networkd passes the delivered kernel request-context
handle into the PASS boundary. The kernel authorizes the original caller for
`MANAGE_NETWORK`; UID or role fields in the request payload cannot grant
access. Configuration-file writes under `/conf` are separately protected as
administrator configuration changes.

## Authoritative code

- `kernel/src/net/` and `kernel/include/net/`
- `kernel/src/syscall.c` and `libc/include/mg/net.h`
- `userspace/networkd/main.c` and `libc/include/mg/network_service.h`
- `userspace/common/network_client.c`, `userspace/netinfo`, and
  `userspace/netcfg`
