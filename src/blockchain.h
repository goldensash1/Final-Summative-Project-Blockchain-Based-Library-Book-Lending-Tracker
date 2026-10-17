#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include <time.h>
#include "crypto.h"
#include "record.h"
#include "storage.h"

/* Proof-of-work difficulty = number of leading '0' hex characters the block
 * hash must start with. Each extra zero makes mining ~16 times harder. */
#define MIN_DIFFICULTY 1
#define MAX_DIFFICULTY 4
#define DEFAULT_DIFFICULTY 2

/* Records per block. Blocks are batched by a fixed size: the miner takes the
 * top `batch size` requests of the prioritised pending pool (configurable
 * from 1 to MAX_RECORDS_PER_BLOCK, default DEFAULT_BATCH_SIZE). */
#define MAX_RECORDS_PER_BLOCK 8
#define DEFAULT_BATCH_SIZE 3

#define MINER_NAME_MAX 40

typedef struct {
    /* ---- header ---- */
    int index;                          /* position in the chain (0 = genesis) */
    time_t timestamp;                   /* when the block was sealed (strictly increasing) */
    int record_count;                   /* lending records in this block */
    char previous_hash[SHA256_HEX_LEN + 1];
    char merkle_root[SHA256_HEX_LEN + 1];   /* root of the records' Merkle tree */
    char hash[SHA256_HEX_LEN + 1];      /* SHA-256 of all header fields + nonce */
    char sealed_by[MINER_NAME_MAX];     /* miner / librarian node that mined it */
    int token_reward;                   /* coins for this block's returns (10 on time, 5 late, summed) */
    char transaction_id[SHA256_HEX_LEN + 1]; /* SHA-256 ID of the reward transaction
                                           (of the reward batch when several returns) */
    int difficulty;                     /* leading zeros this block was mined to */
    unsigned long long nonce;           /* incremented until the hash meets the difficulty */

    /* ---- body ---- */
    LendingRecord records[MAX_RECORDS_PER_BLOCK];
    /* Reward transaction created by the miner for records[i] when it is a
     * RETURNED record (ZERO_HASH otherwise). transaction_id commits to these. */
    char reward_txids[MAX_RECORDS_PER_BLOCK][SHA256_HEX_LEN + 1];
} Block;

typedef struct {
    Block *blocks;
    int count;
    int capacity;
} Blockchain;

/* Validation problems of one block (bit flags). */
enum {
    BLOCK_OK            = 0,
    BLOCK_BAD_HASH      = 1 << 0,   /* stored hash != recomputed hash */
    BLOCK_BROKEN_LINK   = 1 << 1,   /* previous_hash != hash of the block before */
    BLOCK_BAD_POW       = 1 << 2,   /* hash does not meet the block's difficulty */
    BLOCK_BAD_MERKLE    = 1 << 3,   /* recomputed Merkle root != stored merkle_root */
    BLOCK_BAD_SIGNATURE = 1 << 4,   /* a record's ECDSA signature does not verify */
    BLOCK_BAD_SIGNER    = 1 << 5,   /* a record is signed by a key that is not the signer's */
    BLOCK_BAD_TIMESTAMP = 1 << 6,   /* timestamp not strictly greater than the previous block */
    BLOCK_BAD_SUMMARY   = 1 << 7,   /* token_reward / transaction_id / record_count inconsistent */
    BLOCK_BAD_INDEX     = 1 << 8
};

/* ---------------- Block content ---------------- */

/* Recomputes the block hash: SHA-256 over index, timestamp, record_count,
 * previous_hash, merkle_root, sealed_by, token_reward, transaction_id,
 * difficulty and nonce. The records are covered through merkle_root. */
void block_compute_hash(const Block *b, char out_hex[SHA256_HEX_LEN + 1]);

/* Computes token_reward and transaction_id from the records and
 * reward_txids. Returns 1 on success. */
int block_reward_summary(const Block *b, int *token_reward, char txid_out[SHA256_HEX_LEN + 1]);

/* Fills merkle_root, token_reward and transaction_id from the body.
 * Returns 1 on success. */
int block_finalize_body(Block *b);

/* Force-terminates all strings (used after loading from disk). */
void block_sanitize(Block *b);

/* ---------------- Proof of work ---------------- */

int hash_meets_difficulty(const char *hash_hex, int difficulty);

/* Average number of attempts needed at a difficulty: 16^difficulty. */
double pow_expected_attempts(int difficulty);

typedef struct {
    Sha256Prefix sha;
    int difficulty;
} PowJob;

#define POW_ERROR (-1)

/* Places `b` at chain position (index, previous_hash), records difficulty
 * and miner, and absorbs the fixed part of the header (SHA-256 midstate).
 * The block's timestamp must already be set. Returns 1 on success. */
int pow_begin(PowJob *job, Block *b, int index, const char *previous_hash,
              int difficulty, const char *miner);

/* Hashes the block with `nonce`. Returns 1 (writing the hash to out_hash) if
 * it meets the difficulty, 0 if not, POW_ERROR if hashing failed. */
int pow_try(PowJob *job, unsigned long long nonce, char out_hash[SHA256_HEX_LEN + 1]);

void pow_end(PowJob *job);

/* Mines `b` from nonce 0 until a valid hash is found. Returns the number of
 * attempts (0 on error). */
unsigned long long pow_mine(Block *b, int index, const char *previous_hash,
                            int difficulty, const char *miner);

/* ---------------- Chain ---------------- */

int chain_init(Blockchain *chain);
void chain_free(Blockchain *chain);
const char *chain_tip_hash(const Blockchain *chain);
time_t chain_tip_time(const Blockchain *chain);

/* Appends a mined block after checking it extends the tip, its Merkle root
 * and hash are correct and it meets its difficulty. Returns 1 on success. */
int chain_append(Blockchain *chain, const Block *b);

/* Removes the last block (used to undo an append whose transactions failed). */
void chain_pop(Blockchain *chain);

/* Structural and cryptographic checks of block i: hash, previous-hash link,
 * proof of work, Merkle root, every record's ECDSA signature and signer
 * identity, timestamp order and the reward summary. Prints each problem when
 * `print` is set. Returns a mask of BLOCK_* flags (0 = valid). */
int chain_check_block(const Blockchain *chain, int i, int print);

int chain_save(const Blockchain *chain, const char *path);
StorageResult chain_load(Blockchain *chain, const char *path);

/* Prints a summary row for every block. */
void chain_print_summary(const Blockchain *chain);

/* Prints one block in full: header, records and its Merkle tree. */
void chain_print_block(const Blockchain *chain, int index);

#endif
