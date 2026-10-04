#ifndef ACCOUNT_H
#define ACCOUNT_H

#include "transaction.h"

/* One entry of an account's transaction history (singly linked list). */
typedef struct TxHistoryNode {
    char txid[65];
    TxBody body;                    /* sender, recipient, amount, fee, nonce, ... */
    struct TxHistoryNode *next;
} TxHistoryNode;

typedef struct {
    char id[LEDGER_ID_MAX];
    Amount balance;
    unsigned int nonce;             /* confirmed outgoing transactions = next valid nonce */
    Amount reserved;                /* debits of this account's pending transactions */
    unsigned int pending_count;     /* outgoing transactions waiting in the pending pool */
    TxHistoryNode *history_head;    /* oldest first */
    TxHistoryNode *history_tail;    /* for O(1) append */
    int history_len;
} Account;

typedef struct {
    Account *items;
    int count;
    int capacity;
} AccountTable;

int account_table_init(AccountTable *table);

/* Frees every account and its history list. */
void account_table_free(AccountTable *table);

/* Returns the account's index, or -1 if it does not exist. */
int account_index(const AccountTable *table, const char *id);

/* Returns the index of the account, creating it with balance 0 if needed.
 * Returns -1 if out of memory or the id is too long. */
int account_ensure(AccountTable *table, const char *id);

/* Admits a new (pending) transaction: the nonce must be exactly the next
 * one for the sender (confirmed + pending), and the sender's available
 * balance (balance - reserved) must cover amount + fee. Reserves the cost.
 * Returns 1, or 0 with a reason in err. */
int account_admit(AccountTable *table, const Transaction *tx, char *err, size_t err_size);

/* Releases the reservation of a pending transaction that was dropped. */
void account_release(AccountTable *table, const Transaction *tx);

/* Confirms a transaction: checks the nonce and balance again, debits the
 * sender (amount + fee), credits the recipient, increments the sender's nonce
 * and appends the transaction to both accounts' history lists.
 * `was_admitted` says whether a pending reservation must be released.
 * Returns 1, or 0 with a reason in err (nothing is changed). */
int account_apply(AccountTable *table, const Transaction *tx, int was_admitted,
                  char *err, size_t err_size);

/* Prints the full transaction history of `id` by walking its linked list.
 * Returns 0 if the account does not exist. */
int account_print_history(const AccountTable *table, const char *id);

#endif
