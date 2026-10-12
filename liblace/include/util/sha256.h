/*
 * Lace
 * SHA-256 and HMAC-SHA256 (TCP mutual authentication)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_SHA256_H
#define LACE_SHA256_H

#include "../lace_export.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LACE_BEGIN_DECLS

#define LACE_SHA256_LEN 32

void lace_sha256(const void *data, size_t len, uint8_t out[LACE_SHA256_LEN]);
void lace_hmac_sha256(const void *key, size_t key_len, const void *msg,
                      size_t msg_len, uint8_t out[LACE_SHA256_LEN]);

/* The TCP handshake proof: lowercase hex of
 * HMAC-SHA256(token, label ":" client_nonce ":" daemon_nonce).
 * The daemon proves itself with label "laced-daemon", the client with
 * "lace-client", so neither proof can be replayed as the other. out holds
 * 2 * LACE_SHA256_LEN + 1 bytes. */
void lace_auth_proof(const char *token, const char *label,
                     const char *client_nonce, const char *daemon_nonce,
                     char out[2 * LACE_SHA256_LEN + 1]);

/* Constant-time string comparison */
bool lace_secure_equal(const char *a, const char *b);

LACE_END_DECLS

#endif /* LACE_SHA256_H */
