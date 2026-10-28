/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

typedef struct mgtls_context mgtls_context_t;
typedef struct mgtls_session mgtls_session_t;

/* mgTLS operations are process-local and synchronous. The current wolfSSL
 * build uses SINGLE_THREADED and has no internal mutexes or atomic reference
 * counters. Callers must serialize every mgTLS operation within a process,
 * including initialization, contexts, sessions, callbacks, and cleanup.
 * Separate processes have independent library state. The system CA bundle is
 * loaded by context_create(). */
#define MGTLS_SYSTEM_CA_BUNDLE "/share/ca-certificates/ca-bundle.pem"
#define MGTLS_SYSTEM_CA_MANIFEST "/share/ca-certificates/manifest.json"
#define MGTLS_TIMEOUT_DEFAULT 5000U

/* Initialize wolfSSL's library-level state. This is not a TLS handshake and
 * does not establish certificate trust. It fails if Mangrove's CSPRNG is not
 * currently ready. Call once before creating contexts. */
mg_result_t mgtls_init(void);

/* Release library-level state after every context and session has been
 * destroyed. Returns MG_ERR_BUSY while contexts remain alive. */
mg_result_t mgtls_cleanup(void);

/* Return secure bytes from Mangrove's kernel CSPRNG. On any failure the whole
 * requested destination is cleared and the exact native error is returned.
 * A zero-length request succeeds; NULL is permitted only for zero length. */
mg_result_t mgtls_random_bytes(void *buffer, usize length);

/* Create a client context with the administrator-managed system CA store.
 * Trusted realtime is required because wolfSSL checks CA validity dates while
 * loading anchors. Missing files and access errors retain their native result;
 * malformed, empty, digest-mismatched, or incomplete stores return
 * MG_ERR_TRUST_STORE_INVALID. No caller-supplied roots or verification bypass
 * are supported by this API. */
mg_result_t mgtls_context_create(mgtls_context_t **out_context);
/* A context cannot be destroyed while sessions still refer to it. */
mg_result_t mgtls_context_destroy(mgtls_context_t *context);

/* Resolve a DNS name or parse a dotted-decimal IPv4 address, open one native
 * TCP stream, and complete an authenticated TLS 1.3 handshake. DNS names are
 * sent as SNI and matched against certificate subjectAltName DNS identities;
 * IPv4 literals are matched only against iPAddress subjectAltName entries.
 * The timeout covers DNS, TCP connection, and handshake using a monotonic
 * deadline. Zero is nonblocking; MGTLS_TIMEOUT_DEFAULT is five seconds. On
 * every failure the output is NULL and all acquired transport/crypto state is
 * released. Only one live native TCP connection is currently supported by
 * the kernel. */
mg_result_t mgtls_session_connect(mgtls_context_t *context,
                                  const char *server_name, u16 port,
                                  u32 timeout_ms,
                                  mgtls_session_t **out_session);

/* Authenticate TLS over an already connected Mangrove TCP stream. Ownership
 * of stream transfers to this call and is consumed on both success and
 * failure: a successful session closes it from mgtls_session_close(), while
 * a failed setup closes it before returning. The supplied name is still used
 * for SNI and DNS/IP subjectAltName verification. This entry point is useful
 * when an application needs to observe DNS and TCP setup separately from the
 * authenticated TLS handshake. */
mg_result_t mgtls_session_connect_stream(mgtls_context_t *context,
                                         const char *server_name,
                                         mg_handle_t stream,
                                         u32 timeout_ms,
                                         mgtls_session_t **out_session);

/* Encrypted stream operations transfer at most one wolfSSL application
 * operation per call (bounded by INT_MAX). On MG_OK, out_transferred is the
 * exact plaintext byte count accepted/returned and may be partial. On error
 * it is zero. A failed write may have transmitted an incomplete TLS record;
 * such a session is poisoned and must be closed. Receive timeout or
 * interruption leaves the session available for another read. A clean TLS
 * close_notify is reported as MG_ERR_CONNECTION_CLOSED. */
mg_result_t mgtls_session_send(mgtls_session_t *session, const void *data,
                               usize length, u32 timeout_ms,
                               usize *out_transferred);
mg_result_t mgtls_session_receive(mgtls_session_t *session, void *data,
                                  usize capacity, u32 timeout_ms,
                                  usize *out_transferred);

/* Send TLS close_notify within the supplied monotonic deadline. A successful
 * return means the local alert was submitted to TCP; peer close_notify is not
 * required. This does not consume the object. */
mg_result_t mgtls_session_shutdown(mgtls_session_t *session,
                                   u32 timeout_ms);
/* Free wolfSSL state and close the owned TCP handle. The session is consumed
 * unless the native close returns MG_ERR_BUSY, in which case the caller may
 * retry. */
mg_result_t mgtls_session_close(mgtls_session_t *session);
