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

Legacy e1000 interrupts and RTL8168 MSI-X are routed to the BSP, including
devices initialized by the scheduler-backed lifecycle worker. E1000's shared
vector uses a per-controller IRQ lifetime gate; RTL8168 uses the IRQ registry's
per-vector gate. Teardown masks a present device's interrupt source and drains
admitted handlers before releasing their rings. E1000 receive helpers carry an
explicit controller reference; they do not use a shared mutable
current-controller pointer. The per-controller transmit descriptor queue is
protected by an IRQ-saving spinlock because transmit callers may run on
different CPUs and the completion interrupt shares its consumer state.

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

An ICMP echo whose next-hop MAC is not cached first submits an ARP request.
IPv4 reports that state separately from a failed packet submission. The
process-context ICMP API waits for neighbor publication within the caller's
echo timeout, then submits the same echo sequence. An immediate timeout
returns `MG_ERR_WOULD_BLOCK` after starting ARP; a finite timeout with no
neighbor reply returns `MG_ERR_TIMEOUT`. Route, link, device-generation, or
driver submission failures remain unavailable-network errors. Interrupt-
context transmit paths remain nonblocking and keep the boolean transmit
contract. When a submitted echo times out or its device generation becomes
stale, the kernel retires only that identifier/sequence from the single
outstanding-echo slot before releasing process-side admission. A missing
reply therefore cannot leave later requests permanently rejected as an
already-pending echo. The ARP cache uses a short IRQ-safe spinlock because
the receive interrupt publishes entries while process-context route checks
and neighbor queries read them. Cache locks are released before Ethernet
transmission or any wait.

The TCP core currently supports one active connection. Its state is protected
by an IRQ-saving lock shared by process operations and the receive path;
packet transmission happens from a connection snapshot after releasing that
lock. A second connection attempt returns `MG_ERR_BUSY`. A failed or reset
connection remains reserved until its stream owner closes or destroys it, so a
stale object cannot alias a later connection.

The native stream contract is:

- `mg_stream_connect` uses the supplied monotonic timeout. A zero timeout is
  an immediate `MG_ERR_WOULD_BLOCK`; `UINT32_MAX` uses the bounded 5-second
  default. Timeout, Ctrl+C interruption, reset, and device-generation loss are
  reported distinctly. Failed or interrupted handshakes release the reserved
  connection before returning.
- `mg_stream_send` returns a positive byte count for bytes accepted into the
  kernel's bounded TCP retransmission state. It does not assert that the peer
  application consumed the bytes. A return may be shorter than requested;
  callers advance by that count and retry only the remainder. TCP retains the
  last accepted segment until acknowledgment or connection failure, and waits
  for its acknowledgment before accepting a following segment. If a deadline
  or interrupt occurs after earlier segments were accepted, the positive
  partial count takes precedence. If that interrupt follows accepted progress,
  the connection records the interruption so a following operation returns
  `MG_ERR_INTERRUPTED` rather than continuing the cancelled write. A later
  operation observes a persistent transport failure if retries ultimately
  expire. Zero-timeout sends accept immediately available data and otherwise
  return a partial count or `MG_ERR_WOULD_BLOCK`.
- `mg_stream_receive` returns any currently buffered bytes, including a
  partial read. With no data it waits only until its monotonic deadline and
  responds to foreground process interruption. A zero-timeout read returns
  `MG_ERR_WOULD_BLOCK`; buffered bytes are delivered before a peer-close or
  reset status is reported.
- `mg_stream_close` is a bounded graceful FIN exchange. Timeout or
  interruption aborts the connection, and the close request consumes its
  native handle even when it returns that error. Process teardown aborts
  immediately without waiting for the peer.

Blocking stream syscalls retain their kernel object for the operation's
lifetime. A close attempted concurrently with an active send or receive
returns `MG_ERR_BUSY` and leaves the handle available for retry. This keeps
stream storage alive across cross-CPU handle closure and prevents close from
releasing the singleton while an operation still references it.

The retransmission state holds one segment up to the 1,460-byte MSS; the
receive queue holds 4,096 bytes and the advertised receive window tracks its
remaining free space. The implementation handles cumulative and partial ACKs,
retransmission, duplicate-data rejection, FIN/RST, deadlines, and window
updates. It does not implement window scaling, selective acknowledgments,
congestion control, or multiple concurrent connections; these remain TCP
interoperability and throughput limitations. Device removal during active
network traffic remains unsupported because the NIC lifecycle does not yet
provide a hardware-operation quiescence guarantee.

The IRQ lifetime gates serialize interrupt callbacks with teardown, but the
network-device registry and driver paths do not hold a shared device-operation
lifetime across process-side sends running on another CPU. Active removal
during network traffic therefore remains unsupported and unqualified. The
current PCI lifecycle path detaches an e1000 only after its PCI function is no
longer present; that path drains CPU-side IRQ access before releasing the
controller's DMA buffers. A general live-device shutdown sequence, including
quiescing in-flight transmit callers and stopping a still-present e1000's DMA
engines, remains outside the current contract.

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
