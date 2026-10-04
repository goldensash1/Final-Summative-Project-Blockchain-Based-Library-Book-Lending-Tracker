#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blockchain.h"

#define INITIAL_CAPACITY 16
#define MAX_CHAIN_BLOCKS 1000000   /* sanity bound when loading from disk */
#define CONTENT_BUF 512
#define HEADER_BUF 512
#define NONCE_BUF 24

static const char CHAIN_MAGIC[4] = {'L', 'B', 'C', '2'};

static void copy_field(char *dst, size_t dst_size, const char *src) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* ======================= Hashing ======================= */

/* The lending content: everything the librarian signs. */
static int build_content(const Block *b, char *buf, size_t size) {
    int n = snprintf(buf, size, "%ld|%s|%s|%s|%s|%s|%s|%d|%s",
                     (long)b->timestamp, b->book_id, b->book_title,
                     b->member_id, b->member_name, b->action, b->recorded_by,
                     b->token_reward, b->tx_id);
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

static int content_hash(const Block *b, char out_hex[SHA256_HEX_LEN + 1]) {
    char buf[CONTENT_BUF];
    int len = build_content(b, buf, sizeof(buf));
    if (len < 0) {
        out_hex[0] = '\0';
        return 0;
    }
    sha256_hex((const unsigned char *)buf, (size_t)len, out_hex);
    return out_hex[0] != '\0';
}

/* Everything the block hash commits to, except the nonce:
 *   index | previous_hash | content_hash | difficulty | mined_by | signature |
 * The nonce is appended last, so mining can reuse a SHA-256 midstate of this
 * prefix. Returns the prefix length, or -1 for a malformed block. */
static int build_header_prefix(const Block *b, unsigned char *buf, size_t size) {
    char chash[SHA256_HEX_LEN + 1];
    if (!content_hash(b, chash) || b->sig_len > MAX_SIGNATURE_LEN) return -1;

    int n = snprintf((char *)buf, size, "%d|%s|%s|%d|%s|",
                     b->index, b->previous_hash, chash, b->difficulty, b->mined_by);
    if (n < 0 || (size_t)n + b->sig_len + 1 >= size) return -1;

    memcpy(buf + n, b->signature, b->sig_len);
    n += (int)b->sig_len;
    buf[n++] = '|';
    return n;
}

void block_compute_hash(const Block *b, char out_hex[SHA256_HEX_LEN + 1]) {
    unsigned char buf[HEADER_BUF + NONCE_BUF];
    int n = build_header_prefix(b, buf, HEADER_BUF);
    if (n < 0) {
        /* Malformed block (e.g. unterminated strings from a corrupted file):
         * produce a hash that can never match a real one. */
        out_hex[0] = '\0';
        return;
    }
    n += snprintf((char *)buf + n, NONCE_BUF, "%llu", b->nonce);
    sha256_hex(buf, (size_t)n, out_hex);
}

int block_init_signed(Block *b, const char *book_id, const char *book_title,
                      const char *member_id, const char *member_name, const char *action,
                      int token_reward, const char *tx_id,
                      const char *signer, EVP_PKEY *signing_key) {
    memset(b, 0, sizeof(Block));
    b->index = -1;   /* assigned by the miner */
    b->timestamp = time(NULL);
    copy_field(b->book_id, sizeof(b->book_id), book_id);
    copy_field(b->book_title, sizeof(b->book_title), book_title);
    copy_field(b->member_id, sizeof(b->member_id), member_id);
    copy_field(b->member_name, sizeof(b->member_name), member_name);
    copy_field(b->action, sizeof(b->action), action);
    copy_field(b->recorded_by, sizeof(b->recorded_by), signer);
    b->token_reward = token_reward;
    copy_field(b->tx_id, sizeof(b->tx_id), tx_id ? tx_id : ZERO_HASH);

    char chash[SHA256_HEX_LEN + 1];
    return content_hash(b, chash) &&
           crypto_sign_hash(signing_key, chash, b->signature, &b->sig_len);
}

int block_signature_valid(const Block *b) {
    char chash[SHA256_HEX_LEN + 1];
    return content_hash(b, chash) &&
           crypto_verify_hash(b->recorded_by, chash, b->signature, b->sig_len);
}

void block_sanitize(Block *b) {
    b->book_id[sizeof(b->book_id) - 1] = '\0';
    b->book_title[sizeof(b->book_title) - 1] = '\0';
    b->member_id[sizeof(b->member_id) - 1] = '\0';
    b->member_name[sizeof(b->member_name) - 1] = '\0';
    b->action[sizeof(b->action) - 1] = '\0';
    b->recorded_by[sizeof(b->recorded_by) - 1] = '\0';
    b->previous_hash[sizeof(b->previous_hash) - 1] = '\0';
    b->hash[sizeof(b->hash) - 1] = '\0';
    b->tx_id[sizeof(b->tx_id) - 1] = '\0';
    b->mined_by[sizeof(b->mined_by) - 1] = '\0';
}

/* ======================= Proof of work ======================= */

int hash_meets_difficulty(const char *hash_hex, int difficulty) {
    if (difficulty < MIN_DIFFICULTY || difficulty > MAX_DIFFICULTY) return 0;
    if (strlen(hash_hex) != SHA256_HEX_LEN) return 0;
    for (int i = 0; i < difficulty; i++) {
        if (hash_hex[i] != '0') return 0;
    }
    return 1;
}

double pow_expected_attempts(int difficulty) {
    double attempts = 1.0;
    for (int i = 0; i < difficulty; i++) attempts *= 16.0;
    return attempts;
}

/* Same test as hash_meets_difficulty, on the raw digest: each hex character
 * is one 4-bit nibble. Avoids hex-encoding every failed attempt. */
static int digest_meets_difficulty(const unsigned char *digest, int difficulty) {
    for (int i = 0; i < difficulty; i++) {
        unsigned char nibble = (i % 2 == 0) ? (unsigned char)(digest[i / 2] >> 4)
                                            : (unsigned char)(digest[i / 2] & 0x0f);
        if (nibble != 0) return 0;
    }
    return 1;
}

int pow_begin(PowJob *job, Block *b, int index, const char *previous_hash,
              int difficulty, const char *miner) {
    b->index = index;
    copy_field(b->previous_hash, sizeof(b->previous_hash), previous_hash);
    b->difficulty = difficulty;
    copy_field(b->mined_by, sizeof(b->mined_by), miner);
    b->nonce = 0;
    b->hash[0] = '\0';
    job->difficulty = difficulty;

    unsigned char buf[HEADER_BUF];
    int n = build_header_prefix(b, buf, sizeof(buf));
    return n >= 0 && sha256_prefix_init(&job->sha, buf, (size_t)n);
}

int pow_try(PowJob *job, unsigned long long nonce, char out_hash[SHA256_HEX_LEN + 1]) {
    char suffix[NONCE_BUF];
    int n = snprintf(suffix, sizeof(suffix), "%llu", nonce);

    unsigned char digest[SHA256_DIGEST_LEN];
    if (!sha256_prefix_digest(&job->sha, (const unsigned char *)suffix, (size_t)n, digest)) {
        return POW_ERROR;
    }
    if (!digest_meets_difficulty(digest, job->difficulty)) return 0;

    if (out_hash) bytes_to_hex(digest, SHA256_DIGEST_LEN, out_hash);
    return 1;
}

void pow_end(PowJob *job) {
    sha256_prefix_free(&job->sha);
}

unsigned long long pow_mine(Block *b, int index, const char *previous_hash,
                            int difficulty, const char *miner) {
    PowJob job;
    if (!pow_begin(&job, b, index, previous_hash, difficulty, miner)) return 0;

    unsigned long long attempts = 0;
    for (unsigned long long nonce = 0;; nonce++) {
        attempts++;
        int r = pow_try(&job, nonce, b->hash);
        if (r == POW_ERROR) {
            attempts = 0;
            break;
        }
        if (r == 1) {
            b->nonce = nonce;
            break;
        }
    }
    pow_end(&job);
    return attempts;
}

/* ======================= Chain ======================= */

int chain_init(Blockchain *chain) {
    chain->blocks = malloc(sizeof(Block) * INITIAL_CAPACITY);
    chain->count = 0;
    chain->capacity = chain->blocks ? INITIAL_CAPACITY : 0;
    return chain->blocks != NULL;
}

void chain_free(Blockchain *chain) {
    free(chain->blocks);
    chain->blocks = NULL;
    chain->count = 0;
    chain->capacity = 0;
}

/* Ensures room for at least `needed` blocks. Returns 0 if out of memory
 * (the existing blocks are left untouched). */
static int chain_reserve(Blockchain *chain, int needed) {
    int new_capacity = chain->capacity > 0 ? chain->capacity : INITIAL_CAPACITY;
    while (new_capacity < needed) new_capacity *= 2;
    if (new_capacity == chain->capacity) return 1;

    Block *grown = realloc(chain->blocks, sizeof(Block) * (size_t)new_capacity);
    if (!grown) {
        printf("ERROR: Out of memory while growing the chain\n");
        return 0;
    }
    chain->blocks = grown;
    chain->capacity = new_capacity;
    return 1;
}

const char *chain_tip_hash(const Blockchain *chain) {
    return chain->count > 0 ? chain->blocks[chain->count - 1].hash : ZERO_HASH;
}

int chain_append(Blockchain *chain, const Block *b) {
    if (b->index != chain->count || strcmp(b->previous_hash, chain_tip_hash(chain)) != 0) {
        printf("ERROR: Block does not extend the current chain tip\n");
        return 0;
    }

    char recomputed[SHA256_HEX_LEN + 1];
    block_compute_hash(b, recomputed);
    if (recomputed[0] == '\0' || strcmp(recomputed, b->hash) != 0 ||
        !hash_meets_difficulty(b->hash, b->difficulty)) {
        printf("ERROR: Block hash is wrong or does not meet its proof-of-work target\n");
        return 0;
    }

    if (!chain_reserve(chain, chain->count + 1)) return 0;
    chain->blocks[chain->count++] = *b;
    return 1;
}

void chain_pop(Blockchain *chain) {
    if (chain->count > 0) chain->count--;
}

ChainValidationResult chain_check_block(const Blockchain *chain, int i) {
    const Block *b = &chain->blocks[i];

    char recomputed[SHA256_HEX_LEN + 1];
    block_compute_hash(b, recomputed);
    if (recomputed[0] == '\0' || strcmp(recomputed, b->hash) != 0) return CHAIN_BAD_HASH;

    const char *expected_prev = (i == 0) ? ZERO_HASH : chain->blocks[i - 1].hash;
    if (b->index != i || strcmp(b->previous_hash, expected_prev) != 0) return CHAIN_BROKEN_LINK;

    if (!block_signature_valid(b)) return CHAIN_BAD_SIGNATURE;

    if (!hash_meets_difficulty(b->hash, b->difficulty)) return CHAIN_BAD_POW;

    return CHAIN_VALID;
}

const char *chain_result_message(ChainValidationResult result) {
    switch (result) {
        case CHAIN_VALID:
            return "valid";
        case CHAIN_BAD_HASH:
            return "stored hash does not match its recomputed hash (data was changed after mining)";
        case CHAIN_BROKEN_LINK:
            return "previous_hash does not match the preceding block's hash (chain link broken)";
        case CHAIN_BAD_SIGNATURE:
            return "librarian's digital signature does not verify (content changed after signing)";
        case CHAIN_BAD_POW:
            return "hash does not meet its proof-of-work difficulty target";
    }
    return "unknown error";
}

static void format_timestamp(time_t t, char *buf, size_t size) {
    struct tm *tm_info = localtime(&t);
    if (!tm_info || strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info) == 0) {
        snprintf(buf, size, "%ld", (long)t);
    }
}

