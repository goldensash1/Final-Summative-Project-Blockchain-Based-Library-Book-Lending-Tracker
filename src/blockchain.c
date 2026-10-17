#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blockchain.h"
#include "merkle.h"

#define INITIAL_CAPACITY 16
#define MAX_CHAIN_BLOCKS 1000000   /* sanity bound when loading from disk */
#define HEADER_BUF 512
#define NONCE_BUF 24

/* "LBC3": the summative block layout (multi-record blocks with a Merkle
 * root). Files from Formative 1/2 ("LBC1"/"LBC2") are refused on load. */
static const char CHAIN_MAGIC[4] = {'L', 'B', 'C', '3'};

static void copy_field(char *dst, size_t dst_size, const char *src) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* ======================= Hashing ======================= */

/* Everything the block hash commits to, except the nonce. The nonce is
 * appended last so mining can reuse a SHA-256 midstate of this prefix.
 * Returns the prefix length, or -1 for a malformed block. */
static int build_header_prefix(const Block *b, char *buf, size_t size) {
    int n = snprintf(buf, size, "%d|%ld|%d|%s|%s|%s|%d|%s|%d|",
                     b->index, (long)b->timestamp, b->record_count, b->previous_hash,
                     b->merkle_root, b->sealed_by, b->token_reward, b->transaction_id,
                     b->difficulty);
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

void block_compute_hash(const Block *b, char out_hex[SHA256_HEX_LEN + 1]) {
    char buf[HEADER_BUF + NONCE_BUF];
    int n = build_header_prefix(b, buf, HEADER_BUF);
    if (n < 0) {
        out_hex[0] = '\0';   /* never matches a real hash */
        return;
    }
    n += snprintf(buf + n, NONCE_BUF, "%llu", b->nonce);
    sha256_hex((const unsigned char *)buf, (size_t)n, out_hex);
}

int block_reward_summary(const Block *b, int *token_reward, char txid_out[SHA256_HEX_LEN + 1]) {
    if (b->record_count < 0 || b->record_count > MAX_RECORDS_PER_BLOCK) return 0;

    int reward = 0, rewards = 0;
    char concat[MAX_RECORDS_PER_BLOCK * SHA256_HEX_LEN + 1] = "";
    const char *single = NULL;
    for (int i = 0; i < b->record_count; i++) {
        if (b->records[i].action != ACT_RETURNED) continue;
        reward += b->records[i].token_reward;
        if (strcmp(b->reward_txids[i], ZERO_HASH) == 0) continue;
        single = b->reward_txids[i];
        strcat(concat, b->reward_txids[i]);
        rewards++;
    }
    *token_reward = reward;

    /* One return: transaction_id is that reward transaction's SHA-256 ID.
     * Several returns: the SHA-256 of their IDs in block order. */
    if (rewards == 0) copy_field(txid_out, SHA256_HEX_LEN + 1, ZERO_HASH);
    else if (rewards == 1) copy_field(txid_out, SHA256_HEX_LEN + 1, single);
    else sha256_hex((const unsigned char *)concat, strlen(concat), txid_out);
    return txid_out[0] != '\0';
}

int block_finalize_body(Block *b) {
    return merkle_root(b->records, b->record_count, b->merkle_root) &&
           block_reward_summary(b, &b->token_reward, b->transaction_id);
}

void block_sanitize(Block *b) {
    b->previous_hash[sizeof(b->previous_hash) - 1] = '\0';
    b->merkle_root[sizeof(b->merkle_root) - 1] = '\0';
    b->hash[sizeof(b->hash) - 1] = '\0';
    b->sealed_by[sizeof(b->sealed_by) - 1] = '\0';
    b->transaction_id[sizeof(b->transaction_id) - 1] = '\0';
    if (b->record_count < 0 || b->record_count > MAX_RECORDS_PER_BLOCK) b->record_count = 0;
    for (int i = 0; i < MAX_RECORDS_PER_BLOCK; i++) {
        record_sanitize(&b->records[i]);
        b->reward_txids[i][SHA256_HEX_LEN] = '\0';
    }
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

/* Same test as hash_meets_difficulty on the raw digest: each hex character
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
    copy_field(b->sealed_by, sizeof(b->sealed_by), miner);
    b->nonce = 0;
    b->hash[0] = '\0';
    job->difficulty = difficulty;

    char buf[HEADER_BUF];
    int n = build_header_prefix(b, buf, sizeof(buf));
    return n >= 0 && sha256_prefix_init(&job->sha, (const unsigned char *)buf, (size_t)n);
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

time_t chain_tip_time(const Blockchain *chain) {
    return chain->count > 0 ? chain->blocks[chain->count - 1].timestamp : 0;
}

int chain_append(Blockchain *chain, const Block *b) {
    if (b->index != chain->count || strcmp(b->previous_hash, chain_tip_hash(chain)) != 0) {
        printf("ERROR: Block does not extend the current chain tip\n");
        return 0;
    }
    if (chain->count > 0 && b->timestamp <= chain_tip_time(chain)) {
        printf("ERROR: Block timestamp is not later than the previous block\n");
        return 0;
    }

    char root[SHA256_HEX_LEN + 1], recomputed[SHA256_HEX_LEN + 1];
    if (!merkle_root(b->records, b->record_count, root) || strcmp(root, b->merkle_root) != 0) {
        printf("ERROR: Block Merkle root does not match its records\n");
        return 0;
    }
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

int chain_check_block(const Blockchain *chain, int i, int print) {
    const Block *b = &chain->blocks[i];
    int problems = BLOCK_OK;

    if (b->index != i) {
        problems |= BLOCK_BAD_INDEX;
        if (print) printf("  Block #%d: stored index is %d\n", i, b->index);
    }

    /* 1. Recomputed block hash against the stored one. */
    char recomputed[SHA256_HEX_LEN + 1];
    block_compute_hash(b, recomputed);
    if (recomputed[0] == '\0' || strcmp(recomputed, b->hash) != 0) {
        problems |= BLOCK_BAD_HASH;
        if (print) printf("  Block #%d: stored hash does not match the recomputed hash "
                          "(header changed after mining)\n", i);
    }

    /* 2. Hash-pointer link to the previous block. */
    const char *expected_prev = (i == 0) ? ZERO_HASH : chain->blocks[i - 1].hash;
    if (strcmp(b->previous_hash, expected_prev) != 0) {
        problems |= BLOCK_BROKEN_LINK;
        if (print) printf("  Block #%d: previous_hash does not match block #%d's hash "
                          "(chain link broken)\n", i, i - 1);
    }

    /* 3. Proof of work. */
    if (!hash_meets_difficulty(b->hash, b->difficulty)) {
        problems |= BLOCK_BAD_POW;
        if (print) printf("  Block #%d: hash does not meet its difficulty %d\n", i, b->difficulty);
    }

    /* 4. Timestamps strictly increase. */
    if (i > 0 && b->timestamp <= chain->blocks[i - 1].timestamp) {
        problems |= BLOCK_BAD_TIMESTAMP;
        if (print) printf("  Block #%d: timestamp is not later than block #%d's\n", i, i - 1);
    }

    if (b->record_count < 0 || b->record_count > MAX_RECORDS_PER_BLOCK ||
        (i == 0 && b->record_count != 0) || (i > 0 && b->record_count == 0)) {
        problems |= BLOCK_BAD_SUMMARY;
        if (print) printf("  Block #%d: invalid record_count %d\n", i, b->record_count);
        return problems;
    }

    /* 5. Record integrity: recompute the Merkle root from the records. */
    char root[SHA256_HEX_LEN + 1];
    if (!merkle_root(b->records, b->record_count, root) || strcmp(root, b->merkle_root) != 0) {
        problems |= BLOCK_BAD_MERKLE;
        if (print) printf("  Block #%d: Merkle root recomputed from its %d record(s) does not "
                          "match merkle_root (a record was changed)\n", i, b->record_count);
    }

    /* 6. Every record's ECDSA signature, and that it was made with the
     *    signer's registered key. */
    for (int r = 0; r < b->record_count; r++) {
        const LendingRecord *rec = &b->records[r];
        if (!record_signature_valid(rec)) {
            problems |= BLOCK_BAD_SIGNATURE;
            if (print) printf("  Block #%d: record %s (%s): ECDSA signature by %s does not "
                              "verify\n", i, rec->record_id, record_action_name(rec->action),
                              rec->signer);
        } else if (!record_signer_registered(rec)) {
            problems |= BLOCK_BAD_SIGNER;
            if (print) printf("  Block #%d: record %s is signed by a key that is not %s's "
                              "registered public key\n", i, rec->record_id, rec->signer);
        }
    }

    /* 7. token_reward and transaction_id summarise the records. Genesis
     *    instead references the treasury coinbase (checked by the app). */
    if (i > 0) {
        int reward;
        char txid[SHA256_HEX_LEN + 1];
        if (!block_reward_summary(b, &reward, txid) || reward != b->token_reward ||
            strcmp(txid, b->transaction_id) != 0) {
            problems |= BLOCK_BAD_SUMMARY;
            if (print) printf("  Block #%d: token_reward/transaction_id do not match its "
                              "RETURNED records\n", i);
        }
    }
    return problems;
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
    for (int i = 0; i < count; i++) block_sanitize(&chain->blocks[i]);
    return STORAGE_OK;
}

void chain_print_summary(const Blockchain *chain) {
    printf("\nBlockchain: %d block(s)\n", chain->count);
    printf("%-4s %-16s %4s %-14s %-14s %-12s %6s %-10s %2s %8s %s\n", "Idx", "Timestamp", "Recs",
           "Prev hash", "Merkle root", "Sealed by", "Reward", "Tx ID", "D", "Nonce", "Hash");
    for (int i = 0; i < 128; i++) putchar('-');
    putchar('\n');
    for (int i = 0; i < chain->count; i++) {
        const Block *b = &chain->blocks[i];
        char ts[24];
        format_datetime(b->timestamp, ts, sizeof(ts));
        int has_tx = strcmp(b->transaction_id, ZERO_HASH) != 0;
        printf("%-4d %-16s %4d %.12s.. %.12s.. %-12.12s %6d %-10.8s %2d %8llu %.20s...\n",
               b->index, ts, b->record_count, b->previous_hash, b->merkle_root, b->sealed_by,
               b->token_reward, has_tx ? b->transaction_id : "-", b->difficulty, b->nonce, b->hash);
    }
    printf("Use 'chain_view <index>' to see a block's records and Merkle tree.\n");
}

void chain_print_block(const Blockchain *chain, int index) {
    if (index < 0 || index >= chain->count) {
        printf("ERROR: No block #%d (chain has %d block(s))\n", index, chain->count);
        return;
    }
    const Block *b = &chain->blocks[index];
    char ts[24];
    format_datetime(b->timestamp, ts, sizeof(ts));
    printf("\n=== Block #%d ===\n", b->index);
    printf("  timestamp      : %s (%ld)\n", ts, (long)b->timestamp);
    printf("  record_count   : %d\n", b->record_count);
    printf("  previous_hash  : %s\n", b->previous_hash);
    printf("  merkle_root    : %s\n", b->merkle_root);
    printf("  hash           : %s\n", b->hash);
    printf("  sealed_by      : %s\n", b->sealed_by);
    printf("  token_reward   : %d\n", b->token_reward);
    printf("  transaction_id : %s\n", b->transaction_id);
    printf("  difficulty     : %d   nonce: %llu\n", b->difficulty, b->nonce);
    if (b->record_count > 0) {
        printf("  Records:\n");
        record_print_header("    ");
        for (int r = 0; r < b->record_count; r++) {
            const LendingRecord *rec = &b->records[r];
            record_print_row(rec, "    ");
            printf("      signature %s (key %.16s...), condition %s%s%s",
                   record_signature_valid(rec) ? "VALID" : "INVALID", rec->signer_pubkey,
                   condition_name(rec->condition), rec->reason_code[0] ? ", reason " : "",
                   rec->reason_code);
            if (strcmp(b->reward_txids[r], ZERO_HASH) != 0) {
                printf(", reward tx %.16s...", b->reward_txids[r]);
            }
            if (strcmp(rec->tx_id, ZERO_HASH) != 0) printf(", tx %.16s...", rec->tx_id);
            printf("\n");
        }
    }
    merkle_print_tree(b->records, b->record_count);
}
