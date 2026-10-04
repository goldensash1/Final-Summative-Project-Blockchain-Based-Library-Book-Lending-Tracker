#ifndef APP_H
#define APP_H

#include "registry.h"
#include "auth.h"
#include "blockchain.h"
#include "pending.h"
#include "ledger.h"

#define BOOKS_FILE   "books.txt"
#define MEMBERS_FILE "members.txt"
#define USERS_FILE   "users.dat"
#define CHAIN_FILE   "chain.dat"
#define PENDING_FILE "pending.dat"
#define LEDGER_FILE  "ledger.dat"

#define LOAN_PERIOD_DAYS 14

/* Token economics */
#define REWARD_ON_TIME_COINS 10                 /* token_reward of an on-time return */
#define REWARD_LATE_COINS 5                     /* token_reward of a late return */
#define TX_FEE (1 * COIN)                       /* fixed fee on every reward / transfer */
#define BLOCK_REWARD (50 * COIN)                /* paid to the miner(s) per confirmed block */
#define LIBRARY_INITIAL_SUPPLY (10000 * COIN)   /* treasury funded by the genesis block */

/* Everything one run of the program works with. */
typedef struct {
    Registry reg;
    Session session;
    Blockchain chain;        /* confirmed blocks (chain.dat) */
    PendingPool pending;     /* unconfirmed blocks (pending.dat) */
    Ledger ledger;           /* token balances under the chosen model (ledger.dat) */
    int difficulty;          /* PoW difficulty for blocks mined in this session */
} App;

/* Loads (or creates) chain, ledger and pending pool, replaying the ledger
 * into `model`. Returns 1 on success. */
int app_open(App *app, LedgerModel model);
void app_close(App *app);

/* Prints an error and returns 0 unless the session is an ADMIN. */
int app_require_admin(const App *app);

/* Ledger account of the logged-in user when they mine: MINER_<username>. */
void app_miner_account(const App *app, char *out, size_t size);

/* Full integrity check: every block's hash, link, signature and proof of
 * work, and every block's link to its transaction in the ledger. Prints each
 * problem found (and a summary when `verbose`). Returns the problem count. */
int app_validate(App *app, int verbose);

/* Refuses (returns 0) when the chain is invalid, so a tampered chain is
 * never extended or saved. */
int app_ensure_intact(App *app);

/* Signs a new lending block and appends it to the pending pool, carrying
 * `tx` if given. Returns 1 on success. */
int app_submit_block(App *app, const char *book_id, const char *book_title,
                     const char *member_id, const char *member_name, const char *action,
                     int token_reward, const Transaction *tx);

/* Creates and confirms a coinbase transaction paying `amount` of newly
 * mined coins to `account`. Returns 1 on success. */
int app_pay_coinbase(App *app, const char *account, Amount amount, const char *memo);

int app_save_pending(App *app);
int app_save_all(App *app);

#endif
