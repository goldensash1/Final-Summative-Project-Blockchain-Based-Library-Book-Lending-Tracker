#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/core_names.h>
#include <openssl/params.h>

#include "crypto.h"

#define PUBKEY_CACHE_SIZE 64

typedef struct {
    char name[KEY_NAME_MAX];
    EVP_PKEY *key;
} CachedPublicKey;

static CachedPublicKey g_pub_cache[PUBKEY_CACHE_SIZE];
static int g_pub_cache_count = 0;

void sha256_hex(const unsigned char *data, size_t len, char out_hex[SHA256_HEX_LEN + 1]) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx ||
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        /* Never leave the output undefined: an empty hash never matches. */
        out_hex[0] = '\0';
        EVP_MD_CTX_free(ctx);
        return;
    }
    EVP_MD_CTX_free(ctx);

    bytes_to_hex(digest, digest_len, out_hex);
}

void bytes_to_hex(const unsigned char *in, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

int sha256_prefix_init(Sha256Prefix *p, const unsigned char *prefix, size_t len) {
    p->base = EVP_MD_CTX_new();
    p->work = EVP_MD_CTX_new();
    if (!p->base || !p->work ||
        EVP_DigestInit_ex(p->base, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(p->base, prefix, len) != 1) {
        sha256_prefix_free(p);
        return 0;
    }
    return 1;
}

int sha256_prefix_digest(Sha256Prefix *p, const unsigned char *suffix, size_t len,
                         unsigned char out[SHA256_DIGEST_LEN]) {
    unsigned int out_len = 0;
    return EVP_MD_CTX_copy_ex(p->work, p->base) == 1 &&
           EVP_DigestUpdate(p->work, suffix, len) == 1 &&
           EVP_DigestFinal_ex(p->work, out, &out_len) == 1 &&
           out_len == SHA256_DIGEST_LEN;
}

void sha256_prefix_free(Sha256Prefix *p) {
    EVP_MD_CTX_free(p->base);
    EVP_MD_CTX_free(p->work);
    p->base = NULL;
    p->work = NULL;
}

int crypto_valid_key_name(const char *name) {
    size_t len = strlen(name);
    if (len < 3 || len >= KEY_NAME_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)name[i]) && name[i] != '_') return 0;
    }
    return 1;
}

static void key_path(const char *username, const char *kind, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s_%s.pem", KEYS_DIR, username, kind);
}

static EVP_PKEY *generate_ec_keypair(void) {
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (!pctx) return NULL;

    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_group_name(pctx, "prime256v1") <= 0 ||
        EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return NULL;
    }
    EVP_PKEY_CTX_free(pctx);
    return pkey;
}

/* Opens `path` for writing with owner-only permissions (0600), so a private
 * key is never readable by other users, even for an instant. */
static FILE *open_owner_only(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return NULL;
    fchmod(fd, 0600); /* in case the file already existed with wider perms */
    FILE *fp = fdopen(fd, "w");
    if (!fp) close(fd);
    return fp;
}

int crypto_generate_user_keys(const char *username, const char *password) {
    if (!crypto_valid_key_name(username)) return 0;

    if (mkdir(KEYS_DIR, 0700) != 0 && errno != EEXIST) {
        printf("ERROR: Could not create key directory '%s'\n", KEYS_DIR);
        return 0;
    }

    EVP_PKEY *pkey = generate_ec_keypair();
    if (!pkey) {
        printf("ERROR: Failed to generate ECDSA key pair\n");
        return 0;
    }

    char priv_path[128], pub_path[128];
    key_path(username, "private", priv_path, sizeof(priv_path));
    key_path(username, "public", pub_path, sizeof(pub_path));

    int ok = 0;
    FILE *fpriv = open_owner_only(priv_path);
    if (fpriv) {
        ok = PEM_write_PrivateKey(fpriv, pkey, EVP_aes_256_cbc(),
                                  (const unsigned char *)password, (int)strlen(password),
                                  NULL, NULL);
        if (fclose(fpriv) != 0) ok = 0;
    }

    if (ok) {
        FILE *fpub = fopen(pub_path, "w");
        ok = fpub && PEM_write_PUBKEY(fpub, pkey);
        if (fpub && fclose(fpub) != 0) ok = 0;
    }

    EVP_PKEY_free(pkey);
    if (!ok) {
        printf("ERROR: Failed to save key pair for '%s'\n", username);
        remove(priv_path);
        remove(pub_path);
    }
    return ok;
}

