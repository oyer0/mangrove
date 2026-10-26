/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MANGROVE_MGTLS_USER_SETTINGS_H
#define MANGROVE_MGTLS_USER_SETTINGS_H

/* wolfSSL custom-platform configuration for Mangrove userspace.  Keep this
 * list intentionally small and review it against wolfSSL's settings.h when
 * updating the upstream snapshot.  TLS is client-only and TLS 1.3-only here;
 * certificate parsing and verification remain enabled. */
#define WOLFSSL_TLS13
#define WOLFSSL_NO_TLS12
#define HAVE_TLS_EXTENSIONS
#define WC_RSA_PSS
#define WOLFSSL_HARDEN_TLS 112
#define WOLFSSL_EXTRA_ALERTS
#define WOLFSSL_CHECK_ALERT_ON_ERR
#define ECC_TIMING_RESISTANT
#define TFM_TIMING_RESISTANT
#define WC_RSA_BLINDING
#define NO_OLD_TLS
#define NO_DTLS
#define NO_WOLFSSL_SERVER
#define NO_PSK
#define NO_SESSION_CACHE
#define NO_CLIENT_CACHE

/* Mangrove I/O and heap come from loam; TLS code will use custom callbacks.
 * The CSPRNG callback below calls mg_random_bytes and has no host/device
 * entropy fallback. */
#define NO_FILESYSTEM
#define NO_WOLFSSL_DIR
#define NO_WRITEV
#define WOLFSSL_USER_IO
#define WOLFSSL_NO_SOCK
#define NO_DEV_RANDOM
#define NO_DEV_URANDOM
#define SINGLE_THREADED
#define WOLFSSL_SMALL_STACK
#define NO_STDATOMIC_H
#define CUSTOM_RAND_GENERATE_BLOCK mgtls_wolfssl_random_block
int mgtls_wolfssl_random_block(unsigned char *output, unsigned int length);

/* Retain RSA/ECC certificate verification, P-256/P-384, X25519 and Ed25519.
 * TLS 1.3 AEAD suites include AES-GCM and ChaCha20-Poly1305.  Legacy hashes
 * and algorithms not required by this client profile stay disabled. */
#define HAVE_ECC
#define HAVE_CURVE25519
#define HAVE_ED25519
#define WOLFSSL_SHA512
#define HAVE_AESGCM
#define HAVE_CHACHA
#define HAVE_POLY1305
#define HAVE_HKDF
#define WOLFSSL_AES_256
#define NO_DSA
#define NO_DH
#define NO_DES3
#define NO_RC4
#define NO_MD4
#define NO_MD5
#define NO_SHA
#define USE_WOLF_STRCASECMP

#endif