void chain_print_records(const Blockchain *chain) {
    if (chain->count == 0) {
        printf("(no blocks in chain)\n");
        return;
    }

    printf("\nConfirmed lending records (%d block(s)):\n", chain->count);
    printf("%-4s %-22s %-16s %-9s %-6s %-9s %-7s %-19s %-12s %-2s %-8s %s\n",
           "Idx", "Book Title", "Member Name", "Action", "Reward", "Tx ID", "SigOK",
           "Timestamp", "Mined By", "D", "Nonce", "Hash");
    for (int i = 0; i < 136; i++) putchar('-');
    putchar('\n');

    for (int i = 0; i < chain->count; i++) {
        const Block *b = &chain->blocks[i];
        char ts[32];
        format_timestamp(b->timestamp, ts, sizeof(ts));
        int has_tx = strcmp(b->tx_id, ZERO_HASH) != 0;

        printf("%-4d %-22.22s %-16.16s %-9.9s %-6d %-9.8s %-7s %-19s %-12.12s %-2d %-8llu %.16s...\n",
               b->index, b->book_title, b->member_name, b->action, b->token_reward,
               has_tx ? b->tx_id : "-", block_signature_valid(b) ? "VALID" : "INVALID",
               ts, b->mined_by, b->difficulty, b->nonce, b->hash);
    }
    printf("\n");
}