EVP_PKEY *crypto_load_signing_key(const char *username, const char *password) {
    if (!crypto_valid_key_name(username)) return NULL;

    char priv_path[128];
    key_path(username, "private", priv_path, sizeof(priv_path));

    FILE *fp = fopen(priv_path, "r");
    if (!fp) return NULL;
    /* With no callback, OpenSSL uses the last argument as the passphrase. */
    EVP_PKEY *key = PEM_read_PrivateKey(fp, NULL, NULL, (void *)password);
    fclose(fp);
    ERR_clear_error();
    return key;
}

static EVP_PKEY *get_public_key(const char *signer) {
    if (!crypto_valid_key_name(signer)) return NULL;

    for (int i = 0; i < g_pub_cache_count; i++) {
        if (strcmp(g_pub_cache[i].name, signer) == 0) return g_pub_cache[i].key;
    }

    char pub_path[128];
    key_path(signer, "public", pub_path, sizeof(pub_path));
    FILE *fp = fopen(pub_path, "r");
    if (!fp) return NULL;
    EVP_PKEY *key = PEM_read_PUBKEY(fp, NULL, NULL, NULL);
    fclose(fp);
    if (!key) return NULL;

    if (g_pub_cache_count < PUBKEY_CACHE_SIZE) {
        CachedPublicKey *c = &g_pub_cache[g_pub_cache_count++];
        strncpy(c->name, signer, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = '\0';
        c->key = key;
    }
    /* If the cache is full the key is simply not cached (and leaks until
     * exit); 64 distinct librarians is far beyond this demo's scope. */
    return key;
}

int crypto_sign_hash(EVP_PKEY *signing_key,
                     const char hash_hex[SHA256_HEX_LEN + 1],
                     unsigned char sig_out[MAX_SIGNATURE_LEN],
                     unsigned int *sig_len_out) {
    if (!signing_key) return 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;

    size_t sig_len = MAX_SIGNATURE_LEN;
    int ok = 0;
    if (EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, signing_key) == 1 &&
        EVP_DigestSign(ctx, sig_out, &sig_len,
                       (const unsigned char *)hash_hex, strlen(hash_hex)) == 1) {
        *sig_len_out = (unsigned int)sig_len;
        ok = 1;
    }

    EVP_MD_CTX_free(ctx);
    return ok;
}

int crypto_verify_hash(const char *signer,
                       const char hash_hex[SHA256_HEX_LEN + 1],
                       const unsigned char *signature,
                       unsigned int sig_len) {
    if (sig_len == 0 || sig_len > MAX_SIGNATURE_LEN) return 0;

    EVP_PKEY *pub = get_public_key(signer);
    if (!pub) return 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;

    int result = 0;
    if (EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pub) == 1) {
        int rc = EVP_DigestVerify(ctx, signature, sig_len,
                                  (const unsigned char *)hash_hex, strlen(hash_hex));
        result = (rc == 1);
    }

    EVP_MD_CTX_free(ctx);
    ERR_clear_error();
    return result;
}

/* ======================= Summative additions ======================= */

int sha256_raw(const unsigned char *data, size_t len, unsigned char out[SHA256_DIGEST_LEN]) {
    unsigned int out_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = ctx &&
             EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, data, len) == 1 &&
             EVP_DigestFinal_ex(ctx, out, &out_len) == 1 &&
             out_len == SHA256_DIGEST_LEN;
    EVP_MD_CTX_free(ctx);
    if (!ok) memset(out, 0, SHA256_DIGEST_LEN);
    return ok;
}

int crypto_pubkey_hex(EVP_PKEY *key, char out[PUBKEY_HEX_LEN + 1]) {
    unsigned char point[PUBKEY_HEX_LEN / 2 + 8];
    size_t len = 0;
    out[0] = '\0';
    if (!key || EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, point,
                                                sizeof(point), &len) != 1 ||
        len != PUBKEY_HEX_LEN / 2) {
        ERR_clear_error();
        return 0;
    }
    bytes_to_hex(point, len, out);
    return 1;
}

static int hex_to_bytes(const char *hex, unsigned char *out, size_t out_len) {
    if (strlen(hex) != out_len * 2) return 0;
    for (size_t i = 0; i < out_len; i++) {
        int hi = isxdigit((unsigned char)hex[2 * i]), lo = isxdigit((unsigned char)hex[2 * i + 1]);
        if (!hi || !lo) return 0;
        char pair[3] = {hex[2 * i], hex[2 * i + 1], '\0'};
        out[i] = (unsigned char)strtoul(pair, NULL, 16);
    }
    return 1;
}

