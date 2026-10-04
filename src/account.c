#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "account.h"

#define ACCOUNT_INITIAL_CAPACITY 16

int account_table_init(AccountTable *table) {
    table->items = malloc(sizeof(Account) * ACCOUNT_INITIAL_CAPACITY);
    table->count = 0;
    table->capacity = table->items ? ACCOUNT_INITIAL_CAPACITY : 0;
    return table->items != NULL;
}

static void free_history(Account *a) {
    TxHistoryNode *node = a->history_head;
    while (node) {
        TxHistoryNode *next = node->next;
        free(node);
        node = next;
    }
    a->history_head = a->history_tail = NULL;
    a->history_len = 0;
}

void account_table_free(AccountTable *table) {
    for (int i = 0; i < table->count; i++) free_history(&table->items[i]);
    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}

int account_index(const AccountTable *table, const char *id) {
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].id, id) == 0) return i;
    }
    return -1;
}

int account_ensure(AccountTable *table, const char *id) {
    int idx = account_index(table, id);
    if (idx >= 0) return idx;
    if (strlen(id) >= LEDGER_ID_MAX) return -1;

    if (table->count == table->capacity) {
        int new_capacity = table->capacity > 0 ? table->capacity * 2 : ACCOUNT_INITIAL_CAPACITY;
        Account *grown = realloc(table->items, sizeof(Account) * (size_t)new_capacity);
        if (!grown) return -1;
        table->items = grown;
        table->capacity = new_capacity;
    }

    Account *a = &table->items[table->count];
    memset(a, 0, sizeof(*a));
    strcpy(a->id, id);
    return table->count++;
}

/* Allocates a history node for `tx`; it is linked in by history_append. */
static TxHistoryNode *history_node_new(const Transaction *tx) {
    TxHistoryNode *node = malloc(sizeof(*node));
    if (!node) return NULL;
    memcpy(node->txid, tx->txid, sizeof(node->txid));
    node->body = tx->body;
    node->next = NULL;
    return node;
}

static void history_append(Account *a, TxHistoryNode *node) {
    if (a->history_tail) a->history_tail->next = node;
    else a->history_head = node;
    a->history_tail = node;
    a->history_len++;
}

int account_admit(AccountTable *table, const Transaction *tx, char *err, size_t err_size) {
    const TxBody *b = &tx->body;
    if (b->type == TX_COINBASE) return 1;

    if (b->amount <= 0 || b->fee < 0) {
        snprintf(err, err_size, "invalid amount or fee");
        return 0;
    }

    int idx = account_index(table, b->sender);
    Account *sender = idx >= 0 ? &table->items[idx] : NULL;

    /* The nonce orders a sender's transactions and makes each one usable
     * only once: it must be exactly confirmed nonce + transactions already
     * waiting in the pending pool. */
    unsigned int expected = sender ? sender->nonce + sender->pending_count : 0;
    if (b->nonce < expected) {
        snprintf(err, err_size, "invalid nonce: nonce %u was already used by %s (next valid "
                 "nonce is %u). Reused or replayed transaction", b->nonce, b->sender, expected);
        return 0;
    }
    if (b->nonce > expected) {
        snprintf(err, err_size, "invalid nonce: %u skips ahead of the next valid nonce %u for %s "
                 "(nonces must increase by exactly 1)", b->nonce, expected, b->sender);
        return 0;
    }

    Amount cost = tx_total_cost(tx);
    Amount balance = sender ? sender->balance : 0;
    Amount reserved = sender ? sender->reserved : 0;
    if (!sender || balance - reserved < cost) {
        snprintf(err, err_size, "insufficient balance: %s has %s available (balance %s, %s "
                 "reserved by pending transactions) but needs %s (amount %s + fee %s)",
                 b->sender, fmt_amount(balance - reserved), fmt_amount(balance),
                 fmt_amount(reserved), fmt_amount(cost), fmt_amount(b->amount),
                 fmt_amount(b->fee));
        return 0;
    }

    sender->reserved += cost;
    sender->pending_count++;
    return 1;
}

void account_release(AccountTable *table, const Transaction *tx) {
    if (tx->body.type == TX_COINBASE) return;
    int idx = account_index(table, tx->body.sender);
    if (idx < 0) return;

    Account *sender = &table->items[idx];
    if (sender->pending_count > 0) {
        sender->pending_count--;
        sender->reserved -= tx_total_cost(tx);
    }
}

int account_apply(AccountTable *table, const Transaction *tx, int was_admitted,
                  char *err, size_t err_size) {
    const TxBody *b = &tx->body;

    /* Create the recipient first: account_ensure may move the array, so
     * pointers are only taken afterwards. */
    int ri = account_ensure(table, b->recipient);
    if (ri < 0) {
        snprintf(err, err_size, "out of memory");
        return 0;
    }

    /* Allocate history nodes up front so a failure cannot leave a half-applied
     * transaction. */
    TxHistoryNode *recipient_node = history_node_new(tx);
    TxHistoryNode *sender_node = NULL;
    int needs_sender_node = b->type != TX_COINBASE && strcmp(b->sender, b->recipient) != 0;
    if (needs_sender_node) sender_node = history_node_new(tx);
    if (!recipient_node || (needs_sender_node && !sender_node)) {
        free(recipient_node);
        free(sender_node);
        snprintf(err, err_size, "out of memory");
        return 0;
    }

    Account *recipient = &table->items[ri];
    if (b->type == TX_COINBASE) {
        recipient->balance += b->amount;
        history_append(recipient, recipient_node);
        return 1;
    }

    int si = account_index(table, b->sender);
    const char *problem = NULL;
    char reason[200];
    Amount cost = tx_total_cost(tx);
    if (si < 0) {
        problem = "unknown sender account";
    } else if (b->nonce != table->items[si].nonce) {
        snprintf(reason, sizeof(reason), "invalid nonce: expected %u for %s, got %u",
                 table->items[si].nonce, b->sender, b->nonce);
        problem = reason;
    } else if (table->items[si].balance < cost) {
        snprintf(reason, sizeof(reason), "insufficient balance: %s has %s but needs %s",
                 b->sender, fmt_amount(table->items[si].balance), fmt_amount(cost));
        problem = reason;
    }
    if (problem) {
        free(recipient_node);
        free(sender_node);
        snprintf(err, err_size, "%s", problem);
        return 0;
    }

    Account *sender = &table->items[si];
    if (was_admitted && sender->pending_count > 0) {
        sender->pending_count--;
        sender->reserved -= cost;
    }

    sender->balance -= cost;       /* debit amount + fee */
    sender->nonce++;
    recipient->balance += b->amount;  /* credit amount; the fee goes to the miner */

    if (sender_node) history_append(sender, sender_node);
    history_append(recipient, recipient_node);
    return 1;
}

int account_print_history(const AccountTable *table, const char *id) {
    int idx = account_index(table, id);
    if (idx < 0) return 0;

    const Account *a = &table->items[idx];
    printf("\nTransaction history for %s (linked list, %d entr%s, oldest first)\n",
           a->id, a->history_len, a->history_len == 1 ? "y" : "ies");
    printf("Balance: %s | next nonce: %u | pending outgoing: %u\n",
           fmt_amount(a->balance), a->nonce, a->pending_count);

    tx_print_header();
    int n = 0;
    for (const TxHistoryNode *node = a->history_head; node; node = node->next) {
        tx_print_row(++n, node->txid, &node->body);
    }
    if (n == 0) printf("  (no confirmed transactions yet)\n");
    return 1;
}
