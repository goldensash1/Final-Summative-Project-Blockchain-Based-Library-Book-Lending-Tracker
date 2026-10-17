#ifndef LEDGER_H
#define LEDGER_H

#include "transaction.h"
#include "utxo.h"
#include "account.h"
#include "registry.h"
#include "storage.h"

/* Well-known ledger accounts (members use their registry IDs). */
#define LIBRARY_ACCOUNT       "LIBRARY"        /* treasury that pays return rewards */
#define MINER_PREFIX          "MINER_"         /* MINER_<username>: solo / cloud miner */
#define POOL_MINER_PREFIX     "POOL_M"         /* POOL_M1, POOL_M2, ...: simulated pool miners */
#define POOL_OPERATOR_ACCOUNT "POOL_OPERATOR"  /* receives the pool fee */
#define CLOUD_HOST_ACCOUNT    "CLOUD_HOST"     /* receives cloud maintenance fees */

typedef enum {
    MODEL_UTXO = 0,
    MODEL_ACCOUNT = 1
} LedgerModel;

/* The token ledger. Every confirmed transaction is kept, in order, in `log`
 * (persisted to ledger.dat). Only the model chosen for the session builds
 * its state from that log at startup:
 *   MODEL_UTXO    -> `utxo` (the set of unspent outputs)
 *   MODEL_ACCOUNT -> `accounts` (balance + nonce + history list per account)
 * Because both are rebuilt from the same log, switching models between runs
 * never changes anyone's balance; only how it is represented and checked. */
typedef struct {
    LedgerModel model;
    Transaction *log;
    int log_count;
    int log_capacity;
    UtxoSet utxo;
    AccountTable accounts;
} Ledger;

const char *ledger_model_name(LedgerModel model);

int ledger_init(Ledger *ledger, LedgerModel model);
void ledger_free(Ledger *ledger);

/* Makes sure an account exists with balance 0 (account model; a no-op for
 * UTXO, where an owner with no outputs simply has balance 0). */
int ledger_add_account(Ledger *ledger, const char *id);

/* Loads ledger.dat, checks every transaction's SHA-256 ID and replays the
 * transactions into the chosen model. */
StorageResult ledger_load(Ledger *ledger, const char *path);
int ledger_save(const Ledger *ledger, const char *path);

/* Checks a new transaction before it enters the pending pool and reserves
 * the sender's funds (UTXO: locks inputs; account: reserves balance and
 * nonce). Returns 1, or 0 with the reason in err. */
int ledger_admit(Ledger *ledger, Transaction *tx, char *err, size_t err_size);

/* Undoes ledger_admit for a pending transaction that will not be mined. */
void ledger_release(Ledger *ledger, const Transaction *tx);

/* Confirms a transaction (when its block is mined, or when replaying the
 * log): validates it again under the chosen model, applies it and appends it
 * to the log. `tx` is updated with the model's view (UTXO inputs/outputs).
 * Returns 1, or 0 with the reason in err (nothing changed). */
int ledger_confirm(Ledger *ledger, Transaction *tx, int was_admitted, char *err, size_t err_size);

/* Undoes the confirmation of every transaction after the first `keep_count`
 * by rebuilding the model from the shortened log. Pending reservations are
 * cleared too, so the caller re-admits the pending transactions afterwards.
 * Used when a block's transactions cannot all be confirmed. */
int ledger_rollback(Ledger *ledger, int keep_count);

/* Returns the confirmed transaction with this ID, or NULL. */
const Transaction *ledger_find_tx(const Ledger *ledger, const char *txid);

/* Confirmed balance, and the part reserved by pending transactions. */
Amount ledger_balance(const Ledger *ledger, const char *id);
Amount ledger_reserved(const Ledger *ledger, const char *id);

/* Next valid nonce for `id`: confirmed outgoing transactions + pending. */
unsigned int ledger_next_nonce(const Ledger *ledger, const char *id);

/* Returns 1 if `id` has an account or appears in any confirmed transaction. */
int ledger_account_exists(const Ledger *ledger, const char *id);

/* Total coins in circulation (sum of all balances). */
Amount ledger_total_supply(const Ledger *ledger);

/* Writes a short description of an account ("John Doe", "Library reward
 * treasury", ...) into buf. */
void ledger_account_label(const char *id, const Registry *reg, char *buf, size_t size);

/* Balance table for every member and every other ledger account. */
void ledger_print_balances(const Ledger *ledger, const Registry *reg);

/* One line with the account's current state. */
void ledger_print_account_line(const Ledger *ledger, const char *id, const Registry *reg);

/* Full UTXO set (UTXO model only). */
void ledger_print_utxo_set(const Ledger *ledger);

/* Explains how the model handled a freshly admitted transaction. */
void ledger_describe_admitted(const Ledger *ledger, const Transaction *tx);

/* Full transaction history for an account. Account model: walks the
 * account's linked list. UTXO model: filters the confirmed log and shows
 * the account's unspent outputs. Returns 0 if the account is unknown. */
int ledger_print_history(const Ledger *ledger, const char *id);

#endif
