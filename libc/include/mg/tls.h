/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

/* Initialize wolfSSL's library-level state. This is not a TLS handshake and
 * does not establish certificate trust. It fails if Mangrove's CSPRNG is not
 * currently ready. */
mg_result_t mgtls_init(void);

/* Release library-level state after all future TLS contexts and sessions have
 * been destroyed. A process using one-shot utilities normally need not call
 * this before process exit. */
mg_result_t mgtls_cleanup(void);

/* Return secure bytes from Mangrove's kernel CSPRNG. On any failure the whole
 * requested destination is cleared and the exact native error is returned.
 * A zero-length request succeeds; NULL is permitted only for zero length. */
mg_result_t mgtls_random_bytes(void *buffer, usize length);
