#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include <time.h>
#include "crypto.h"
#include "storage.h"

/* 64 zeros: the previous_hash of the genesis block, and the tx_id of a block
 * that carries no transaction. */
#define ZERO_HASH "0000000000000000000000000000000000000000000000000000000000000000"
_Static_assert(sizeof(ZERO_HASH) == SHA256_HEX_LEN + 1, "ZERO_HASH must be 64 zeros");

/* Proof-of-work difficulty = number of leading '0' hex characters the block
 * hash must start with. Each extra zero makes mining ~16 times harder. */
#define MIN_DIFFICULTY 1
#define MAX_DIFFICULTY 4
#define DEFAULT_DIFFICULTY 2

#define MINER_NAME_MAX 40

#define ACTION_GENESIS  "GENESIS"
#define ACTION_BORROWED "BORROWED"
#define ACTION_RETURNED "RETURNED"
#define ACTION_OVERDUE  "OVERDUE"
#define ACTION_TRANSFER "TRANSFER"

typedef struct {
    /* ---- Formative 1 fields (unchanged) ---- */
    int index;
    time_t timestamp;
    char book_id[20];
    char book_title[80];
    char member_id[20];
    char member_name[50];
    char action[10];                /* BORROWED, RETURNED, OVERDUE, TRANSFER (GENESIS for block 0) */
    char recorded_by[KEY_NAME_MAX]; /* librarian whose key signed this block */
    char previous_hash[65];
    unsigned char signature[MAX_SIGNATURE_LEN];
    unsigned int sig_len;
    char hash[65];

    /* ---- Formative 2: token reward ---- */
    int token_reward;               /* coins earned: 10 on-time return, 5 late, 0 otherwise */
    char tx_id[65];                 /* SHA-256 ID of the reward (or transfer) transaction,
                                       ZERO_HASH when the block carries none */

    /* ---- Formative 2: proof of work, set by the miner ---- */
    int difficulty;                 /* leading zeros this block's hash was mined to */
    char mined_by[MINER_NAME_MAX];  /* who confirmed it: a user, "POOL" or "CLOUD:<user>" */
    unsigned long long nonce;       /* incremented until the hash meets the difficulty */
} Block;

typedef struct {
    Block *blocks;
    int count;
    int capacity;
} Blockchain;

typedef enum {
    CHAIN_VALID = 0,
    CHAIN_BAD_HASH,        /* stored hash != recomputed hash */
    CHAIN_BROKEN_LINK,     /* previous_hash != hash of the block before */
    CHAIN_BAD_SIGNATURE,   /* librarian's signature does not verify */
    CHAIN_BAD_POW          /* hash does not meet the block's difficulty */
} ChainValidationResult;

/* ---------------- Block content and signature ----------------
 * A lending event is signed by the librarian as soon as it is recorded, while
 * it is still in the pending pool. The signature covers the lending content
 * (timestamp, book, member, action, recorded_by, token_reward, tx_id) but not
 * the chain position, which only the miner decides. */

/* Fills a new unconfirmed block and signs its content. index is -1 until
 * mined. Returns 1 on success. */
int block_init_signed(Block *b, const char *book_id, const char *book_title,
                      const char *member_id, const char *member_name, const char *action,
                      int token_reward, const char *tx_id,
                      const char *signer, EVP_PKEY *signing_key);

/* Verifies the content signature with the signer's PUBLIC key. */
int block_signature_valid(const Block *b);

/* Recomputes the block hash: SHA-256 over index, previous_hash, content hash,
 * difficulty, mined_by, signature and nonce. */
void block_compute_hash(const Block *b, char out_hex[SHA256_HEX_LEN + 1]);

/* Force-terminates all strings (used after loading from disk). */
void block_sanitize(Block *b);

/* ---------------- Proof of work ---------------- */

/* Returns 1 if `hash_hex` starts with `difficulty` '0' characters and the
 * difficulty is in range. */
int hash_meets_difficulty(const char *hash_hex, int difficulty);

/* Average number of attempts needed at a difficulty: 16^difficulty. */
double pow_expected_attempts(int difficulty);

typedef struct {
    Sha256Prefix sha;
    int difficulty;
} PowJob;

#define POW_ERROR (-1)

/* Places `b` at chain position (index, previous_hash), records the
 * difficulty and miner, and prepares hashing. Returns 1 on success. */
int pow_begin(PowJob *job, Block *b, int index, const char *previous_hash,
              int difficulty, const char *miner);

/* Hashes the block with `nonce`. Returns 1 (and writes the hash to out_hash)
 * if it meets the difficulty, 0 if not, POW_ERROR if hashing failed. Does not
 * modify the block. */
int pow_try(PowJob *job, unsigned long long nonce, char out_hash[SHA256_HEX_LEN + 1]);

void pow_end(PowJob *job);

/* Mines `b` from nonce 0 until a valid hash is found and stores nonce and
 * hash in the block. Returns the number of attempts (0 on error). */
unsigned long long pow_mine(Block *b, int index, const char *previous_hash,
                            int difficulty, const char *miner);

/* ---------------- Chain ---------------- */

/* Initializes an empty chain in memory. Returns 0 if allocation fails. */
int chain_init(Blockchain *chain);

/* Frees the dynamic block array. */
void chain_free(Blockchain *chain);

/* Hash of the last block (ZERO_HASH for an empty chain). */
const char *chain_tip_hash(const Blockchain *chain);

/* Appends a mined block after checking it extends the tip and that its hash
 * is correct and meets its difficulty. Returns 1 on success. */
int chain_append(Blockchain *chain, const Block *b);

/* Removes the last block (used to undo an append whose transaction failed). */
void chain_pop(Blockchain *chain);

/* Checks block i: (a) stored hash == recomputed hash, (b) previous_hash ==
 * preceding block's hash (64 zeros for genesis), (c) the signature verifies
 * with the signer's public key, (d) the hash meets its difficulty. */
ChainValidationResult chain_check_block(const Blockchain *chain, int i);

/* Human-readable explanation of a validation failure. */
const char *chain_result_message(ChainValidationResult result);

/* Prints every confirmed block, including token reward and signature
 * validity. */
void chain_print_records(const Blockchain *chain);

/* Persists the chain (atomic temp-file + rename). Returns 1 on success. */
int chain_save(const Blockchain *chain, const char *path);

/* Loads the chain. Returns STORAGE_MISSING if the file does not exist yet. */
StorageResult chain_load(Blockchain *chain, const char *path);

/* Tamper demo 1: overwrites a block's action WITHOUT recomputing its hash.
 * Returns 1 if the index was valid. */
int chain_tamper_action(Blockchain *chain, int index, const char *new_action);

/* Tamper demo 2: changes a block's token reward and RE-MINES its hash, like
 * an attacker with hash power would. Returns the attempts used (0 on error). */
unsigned long long chain_tamper_reward(Blockchain *chain, int index, int new_reward);

#endif
