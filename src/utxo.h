#ifndef UTXO_H
#define UTXO_H

#include "transaction.h"

/* One unspent transaction output: coins that `owner` can spend by naming
 * (txid, vout) as an input of a new transaction. */
typedef struct {
    char txid[65];
    int vout;
    char owner[LEDGER_ID_MAX];
    Amount amount;
    int confirmed;          /* 0 = change output of a transaction still in the
                               pending pool (only its sender may chain on it) */
    char reserved_by[65];   /* txid of the pending transaction that will spend
                               this output, "" if it is free */
} Utxo;

/* The UTXO set: every output that has been created and not yet spent, in
 * creation order (oldest first). A balance is not stored anywhere; it is the
 * sum of the outputs an owner holds. */
typedef struct {
    Utxo *items;
    int count;
    int capacity;
} UtxoSet;

int utxo_set_init(UtxoSet *set);
void utxo_set_free(UtxoSet *set);

/* Admits a new (pending) transaction. If it has no inputs yet, coins are
 * selected oldest-first from the sender's free outputs; otherwise the given
 * inputs are checked. Builds the outputs (payment + change) and reserves the
 * inputs so no other pending transaction can spend them (double-spend
 * prevention). The change output is added as unconfirmed, so the sender can
 * make further transactions before this one is mined (as in Bitcoin's
 * mempool); the payment only reaches the recipient on confirmation.
 * Returns 1, or 0 with a reason in err. */
int utxo_admit(UtxoSet *set, Transaction *tx, char *err, size_t err_size);

/* Undoes utxo_admit for a dropped pending transaction: frees its inputs and
 * removes its unconfirmed change output. */
void utxo_release(UtxoSet *set, const Transaction *tx);

/* Confirms a transaction: validates its inputs against the set (they must
 * exist, be confirmed and unspent, belong to the sender, and cover
 * amount + fee), then removes them and adds the new outputs. Fills in
 * inputs/outputs if the transaction has none yet. Returns 1, or 0 with a
 * reason in err. */
int utxo_apply(UtxoSet *set, Transaction *tx, char *err, size_t err_size);

/* Confirmed balance: sum of the confirmed outputs owned by `owner`. */
Amount utxo_balance(const UtxoSet *set, const char *owner);

/* Sum of `owner`'s confirmed outputs reserved by pending transactions. */
Amount utxo_locked(const UtxoSet *set, const char *owner);

/* Number of confirmed outputs owned by `owner`. */
int utxo_count_for(const UtxoSet *set, const char *owner);

/* Number of distinct pending transactions spending `owner`'s outputs. */
int utxo_pending_tx_count(const UtxoSet *set, const char *owner);

/* Total of all confirmed outputs (coins in circulation). */
Amount utxo_total(const UtxoSet *set);

/* Prints the full UTXO set: every unspent output and its owner. */
void utxo_print_set(const UtxoSet *set);

/* Prints only the outputs owned by `owner`. */
void utxo_print_owner(const UtxoSet *set, const char *owner);

/* Prints an admitted transaction as inputs -> outputs + fee. */
void utxo_print_tx(const UtxoSet *set, const Transaction *tx);

#endif
