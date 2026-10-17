#ifndef APP_H
#define APP_H

#include "registry.h"
#include "auth.h"
#include "blockchain.h"
#include "pending.h"
#include "ledger.h"
#include "state.h"

/* Input registries (plain text, comma-separated) */
#define BOOKS_FILE    "books.txt"
#define MEMBERS_FILE  "members.txt"
#define COPIES_FILE   "copies.txt"
#define USERS_FILE    "users.dat"

/* Persisted state (binary, written after every mined block and on exit) */
#define CHAIN_FILE    "chain.dat"          /* every block with its lending records */
#define PENDING_FILE  "pending.dat"        /* the pending pool */
#define LEDGER_FILE   "ledger.dat"         /* every confirmed token transaction */
#define STATE_FILE    "library_state.dat"  /* copy inventory, reservation queues, nonces, fines */
#define BALANCES_FILE "balances.dat"       /* balance + account nonce of every account */
#define UTXO_FILE     "utxo.dat"           /* the UTXO set (UTXO sessions) */
#define META_FILE     "meta.dat"           /* difficulty, block reward, batch size, chain tip, ... */

/* Token economics */
#define REWARD_ON_TIME_COINS 10                 /* token_reward of an on-time return */
#define REWARD_LATE_COINS 5                     /* token_reward of a late return */
#define TX_FEE (1 * COIN)                       /* fixed fee on every reward / transfer */
#define DEFAULT_BLOCK_REWARD (50 * COIN)        /* paid to the miner(s) per confirmed block */
#define LIBRARY_INITIAL_SUPPLY (10000 * COIN)   /* treasury funded by the genesis block */

/* Fraud heuristics */
#define MAX_CONCURRENT_BORROWS 3                /* copies one member may hold at once */
#define REBORROW_WINDOW_SECONDS (24L * 3600L)   /* re-borrowing a title within 24 h of returning it */

/* Chain metadata, saved in meta.dat. */
typedef struct {
    int difficulty;
    Amount block_reward;
    int batch_size;               /* records per block (fixed-size batching) */
    long clock_offset;            /* seconds added by advance_days (simulated clock) */
    long next_request_seq;        /* next request_id number */
    int chain_height;             /* blocks when last saved */
    char tip_hash[SHA256_HEX_LEN + 1];
    time_t saved_at;
} ChainMeta;

/* Everything one run of the program works with. */
typedef struct {
    Registry reg;
    Session session;
    Blockchain chain;         /* confirmed blocks */
    PendingPool pending;      /* signed requests waiting to be mined */
    Ledger ledger;            /* token balances under the chosen model */
    LibraryState *state;      /* confirmed inventory / queues / nonces / fines */
    ChainMeta meta;
    int locked;               /* 1 while the loaded chain has not passed validate_chain */
} App;

/* The library clock: real time plus any days added with advance_days. */
time_t app_now(const App *app);

/* Loads the saved state (or creates the genesis block), runs validate_chain
 * on what was loaded and rebuilds the pending pool's reservations.
 * cli_difficulty / cli_batch override the saved values when > 0.
 * Returns 1 on success (the session may still be locked). */
int app_open(App *app, LedgerModel model, int cli_difficulty, int cli_batch);
void app_close(App *app);

/* chain_load: discards the in-memory state and loads everything again from
 * disk, then re-validates. Returns 1 if the reload succeeded. */
int app_reload(App *app);

/* Saves every file (refused while the session is locked). */
int app_save_all(App *app);
int app_save_pending(App *app);

/* validate_chain: every block's hash, link, proof of work, timestamp,
 * Merkle root and record signatures; replays every record through the
 * state machine (nonces, transitions); checks every token transaction a
 * block refers to. Prints problems (and a summary when verbose). If
 * `replayed` is given, it receives the state rebuilt from the chain.
 * Sets app->locked when problems are found. Returns the problem count. */
int app_validate(App *app, int verbose, LibraryState *replayed);

/* Prints an error and returns 0 when the session is locked / not ADMIN. */
int app_require_unlocked(const App *app);
int app_require_admin(const App *app);

/* Signer name of the logged-in librarian: LIB:<username>. */
void app_librarian_signer(const App *app, char *out, size_t size);

/* Ledger account of the logged-in user when they mine: MINER_<username>. */
void app_miner_account(const App *app, char *out, size_t size);

/* The state after applying every pending request (in submission order) to
 * the confirmed state: what a new request is checked against. */
void app_projected_state(const App *app, LibraryState *out);

/* Signs and submits a request to the pending pool:
 *   1. assigns request_id and timestamp, and the signer's next nonce
 *      (explicit_nonce >= 0 uses that nonce instead, to test replay protection);
 *   2. signs it with the signer's private key and verifies the signature;
 *   3. rejects a wrong nonce and any invalid state transition;
 *   4. runs the fraud heuristics: if one triggers the request is stored as
 *      SUSPICIOUS and withheld from mining;
 *   5. admits any attached token transaction (funds reserved).
 * req->rec must hold the record content; priority / waiting_since / tx must
 * be set. Returns 1 if the request entered the pool. */
int app_submit(App *app, PendingRequest *req, const char *signer, long explicit_nonce);

/* Re-checks the pool after a request was removed: later requests of the
 * same signer are re-signed with corrected nonces, and requests that are no
 * longer valid are dropped. Returns the number of changes. */
int app_revalidate_pool(App *app);

/* Removes pending request i (releasing its reserved funds). */
void app_drop_request(App *app, int i, const char *reason);

/* Re-admits every pending token transaction (after a restart or rollback). */
void app_readmit_pending(App *app);

/* Overdue detection: flags every BORROWED copy past its due date with a
 * librarian-signed OVERDUE request. Returns the number flagged. */
int app_overdue_scan(App *app, int verbose);

/* Creates and confirms a coinbase transaction paying `amount` of newly
 * mined coins to `account`. Returns 1 on success. */
int app_pay_coinbase(App *app, const char *account, Amount amount, const char *memo);

/* Timestamp for a new block: now, but strictly after the chain tip. */
time_t app_block_time(const App *app);

/* Days overdue (rounded up) of a due date at time `now`; 0 if not overdue. */
int days_overdue(time_t due_date, time_t now);

#endif
