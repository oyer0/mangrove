# mgTLS

mgTLS is Mangrove's native source integration of wolfSSL. Its TLS 1.3 client
API builds the upstream TLS/X.509 implementation into `libmgtls.a`, adapts it
to loam, the native TCP stream interface, Mangrove's trusted realtime, and the
system CA store. It provides authenticated encrypted sessions for native
userspace programs. `fetch` uses this API for authenticated HTTPS downloads.

## Upstream provenance and license

The imported snapshot is the official wolfSSL **5.9.4** source release, dated
September 25, 2026. The source archive is named `wolfssl-5.9.4.zip`; its
SHA-256 is
`9cd0d69d208d025b16836c177658cc0deebc2a85c4b60898c9bc546bc6cbdf18`.
The archive was obtained from the [official wolfSSL download page](https://www.wolfssl.com/download/).
`libmgtls/upstream/wolfssl-5.9.4/COPYING` and `LICENSING` are the upstream
license texts. wolfSSL is distributed under GPL version 3 with the narrow
upstream exception listed in `LICENSING`; the imported source is not
relicensed as GPL-3.0-or-later. Mangrove-authored integration files use the
repository's GPL-3.0-or-later terms. Original per-file copyright and license
notices remain in imported files.

`MG_IMPORT.json` records the archive identity, digest, imported components,
exclusions, and exact file inventory. The snapshot contains wolfSSL TLS/X.509
sources and headers plus wolfCrypt's generic C sources and headers. The build
selects only the client TLS sources and crypto primitives needed by this
configuration; platform-specific `wolfcrypt/src/port` implementations,
examples, generated files, and build products are not compiled or imported
as source code.

The importer is `tools/import_mgtls_upstream.py`. For a later reviewed release:

1. Obtain the official source archive and independently review its release and
   per-file notices.
2. Verify the published SHA-256, then import to a new versioned directory:

   ```sh
   python3 tools/import_mgtls_upstream.py /path/to/wolfssl-X.Y.Z.zip \
     --version X.Y.Z --sha256 PUBLISHED_64_HEX_SHA256 \
     --destination libmgtls/upstream/wolfssl-X.Y.Z
   ```

3. Compare upstream source and licensing changes with the existing snapshot.
   Reconcile `user_settings.h`, the selected source list, and any isolated
   platform adaptations. Do not overwrite an existing snapshot or edit
   imported upstream files for routine platform integration.
4. Run the host sanitizer/vector suite, target archive and consumer link,
   and bounded native smoke test before changing the selected version.

The integration is kept outside the imported tree so the next upstream source
update can be re-imported and compared without replaying a mass symbol rename.
wolfSSL internal and library symbols retain their upstream names; the public
Mangrove platform API is under `mgtls_*`. No independent Mangrove rewrite of
wolfSSL cryptographic code is claimed.

## Configuration and algorithms

`libmgtls/include/user_settings.h` is the custom wolfSSL configuration. It
enables TLS 1.3 client code and X.509 certificate support, and disables TLS
1.2 and older TLS, DTLS, server mode, PSK, session/client caches, filesystem
helpers, and wolfSSL socket I/O. The selected cryptographic sources include
SHA-256/SHA-384/SHA-512 support, HMAC/HKDF, AES-GCM, ChaCha20-Poly1305, RSA
with PSS, ECC, X25519, Ed25519, and the arithmetic required by those
implementations. TLS 1.3 method symbols are present in the archive; TLS 1.2
client method symbols are absent.

The build uses wolfSSL's documented custom-platform settings and
`WOLFSSL_USER_IO`. `mgtls_session_connect()` resolves a DNS name or accepts an
IPv4 literal, opens one native TCP stream, and performs a TLS 1.3 client
handshake. DNS names are sent as SNI and verified against DNS subjectAltName
entries. IPv4 literals are verified against iPAddress subjectAltName entries;
they are not treated as DNS names. Common Name fallback is disabled.

The session loads and checks the installed CA bundle and manifest when a
context is created. Peer verification is mandatory. wolfSSL's certificate-time
callback reads Mangrove trusted UTC and fails closed if it is not trusted,
available, or representable. The [trusted realtime contract](../kernel/timekeeping.md)
and [system CA store](ca-trust-store.md) describe those sources. The bundle
does not preserve Mozilla policy metadata such as distrust-after dates or
purpose-specific constraints; mgTLS currently uses the included certificates
as trust anchors, subject to wolfSSL chain, validity, and hostname checks.
Applications must not interpret a failed session setup as authenticated.

wolfCrypt's `CUSTOM_RAND_GENERATE_BLOCK` is connected to
`mg_random_bytes()`. wolfSSL's own DRBG is not seeded from a time, libc random
function, or fallback source; Mangrove's kernel CSPRNG is the source. The
adapter clears the complete requested range if the native random call fails,
because the native API may have written a prefix before reporting an error.
`mgtls_init()` checks `mg_random_status()` before wolfSSL initialization, and
generation still fails closed if readiness is lost later.

## Public API and concurrency contract

Include `<mg/tls.h>` (also available through `<mangrove.h>`):

- `mgtls_init()` checks secure-random readiness, installs the trusted UTC
  callback, and initializes wolfSSL's process-local library state.
- `mgtls_random_bytes(buffer, length)` provides fail-closed secure bytes;
  errors clear the full destination.
- `mgtls_context_create()` requires trusted UTC, loads the system CA store, and
  creates a TLS 1.3 client context. wolfSSL validates anchor dates as it loads
  them, so unavailable/untrusted time is returned before trust-store errors.
  This call does not create directories or modify trust data.
- `mgtls_session_connect()` owns a TCP stream after it is opened and returns
  only after authenticated handshake completion. A monotonic deadline covers
  DNS, TCP connect, and TLS handshake; the default is five seconds.
- `mgtls_session_send()` and `mgtls_session_receive()` report exact transferred
  plaintext counts. A successful call may be partial. A failed write poisons
  the session because wolfSSL requires retrying a pending write with the same
  buffer; close it instead of changing the pending input. Read timeout and
  interruption preserve the session for a later read.
- `mgtls_session_shutdown()` submits local `close_notify`; `mgtls_session_close()`
  releases TLS state and its owned TCP handle.
- `mgtls_cleanup()` releases wolfSSL library state only after all contexts have
  been destroyed.

The public declarations are in `<mg/tls.h>`. wolfSSL types and internal
symbols are private to the library build. `fetch` creates an mgTLS client
context lazily when an HTTPS URL is encountered, uses the URL hostname or IPv4
literal for identity verification, and streams the HTTP request and response
through encrypted session I/O. Its HTTP path continues to use native TCP
directly. HTTPS requires the system CA bundle and trusted UTC; there is no
verification bypass or fallback to plaintext. Redirects may move from HTTP to
HTTPS, while HTTPS-to-HTTP redirects are rejected. `fetch` is limited to TLS
1.3 and the current native IPv4 TCP API. The system CA bundle does not carry
Mozilla policy metadata such as distrust-after dates.

wolfSSL 5.9.4's `SINGLE_THREADED` configuration disables mutex operations and
uses ordinary, non-atomic reference counters. This matches Mangrove's current
process model: each process has one userspace thread, and its address space
contains its own wolfSSL and mgTLS global state. wolfSSL's static
certificate-time callback and library initialization count are therefore
process-local, not shared by independent processes. Separate processes can
create independent mgTLS contexts and sessions without racing through
wolfSSL's globals; however, Mangrove's TCP implementation currently permits
only one live connection system-wide, so it limits simultaneous network
sessions independently of wolfSSL. Within one process, applications must
serialize all mgTLS calls and must not call wolfSSL directly. Sequential use
of multiple contexts/sessions follows the API lifecycle contract.

The no-lock configuration is not a promise of thread safety. If Mangrove adds
multiple userspace threads in one process, mgTLS must move to the platform's
wolfSSL mutex/atomic integration and protect its own initialization, time
callback error, and context-lifetime state before concurrent API use. Merely
removing `SINGLE_THREADED` would not provide that integration.

The client build includes SHA-384 for certificate-signature verification and
wolfSSL alternate-chain path selection. Alternate-chain handling permits a
cross-signed intermediate/root candidate to be skipped only when the peer
certificate still verifies to a configured trusted root. Host fixtures cover
a cross-signed path with the matching trusted root and the same path with an
untrusted root; the latter remains rejected. The selected single-precision
RSA implementation supports 4096-bit certificate-signature verification
because current trusted public chains can include a cross-signed root signed
by a 4096-bit RSA CA. Larger RSA sizes are not enabled by this profile. A
certificate signature verification failure is reported as
`MG_ERR_CERTIFICATE_SIGNATURE`; other chain, hostname, or validity failures
remain `MG_ERR_CERTIFICATE_INVALID` (trusted-time failures retain their
specific time error). These distinctions do not change verification policy.

`WOLFSSL_SMALL_STACK` moves selected large wolfSSL temporaries to the process
heap. The documented native user stack is 32 KiB; wolfSSL and trust-store
allocations use loam's process heap and can fail. TLS session establishment is
synchronous and currently uses the kernel's one-active-TCP-connection
facility. A successful native TLS handshake exercises the actual user stack,
heap, trusted clock, CA bundle, CSPRNG, and TCP integration, but does not
establish physical-hardware or cryptographic certification.

The bounded two-vCPU Q35/OVMF TLS smoke run measured a 5,352-byte user-stack
watermark and a 348,112-byte peak of intercepted dynamic allocation requests;
the tracked live allocation total returned to zero after context/session
cleanup. The test watermark covers the documented 32 KiB stack and samples it
after the handshake, so the reported stack high-water includes a small amount
of measurement/cleanup call depth. The heap counter wraps userspace
`malloc`/`calloc`/`realloc`/`free` for the test executable and includes its
tracking headers, but excludes allocator metadata, statically reserved memory,
and kernel-side TCP or process allocations. These are one successful example
profile, not a maximum or a low-memory guarantee.

`make mgtls` builds `build/userspace/libmgtls.a`; `make binaries` includes that
archive in the standard target build. The test-only `mgtlstest` program is not
part of normal image payloads.

## Verification and limits

`make test-mgtls` runs upstream SHA-256, AES-GCM, and X25519 known-answer
checks, entropy adapter tests, and production-source TLS 1.3 client fixtures
under ASan/UBSan. The local fixture checks DNS and IP SAN matching, trusted and
untrusted roots, expired and future certificates, chain completeness,
cross-signed paths requiring 4096-bit RSA verification, corrupted
certificate-signature rejection, missing or corrupt CA stores, untrusted
realtime, timeout, interruption, reset, and encrypted application-data
exchange. LeakSanitizer is disabled where the host
cannot initialize it. Functional tests are not a cryptographic certification,
wolfSSL security audit, or proof of upstream implementation correctness.

`make test-guest-mgtls` builds a disposable two-vCPU Q35/OVMF guest and runs
the cryptographic smoke tests against Mangrove's CSPRNG. The runner independently
checks the stopped MGFS image. A separate `make test-guest-mgtls-tls` workflow
performs an authenticated TLS 1.3 exchange to `example.com` using the guest's
trusted clock and packaged CA store, then checks the stopped image. Public
network availability and the remote endpoint's current certificate chain are
external test dependencies. These checks do not qualify physical hardware,
concurrent threads within a process, low-memory fault injection on the target,
or production security.

`make test-guest-fetch-https` exercises `/bin/fetch` for both HTTP and
authenticated HTTPS in a disposable two-vCPU Q35/OVMF guest. It uses a kernel
built with trusted UTC, normal QEMU user networking, the packaged system CA
bundle, and the remote site's current certificate chain. The runner validates
downloaded content and independently checks the stopped MGFS image. It does
not qualify physical networking, certificates outside the installed CA
snapshot, or every possible server TLS configuration.