/* Builds an EC public key object from the on-chain hex encoding. */
static EVP_PKEY *pubkey_from_hex(const char *pubkey_hex) {
    unsigned char point[PUBKEY_HEX_LEN / 2];
    if (!hex_to_bytes(pubkey_hex, point, sizeof(point))) return NULL;

    OSSL_PARAM params[3];
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)"prime256v1", 0);
    params[1] = OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point));
    params[2] = OSSL_PARAM_construct_end();

    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (!ctx || EVP_PKEY_fromdata_init(ctx) <= 0 ||
        EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
        pkey = NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    ERR_clear_error();
    return pkey;
}

/* Small cache of parsed on-chain keys: validation verifies every record of
 * the chain, and most records come from a handful of signers. */
#define HEXKEY_CACHE_SIZE 256
typedef struct {
    char hex[PUBKEY_HEX_LEN + 1];
    EVP_PKEY *key;
} CachedHexKey;
static CachedHexKey g_hex_cache[HEXKEY_CACHE_SIZE];
static int g_hex_cache_count = 0;

static int verify_with_key(EVP_PKEY *pub, const char *hash_hex,
                           const unsigned char *signature, unsigned int sig_len) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;
    int result = EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pub) == 1 &&
                 EVP_DigestVerify(ctx, signature, sig_len, (const unsigned char *)hash_hex,
                                  strlen(hash_hex)) == 1;
    EVP_MD_CTX_free(ctx);
    ERR_clear_error();
    return result;
}

int crypto_verify_hash_pubhex(const char *pubkey_hex,
                              const char hash_hex[SHA256_HEX_LEN + 1],
                              const unsigned char *signature, unsigned int sig_len) {
    if (sig_len == 0 || sig_len > MAX_SIGNATURE_LEN || strlen(pubkey_hex) != PUBKEY_HEX_LEN) return 0;

    for (int i = 0; i < g_hex_cache_count; i++) {
        if (strcmp(g_hex_cache[i].hex, pubkey_hex) == 0) {
            return verify_with_key(g_hex_cache[i].key, hash_hex, signature, sig_len);
        }
    }

    EVP_PKEY *pub = pubkey_from_hex(pubkey_hex);
    if (!pub) return 0;
    int ok = verify_with_key(pub, hash_hex, signature, sig_len);
    if (g_hex_cache_count < HEXKEY_CACHE_SIZE) {
        CachedHexKey *c = &g_hex_cache[g_hex_cache_count++];
        snprintf(c->hex, sizeof(c->hex), "%s", pubkey_hex);
        c->key = pub;
    } else {
        EVP_PKEY_free(pub);
    }
    return ok;
}

/* ----------------------- Member keystore ----------------------- */

#define MEMBER_ID_FILE_MAX 20
#define MEMBER_KEY_CACHE_SIZE 256

typedef struct {
    char signer[48];
    char pubhex[PUBKEY_HEX_LEN + 1];
    EVP_PKEY *priv;           /* member private key (NULL for librarians) */
} KeystoreEntry;
static KeystoreEntry g_keystore[MEMBER_KEY_CACHE_SIZE];
static int g_keystore_count = 0;

static void member_key_path(const char *member_id, const char *kind, char *out, size_t size) {
    snprintf(out, size, "%s/%s_%s.pem", MEMBER_KEYS_DIR, member_id, kind);
}

/* Member IDs become file names, so only letters, digits, '-' and '_' are
 * accepted (no path separators). */
static int safe_file_id(const char *id) {
    size_t len = strlen(id);
    if (len == 0 || len >= MEMBER_ID_FILE_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)id[i]) && id[i] != '_' && id[i] != '-') return 0;
    }
    return 1;
}

