# DHCP lease lifecycle

`networkd` owns IPv4 DHCP lease policy.  The kernel retains the active lease
and its boot-local monotonic deadlines; it does not run a DHCP policy daemon.

An accepted lease moves through `BOUND`, `RENEWING`, and `REBINDING`.  At T1,
`networkd` sends a bounded unicast renewal request when the server identifier
is known.  At T2 it uses broadcast rebinding.  The existing address remains
usable until the lease expiry deadline.  If no ACK arrives by expiry,
`networkd` clears the address, routes, DNS-derived state, ARP cache, and lease
metadata before beginning acquisition again.

Lease timing is measured only with the monotonic millisecond clock.  DHCP T1
and T2 options are used when valid.  Missing values default to one-half and
seven-eighths of the lease duration.  Values that do not produce the safe
ordering `0 < T1 < T2 < lease` are normalized inward when possible or
rejected.  Lease durations shorter than three seconds are rejected.

`networkd` waits in its existing IPC receive path with the next DHCP deadline
as a bounded scheduler timeout.  Network events and control requests wake the
same wait.  Retransmissions use 1, 2, and 4 second backoff, bounded by the
current renewal/rebinding phase and lease expiry.  Link loss suppresses DHCP
transmission while preserving a still-valid lease; expiry still wakes the
service so stale configuration is removed.
