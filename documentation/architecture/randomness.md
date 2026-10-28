# Cryptographic random bytes

Pith exposes kernel-generated cryptographic random bytes through the native
loam API in `libc/include/mg/random.h`:

- `mg_random_status()` reports whether the generator can currently serve a
  request. It returns `MG_OK` or `MG_ERR_ENTROPY_UNAVAILABLE`; checking status
  may initialize or reseed the generator.
- `mg_random_bytes(buffer, length)` fills the entire requested range and
  returns `MG_OK` only on complete success. A zero-length request succeeds
  without requiring readiness, and `NULL` is valid only for that case. The
  kernel validates the complete destination as writable before writing.
  Invalid or unwritable ranges return `MG_ERR_BAD_ARGUMENT`; missing or failed
  entropy returns `MG_ERR_ENTROPY_UNAVAILABLE`. On an error, some leading
  bytes may already have been written if a later reseed fails; callers must
  discard the entire requested range.

These functions use syscall numbers 100 (status) and 101 (fill). The kernel
does not allocate memory for a request. Its practical request bounds are the
caller's mapped writable address range, available CPU time, and process
address arithmetic. Work is divided into 256-byte DRBG Generate calls; the
state lock is released between chunks. There is no smaller public byte-count
ceiling or silent truncation.

## Generator and initialization

The generator is HMAC_DRBG with SHA-256, following NIST SP 800-90A Rev. 1,
Section 10.1.2. Pith uses the Mangrove SHA-256 and HMAC implementation. At
instantiation it collects 32 bytes for entropy input and 16 bytes for nonce;
each reseed obtains 32 additional bytes. It reseeds before generation once the
DRBG request counter reaches 10,000. With the kernel's 256-byte chunks this
means a reseed is required at intervals of at most approximately 2.56 MiB.
The primitive rejects a single Generate request larger than 65,536 bytes,
although the kernel service uses smaller chunks.

The optional `INIT_CSPRNG` service is attempted after scheduler initialization
and before AP startup. Lack of entropy does not prevent boot. A later status or
byte request retries initialization. Reseed failure clears the generator
state; the kernel does not continue serving bytes from a state whose required
reseed failed. The generator state is global to the kernel, protected by an
IRQ-safe lock, and is never returned or logged. Processes receive only bytes
written to their validated destination. A scoped user-memory lease keeps the
destination mapping valid during the request, including while another worker
runs. A failed request may have written a prefix; callers must discard the
entire requested range.

## Entropy source and trust assumptions

On x86-64, Pith prefers RDSEED and falls back to RDRAND when RDSEED is absent
or repeatedly reports no ready word. CPUID support is checked on the executing
CPU immediately before the instruction. Local interrupts are disabled across
that check and instruction so migration cannot execute an unsupported opcode
on a different CPU. Each instruction is retried at most 16 times. An
unsupported source or one that never returns a word is not usable; if both
paths fail, the API returns `MG_ERR_ENTROPY_UNAVAILABLE`. No timestamp, fixed
seed, libc `rand()`, or fabricated fallback is used.

This policy trusts the advertised RDSEED/RDRAND implementation and its
successful carry status to supply unpredictable seed material. Pith has no
independent physical entropy estimator, entropy-source health test, EFI RNG,
TPM source, or independent second hardware source. RDSEED and RDRAND on one
processor are not independent sources. A successful hardware instruction is
therefore a platform trust assumption, not a measurement of physical entropy
or proof against a defective, malicious, or virtualized source. If no
supported source returns data, the API fails closed.

## Verification limits

The production HMAC_DRBG is tested against a NIST SP 800-90A/CAVP HMAC_DRBG
SHA-256 known-answer case. SHA-256 and HMAC also have independent host
reference checks. These tests establish agreement for covered vectors and
failure paths; they are not CAVP validation, cryptographic certification, or
an independent implementation audit. NIST describes its test vectors as
informal checks that do not replace formal validation. Mangrove does not
currently claim production cryptographic assurance for this implementation
or its hardware entropy sources.

The interface supplies randomness to Mangrove's new [mgTLS
foundation](../security/mgtls.md). Its custom wolfCrypt random callback
propagates entropy failures and clears any partially written request before
returning. The library currently exposes initialization and random-byte
foundation APIs only; no TLS handshake or application TLS behavior is
available yet.

References:

- [NIST SP 800-90A Rev. 1](https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-90Ar1.pdf),
  HMAC_DRBG, Section 10.1.2.
- [NIST CAVP random-number-generator testing](https://csrc.nist.gov/Projects/Cryptographic-Algorithm-Validation-Program/Random-Number-Generators),
  including the limits on assurance provided by test vectors.

Authoritative implementation: `kernel/src/entropy.c`,
`kernel/src/entropy_x86.c`, `kernel/src/crypto/`, `kernel/src/csprng.c`,
`kernel/src/random_syscall.c`, and `libc/src/random.c`.
