#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include <openssl/evp.h>

#define SHA256_HEX_LEN 64   /* hex characters, plus '\0' = 65 */
#define SHA256_DIGEST_LEN 32
#define MAX_SIGNATURE_LEN 72
#define KEYS_DIR "keys"
#define KEY_NAME_MAX 32     /* also the size of Block.recorded_by */

/* Computes the SHA-256 hash of `data` (length `len`) and writes it as a
 * lowercase hex string (65 bytes incl. terminator) into `out_hex`. */
void sha256_hex(const unsigned char *data, size_t len, char out_hex[SHA256_HEX_LEN + 1]);

/* Writes `len` bytes as lowercase hex (2*len chars + '\0') into `out`. */
void bytes_to_hex(const unsigned char *in, size_t len, char *out);

/* SHA-256 "midstate" for proof-of-work. Between mining attempts only the
 * nonce changes, so the fixed part of the block header is absorbed once and
 * each attempt only hashes the short nonce suffix on a copy of that state.
 * The result is identical to SHA-256(prefix || suffix). */
typedef struct {
    EVP_MD_CTX *base;   /* state after absorbing the prefix */
    EVP_MD_CTX *work;   /* scratch context reused by every attempt */
} Sha256Prefix;

/* Absorbs `prefix`. Returns 1 on success (free with sha256_prefix_free). */
int sha256_prefix_init(Sha256Prefix *p, const unsigned char *prefix, size_t len);

/* Computes the raw digest of prefix || suffix. Returns 1 on success. */
int sha256_prefix_digest(Sha256Prefix *p, const unsigned char *suffix, size_t len,
                         unsigned char out[SHA256_DIGEST_LEN]);

void sha256_prefix_free(Sha256Prefix *p);

/* Returns 1 if `name` is 3-31 chars of [A-Za-z0-9_]. Usernames double as key
 * file names, so this also guarantees they cannot escape KEYS_DIR. */
int crypto_valid_key_name(const char *name);

/* Generates a new ECDSA (P-256) key pair for `username`:
 *   keys/<username>_private.pem  AES-256 encrypted with `password`, mode 0600
 *   keys/<username>_public.pem   plain PEM, used by anyone to verify
 * Returns 1 on success. */
int crypto_generate_user_keys(const char *username, const char *password);

/* Decrypts and loads `username`'s private signing key using `password`.
 * Returns NULL if the key file is missing or the password is wrong.
 * Free with EVP_PKEY_free. */
EVP_PKEY *crypto_load_signing_key(const char *username, const char *password);

/* Signs the SHA-256 hex digest `hash_hex` with `signing_key`. Writes the
 * DER-encoded signature into `sig_out` (capacity MAX_SIGNATURE_LEN) and its
 * length into `sig_len_out`. Returns 1 on success. */
int crypto_sign_hash(EVP_PKEY *signing_key,
                     const char hash_hex[SHA256_HEX_LEN + 1],
                     unsigned char sig_out[MAX_SIGNATURE_LEN],
                     unsigned int *sig_len_out);

/* Verifies `signature` over `hash_hex` using the PUBLIC key of `signer`
 * (loaded from keys/<signer>_public.pem and cached). Returns 1 if valid. */
int crypto_verify_hash(const char *signer,
                       const char hash_hex[SHA256_HEX_LEN + 1],
                       const unsigned char *signature,
                       unsigned int sig_len);

/* Releases all cached public keys. */
void crypto_cleanup(void);

#endif