int keystore_ensure_member(const char *member_id, int *created) {
    if (created) *created = 0;
    if (!safe_file_id(member_id)) return 0;

    char priv_path[128], pub_path[128];
    member_key_path(member_id, "private", priv_path, sizeof(priv_path));
    member_key_path(member_id, "public", pub_path, sizeof(pub_path));
    FILE *probe = fopen(pub_path, "r");
    if (probe) {
        fclose(probe);
        return 1;
    }

    if ((mkdir(KEYS_DIR, 0700) != 0 && errno != EEXIST) ||
        (mkdir(MEMBER_KEYS_DIR, 0700) != 0 && errno != EEXIST)) {
        printf("ERROR: Could not create key directory '%s'\n", MEMBER_KEYS_DIR);
        return 0;
    }

    EVP_PKEY *pkey = generate_ec_keypair();
    if (!pkey) return 0;

    int ok = 0;
    FILE *fpriv = open_owner_only(priv_path);
    if (fpriv) {
        ok = PEM_write_PrivateKey(fpriv, pkey, NULL, NULL, 0, NULL, NULL);
        if (fclose(fpriv) != 0) ok = 0;
    }
    if (ok) {
        FILE *fpub = fopen(pub_path, "w");
        ok = fpub && PEM_write_PUBKEY(fpub, pkey);
        if (fpub && fclose(fpub) != 0) ok = 0;
    }
    EVP_PKEY_free(pkey);
    if (!ok) {
        remove(priv_path);
        remove(pub_path);
        printf("ERROR: Could not save the key pair of member %s\n", member_id);
        return 0;
    }
    if (created) *created = 1;
    return 1;
}

static KeystoreEntry *keystore_lookup(const char *signer) {
    for (int i = 0; i < g_keystore_count; i++) {
        if (strcmp(g_keystore[i].signer, signer) == 0) return &g_keystore[i];
    }
    return NULL;
}

/* Loads (and caches) a signer's registered public key, plus the private key
 * for members. */
static KeystoreEntry *keystore_load(const char *signer) {
    KeystoreEntry *e = keystore_lookup(signer);
    if (e) return e;
    if (g_keystore_count >= MEMBER_KEY_CACHE_SIZE) return NULL;

    int is_librarian = strncmp(signer, LIBRARIAN_SIGNER_PREFIX, strlen(LIBRARIAN_SIGNER_PREFIX)) == 0;
    char pub_path[128], priv_path[128];
    EVP_PKEY *pub = NULL, *priv = NULL;

    if (is_librarian) {
        const char *user = signer + strlen(LIBRARIAN_SIGNER_PREFIX);
        pub = get_public_key(user);   /* cached by the librarian key cache */
        if (!pub) return NULL;
    } else {
        if (!safe_file_id(signer)) return NULL;
        member_key_path(signer, "public", pub_path, sizeof(pub_path));
        member_key_path(signer, "private", priv_path, sizeof(priv_path));
        FILE *fp = fopen(pub_path, "r");
        if (!fp) return NULL;
        pub = PEM_read_PUBKEY(fp, NULL, NULL, NULL);
        fclose(fp);
        fp = fopen(priv_path, "r");
        if (fp) {
            priv = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
            fclose(fp);
        }
        ERR_clear_error();
        if (!pub) {
            EVP_PKEY_free(priv);
            return NULL;
        }
    }

    e = &g_keystore[g_keystore_count];
    memset(e, 0, sizeof(*e));
    snprintf(e->signer, sizeof(e->signer), "%s", signer);
    int ok = crypto_pubkey_hex(pub, e->pubhex);
    if (!is_librarian) EVP_PKEY_free(pub);
    if (!ok) {
        EVP_PKEY_free(priv);
        return NULL;
    }
    e->priv = priv;
    g_keystore_count++;
    return e;
}

EVP_PKEY *keystore_member_key(const char *member_id) {
    KeystoreEntry *e = keystore_load(member_id);
    return e ? e->priv : NULL;
}

int keystore_registered_pubkey(const char *signer, char out[PUBKEY_HEX_LEN + 1]) {
    KeystoreEntry *e = keystore_load(signer);
    if (!e) {
        out[0] = '\0';
        return 0;
    }
    snprintf(out, PUBKEY_HEX_LEN + 1, "%s", e->pubhex);
    return 1;
}

void crypto_cleanup(void) {
    for (int i = 0; i < g_pub_cache_count; i++) {
        EVP_PKEY_free(g_pub_cache[i].key);
    }
    g_pub_cache_count = 0;
    for (int i = 0; i < g_hex_cache_count; i++) EVP_PKEY_free(g_hex_cache[i].key);
    g_hex_cache_count = 0;
    for (int i = 0; i < g_keystore_count; i++) EVP_PKEY_free(g_keystore[i].priv);
    g_keystore_count = 0;
}
