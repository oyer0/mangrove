# mgTLS foundation

mgTLS is a native Mangrove source integration of wolfSSL. This Phase 1
foundation builds the upstream TLS 1.3 client and X.509 code into
`libmgtls.a`, adapts its platform boundary to loam, and verifies selected
cryptographic implementations. It does **not** yet perform a TLS handshake,
connect sockets, load the system trust store, or apply certificate/hostname
policy. It is not yet suitable for authenticated HTTPS applications.

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
`WOLFSSL_USER_IO`; the future transport adapter must provide explicit read and
write callbacks. This phase does not provide networking callbacks, certificate
loading, root selection, hostname checks, or trust decisions. The existing
[trusted realtime contract](../kernel/timekeeping.md) and
[system CA store](ca-trust-store.md) are not yet wired into wolfSSL
verification. Until that work is complete, applications must not treat this
foundation as authenticated transport.

wolfCrypt's `CUSTOM_RAND_GENERATE_BLOCK` is connected to
`mg_random_bytes()`. wolfSSL's own DRBG is not seeded from a time, libc random
function, or fallback source; Mangrove's kernel CSPRNG is the source. The
adapter clears the complete requested range if the native random call fails,
because the native API may have written a prefix before reporting an error.
`mgtls_init()` checks `mg_random_status()` before wolfSSL initialization, and
generation still fails closed if readiness is lost later.

## Public API and resource model

Include `<mg/tls.h>` (also available through `<mangrove.h>`):

- `mgtls_init()` checks secure-random readiness and initializes wolfSSL's
  process-local library state.
- `mgtls_random_bytes(buffer, length)` provides the same fail-closed secure
  byte source for Mangrove TLS callers; errors clear the full destination.
- `mgtls_cleanup()` releases wolfSSL library state after contexts have been
  destroyed.

These are foundation APIs, not a session API. A native `mgtls_ctx` interface
will be defined when transport callbacks and mandatory certificate policy can
be enforced together. Applications should not call wolfSSL internals through
the private build include path.

Mangrove currently runs one userspace thread per process. `SINGLE_THREADED`
matches that execution model; concurrent use of one mgTLS process/library
instance is not promised. Independent processes use independent wolfSSL state
and the kernel random service's SMP synchronization. `WOLFSSL_SMALL_STACK`
moves selected large wolfSSL temporaries to the process heap. The documented
native user stack is 32 KiB; wolfSSL's dynamic allocations use loam's process
heap and can fail. Allocation-failure behavior during complete TLS handshakes
has not yet been qualified, because handshakes are not implemented here.

`make mgtls` builds `build/userspace/libmgtls.a`; `make binaries` includes that
archive in the standard target build. The test-only `mgtlstest` program is not
part of normal image payloads.

## Verification and limits

`make test-mgtls` runs upstream SHA-256, AES-GCM, and X25519 known-answer
checks under ASan/UBSan; checks large random requests and output clearing on
partial entropy failure; compiles a TLS 1.3 client API probe; and checks the
archive's TLS-version symbols. LeakSanitizer is disabled where the host cannot
initialize it. These vectors and functional tests are not a cryptographic
certification, wolfSSL security audit, or proof of upstream implementation
correctness.

`make test-guest-mgtls` builds a disposable two-vCPU Q35/OVMF guest and runs
those cryptographic operations against the actual Mangrove CSPRNG interface.
The runner independently checks the stopped MGFS image. This does not qualify
physical hardware, TLS record exchange, network behavior, certificate-chain
validation, hostname matching, trusted-time integration, low-memory handshake
behavior, or production security.
