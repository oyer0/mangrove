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

At boot, the legacy e1000 interrupt is routed through the IOAPIC to the BSP.
Its receive helpers carry an explicit controller reference; they do not use a
shared mutable current-controller pointer. The per-controller transmit descriptor
queue is protected by an IRQ-saving spinlock because transmit callers may run
on different CPUs and the completion interrupt shares its consumer state.
These rules protect receive context selection and transmit-ring ownership;
they do not establish a general device-operation lifetime for hot removal.

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
result codes and timeouts. ICMP supports one outstanding echo request; a
second concurrent request returns `MG_ERR_BUSY`. Its request and response
state is protected against interrupt and process access. UDP endpoint
registration, receive dispatch, and
unregistration use a kernel spinlock. Dispatch holds that lock across the
bounded receive callback, so successful unregistration is a callback-quiescence
point before an owning datagram object is released. Receive callbacks must be
nonblocking and must not recursively register or unregister endpoints. The
native datagram table, creation reservations, automatic port allocation, and
packet queues use a separate lock; creation never holds it while acquiring the
UDP endpoint lock, and teardown unregisters before removing the object.

The current TCP implementation remains a single connection. Its connection
state is shared between process operations and the receive interrupt path; it
does not have a complete synchronization protocol for concurrent TCP
syscalls on multiple CPUs. Avoid treating the current TCP singleton as a
multi-caller interface.

The PCI network lifecycle worker can unregister and tear down a NIC while
process-side sends run on another online CPU. The registry and driver paths do
not hold a shared device-operation lifetime across that interval; local
interrupt masking does not serialize another CPU. Device removal during active
network traffic is therefore not safe or qualified. A removal event alone does
not establish that in-flight sends or receive processing have quiesced.
E1000 DMA rings and buffers also require hardware quiescence before their
frames can be released. The current lifecycle path provides no such guarantee;
active e1000 removal remains unsupported.

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
