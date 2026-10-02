/* rsa.h - verifying a release signature. See rsa.c for why RSA. */
#ifndef _LP_RSA_H
#define _LP_RSA_H

#include "types.h"

#define RSA2048_BYTES 256

/* Is sig_be a valid RSA-2048 PKCS#1 v1.5 signature, made with the
 * private key matching modulus_be (e = 65537), over the given 32-byte
 * SHA-256 digest?
 *
 * All three arguments are raw bytes, most significant first: the
 * modulus and the signature are RSA2048_BYTES each, the digest is 32.
 * Returns false for anything wrong at all - a bad signature, a
 * malformed key, padding that is off by a byte. There is no error
 * detail on purpose: to the caller there are only two outcomes, and
 * treating "nearly right" as different from "wrong" is how a check
 * turns into a suggestion. */
bool lp_rsa2048_verify(const u8 *modulus_be, const u8 *sig_be,
                       const u8 *sha256);

#endif