int chain_save(const Blockchain *chain, const char *path) {
    return storage_save(path, CHAIN_MAGIC, chain->blocks, sizeof(Block), chain->count);
}

StorageResult chain_load(Blockchain *chain, const char *path) {
    void *records = NULL;
    int count = 0;
    StorageResult rc = storage_load(path, CHAIN_MAGIC, sizeof(Block), MAX_CHAIN_BLOCKS,
                                    &records, &count);
    if (rc != STORAGE_OK) return rc;

    if (count < 1) {
        free(records);
        printf("ERROR: Chain file '%s' holds no blocks (not even genesis)\n", path);
        return STORAGE_ERROR;
    }

    free(chain->blocks);
    chain->blocks = records;
    chain->count = count;
    chain->capacity = count;

    /* Any real change still fails validation; this only stops reads past
     * the end of a field. */
    for (int i = 0; i < count; i++) block_sanitize(&chain->blocks[i]);
    return STORAGE_OK;
}

int chain_tamper_action(Blockchain *chain, int index, const char *new_action) {
    if (index < 0 || index >= chain->count) return 0;
    copy_field(chain->blocks[index].action, sizeof(chain->blocks[index].action), new_action);
    /* Deliberately NOT recomputing the hash -- this is the tamper demo. */
    return 1;
}

unsigned long long chain_tamper_reward(Blockchain *chain, int index, int new_reward) {
    if (index < 0 || index >= chain->count) return 0;
    Block *b = &chain->blocks[index];
    b->token_reward = new_reward;

    /* Re-mine at the same position, so the block's own hash and PoW are
     * valid again. Copies avoid passing a field as both source and target. */
    char prev[sizeof(b->previous_hash)], miner[sizeof(b->mined_by)];
    copy_field(prev, sizeof(prev), b->previous_hash);
    copy_field(miner, sizeof(miner), b->mined_by);
    return pow_mine(b, b->index, prev, b->difficulty, miner);
}
