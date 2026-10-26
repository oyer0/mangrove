# System CA root store

Mangrove packages administrator-managed public root certificates at
`/share/ca-certificates/`. The directory contains:

| File | Contents |
| --- | --- |
| `ca-bundle.pem` | PEM-encoded public root CA certificates. |
| `manifest.json` | Snapshot, source, converter, license, count, and bundle digest. |
| `MPL-2.0.txt` | The retained Mozilla Public License 2.0 text. |

The files are owned by system UID 0 with owner read/write and ordinary-user
read permissions. Ordinary user processes cannot create or replace entries
there. Fresh image creation installs the store. Incremental system image
updates validate and update these managed files while preserving unrelated
entries; an existing managed path with unexpected ownership or type makes the
update fail rather than being silently adopted.

## Provenance and updates

This snapshot is the Mozilla Root Store bundle distributed by curl CA Extract
at [`https://curl.se/ca/cacert.pem`](https://curl.se/ca/cacert.pem). The checked
snapshot is dated `Tue Oct 20 13:32:40 2026 GMT`, contains 118 certificates,
and was converted from Mozilla `certdata.txt` by `mk-ca-bundle.pl` version
1.35. The source `certdata.txt` digest recorded by curl is
`beb7e6dfe6499926e52c075c27bcfbe4c957f8609c575b3860273ae2806f63eb`; the
packaged PEM digest is
`796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe`.
Mozilla's [Root Store Policy](https://www.mozilla.org/en-US/about/governance/policies/security-group/certs/policy/)
and [root-store maintenance documentation](https://firefox-source-docs.mozilla.org/security/nss/runbooks/rootstore.html)
describe the upstream trust program. The bundle and its license are data; no
third-party TLS implementation is included. The bundle's upstream PEM comments
are preserved, and the accompanying MPL-2.0 license is retained.

To review a later snapshot, obtain `cacert.pem` from curl CA Extract, inspect
its date, Mozilla source digest, certificate changes, and upstream policy
status, then run:

```sh
python3 tools/ca_store.py import /path/to/reviewed-cacert.pem
python3 tools/ca_store.py verify
make test-ca-store
```

The importer validates PEM framing, base64/DER encoding, the upstream
provenance header, certificate count, and the checked license, then records a
manifest digest. It does not independently authenticate a downloaded source
or decide whether a root is trustworthy; source review and the signed Mangrove
change remain the trust decision. `verify` must pass before fresh or
incremental MGFS image preparation packages the files. Use the normal image
update tools to propagate an approved change. Runtime bundle replacement and
automatic network updates are not implemented.

curl's PEM extract is a list of roots, not the full NSS/Mozilla policy
database. It does not encode every root-program constraint or policy rule.
Future TLS validation must account for applicable constraints and must not
interpret mere presence in this file as permission to bypass certificate
chain, hostname, or validity checks.

## Time and TLS boundary

The kernel exposes realtime quality separately from the strict clock read.
Only a clock explicitly trusted as UTC is suitable for certificate validity
checks. Untrusted, unavailable, or invalid realtime remains a failure state;
monotonic uptime is not a wall clock. See [kernel timekeeping](../kernel/timekeeping.md)
and the [loam time API](../architecture/c-runtime.md).

The kernel provides neither TLS parsing nor certificate policy. The native
[mgTLS client](mgtls.md) loads this store and performs TLS 1.3 chain, hostname,
and certificate-time verification in userspace. `fetch` uses that interface
for HTTPS and fails closed when trusted realtime or a valid, accessible trust
store is unavailable. Other applications using TLS remain responsible for
handling verification errors and must not bypass them.

The packaged snapshot supplies trust anchors to mgTLS; it is not an independent
cryptographic assurance or a claim that the CA source's full policy metadata is
represented or enforced.
