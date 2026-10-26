/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <wolfssl/ssl.h>

#if !defined(WOLFSSL_TLS13) || !defined(WOLFSSL_NO_TLS12) || \
    !defined(NO_WOLFSSL_SERVER)
#error mgTLS must remain a client-only TLS 1.3 configuration
#endif

/* Compile-time API probe; the archive link and native guest test exercise the
 * actual objects. Keeping a typed reference catches settings/header drift. */
WOLFSSL_METHOD *(*mgtls_tls13_client_method_probe)(void) =
    wolfTLSv1_3_client_method;
