#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ledger.h"

#define LOG_INITIAL_CAPACITY 16
#define MAX_LEDGER_TXS 1000000

static const char LEDGER_MAGIC[4] = {'L', 'B', 'L', '2'};

const char *ledger_model_name(LedgerModel model) {
    return model == MODEL_UTXO ? "UTXO" : "Account-based";
}

int ledger_init(Ledger *ledger, LedgerModel model) {
    memset(ledger, 0, sizeof(*ledger));
    ledger->model = model;
    ledger->log = malloc(sizeof(Transaction) * LOG_INITIAL_CAPACITY);
    ledger->log_capacity = ledger->log ? LOG_INITIAL_CAPACITY : 0;
    return ledger->log && utxo_set_init(&ledger->utxo) && account_table_init(&ledger->accounts);
}

void ledger_free(Ledger *ledger) {
    free(ledger->log);
    ledger->log = NULL;
    ledger->log_count = ledger->log_capacity = 0;
    utxo_set_free(&ledger->utxo);
    account_table_free(&ledger->accounts);
}

int ledger_add_account(Ledger *ledger, const char *id) {
    if (ledger->model != MODEL_ACCOUNT) return 1;
    return account_ensure(&ledger->accounts, id) >= 0;
}

static int log_reserve(Ledger *ledger, int needed) {
    if (needed <= ledger->log_capacity) return 1;
    int new_capacity = ledger->log_capacity > 0 ? ledger->log_capacity : LOG_INITIAL_CAPACITY;
    while (new_capacity < needed) new_capacity *= 2;

    Transaction *grown = realloc(ledger->log, sizeof(Transaction) * (size_t)new_capacity);
    if (!grown) return 0;
    ledger->log = grown;
    ledger->log_capacity = new_capacity;
    return 1;
}

const Transaction *ledger_find_tx(const Ledger *ledger, const char *txid) {
    for (int i = 0; i < ledger->log_count; i++) {
        if (strcmp(ledger->log[i].txid, txid) == 0) return &ledger->log[i];
    }
    return NULL;
}

int ledger_admit(Ledger *ledger, Transaction *tx, char *err, size_t err_size) {
    if (tx->body.type == TX_COINBASE) {
        snprintf(err, err_size, "coinbase transactions can only be created by miners");
        return 0;
    }
    if (!tx_id_valid(tx)) {
        snprintf(err, err_size, "transaction ID does not match its contents (tampered)");
        return 0;
    }

    /* The model's own rules come first, so a replayed transaction is caught
     * by the mechanism being demonstrated: a spent input (UTXO) or a reused
     * nonce (account). */
    int ok = ledger->model == MODEL_UTXO
                 ? utxo_admit(&ledger->utxo, tx, err, err_size)
                 : account_admit(&ledger->accounts, tx, err, err_size);
    if (!ok) return 0;

    if (ledger_find_tx(ledger, tx->txid)) {
        ledger_release(ledger, tx);
        snprintf(err, err_size, "duplicate: transaction %.8s is already confirmed", tx->txid);
        return 0;
    }
    return 1;
}

void ledger_release(Ledger *ledger, const Transaction *tx) {
    if (ledger->model == MODEL_UTXO) utxo_release(&ledger->utxo, tx);
    else account_release(&ledger->accounts, tx);
}

int ledger_confirm(Ledger *ledger, Transaction *tx, int was_admitted, char *err, size_t err_size) {
    if (!tx_id_valid(tx)) {
        snprintf(err, err_size, "transaction ID does not match its contents (tampered)");
        return 0;
    }
    if (ledger_find_tx(ledger, tx->txid)) {
        snprintf(err, err_size, "duplicate: transaction %.8s is already confirmed", tx->txid);
        return 0;
    }
    if (!log_reserve(ledger, ledger->log_count + 1)) {
        snprintf(err, err_size, "out of memory");
        return 0;
    }

    Transaction applied = *tx;
    int ok;
    if (ledger->model == MODEL_UTXO) {
        ok = utxo_apply(&ledger->utxo, &applied, err, err_size);
    } else {
        /* Inputs/outputs are UTXO-only data and not part of the txid; drop
         * them so a later UTXO session derives them from its own state. */
        applied.input_count = 0;
        applied.output_count = 0;
        ok = account_apply(&ledger->accounts, &applied, was_admitted, err, err_size);
    }
    if (!ok) return 0;

    ledger->log[ledger->log_count++] = applied;
    *tx = applied;
    return 1;
}

int ledger_rollback(Ledger *ledger, int keep_count) {
    if (keep_count < 0 || keep_count > ledger->log_count) return 0;
    Transaction *kept = malloc(sizeof(Transaction) * (size_t)(keep_count > 0 ? keep_count : 1));
    if (!kept) return 0;
    memcpy(kept, ledger->log, sizeof(Transaction) * (size_t)keep_count);

    LedgerModel model = ledger->model;
    ledger_free(ledger);
    int ok = ledger_init(ledger, model);
    for (int i = 0; ok && i < keep_count; i++) {
        char err[256];
        ok = ledger_confirm(ledger, &kept[i], 0, err, sizeof(err));
    }
    free(kept);
    return ok;
}

StorageResult ledger_load(Ledger *ledger, const char *path) {
    void *records = NULL;
    int count = 0;
    StorageResult rc = storage_load(path, LEDGER_MAGIC, sizeof(Transaction), MAX_LEDGER_TXS,
                                    &records, &count);
    if (rc != STORAGE_OK) return rc;

    Transaction *txs = records;
    for (int i = 0; i < count; i++) {
        tx_sanitize(&txs[i]);
        if (!tx_id_valid(&txs[i])) {
            printf("ERROR: %s: transaction #%d (%.8s) does not match its SHA-256 ID. The "
                   "ledger file was modified.\n", path, i, txs[i].txid);
            free(records);
            return STORAGE_ERROR;
        }

        char err[256];
        if (!ledger_confirm(ledger, &txs[i], 0, err, sizeof(err))) {
            printf("ERROR: %s: transaction #%d (%.8s) cannot be replayed under the %s model: %s\n",
                   path, i, txs[i].txid, ledger_model_name(ledger->model), err);
            free(records);
            return STORAGE_ERROR;
        }
    }
    free(records);
    return STORAGE_OK;
}

int ledger_save(const Ledger *ledger, const char *path) {
    return storage_save(path, LEDGER_MAGIC, ledger->log, sizeof(Transaction), ledger->log_count);
}

Amount ledger_balance(const Ledger *ledger, const char *id) {
    if (ledger->model == MODEL_UTXO) return utxo_balance(&ledger->utxo, id);
    int idx = account_index(&ledger->accounts, id);
    return idx >= 0 ? ledger->accounts.items[idx].balance : 0;
}

Amount ledger_reserved(const Ledger *ledger, const char *id) {
    if (ledger->model == MODEL_UTXO) return utxo_locked(&ledger->utxo, id);
    int idx = account_index(&ledger->accounts, id);
    return idx >= 0 ? ledger->accounts.items[idx].reserved : 0;
}

unsigned int ledger_next_nonce(const Ledger *ledger, const char *id) {
    if (ledger->model == MODEL_ACCOUNT) {
        int idx = account_index(&ledger->accounts, id);
        if (idx < 0) return 0;
        return ledger->accounts.items[idx].nonce + ledger->accounts.items[idx].pending_count;
    }

    /* The UTXO model does not need nonces to stop replays (spent inputs do
     * that), but every transaction still records one so the same log can be
     * replayed under the account model in a later session. */
    unsigned int n = 0;
    for (int i = 0; i < ledger->log_count; i++) {
        const TxBody *b = &ledger->log[i].body;
        if (b->type != TX_COINBASE && strcmp(b->sender, id) == 0) n++;
    }
    return n + (unsigned int)utxo_pending_tx_count(&ledger->utxo, id);
}

int ledger_account_exists(const Ledger *ledger, const char *id) {
    if (ledger->model == MODEL_ACCOUNT && account_index(&ledger->accounts, id) >= 0) return 1;
    for (int i = 0; i < ledger->log_count; i++) {
        const TxBody *b = &ledger->log[i].body;
        if (strcmp(b->recipient, id) == 0 || strcmp(b->sender, id) == 0) return 1;
    }
    return 0;
}

Amount ledger_total_supply(const Ledger *ledger) {
    Amount total = 0;
    if (ledger->model == MODEL_UTXO) {
        total = utxo_total(&ledger->utxo);
    } else {
        for (int i = 0; i < ledger->accounts.count; i++) total += ledger->accounts.items[i].balance;
    }
    return total;
}

void ledger_account_label(const char *id, const Registry *reg, char *buf, size_t size) {
    for (int i = 0; i < reg->member_count; i++) {
        if (strcmp(reg->members[i].member_id, id) == 0) {
            snprintf(buf, size, "%s", reg->members[i].full_name);
            return;
        }
    }
    if (strcmp(id, LIBRARY_ACCOUNT) == 0) snprintf(buf, size, "Library reward treasury");
    else if (strcmp(id, POOL_OPERATOR_ACCOUNT) == 0) snprintf(buf, size, "Mining pool operator");
    else if (strcmp(id, CLOUD_HOST_ACCOUNT) == 0) snprintf(buf, size, "Cloud mining provider");
    else if (strncmp(id, MINER_PREFIX, strlen(MINER_PREFIX)) == 0)
        snprintf(buf, size, "Miner (user %s)", id + strlen(MINER_PREFIX));
    else if (strncmp(id, POOL_MINER_PREFIX, strlen(POOL_MINER_PREFIX)) == 0)
        snprintf(buf, size, "Pool miner");
    else snprintf(buf, size, "-");
}

static int is_member(const Registry *reg, const char *id) {
    for (int i = 0; i < reg->member_count; i++) {
        if (strcmp(reg->members[i].member_id, id) == 0) return 1;
    }
    return 0;
}

static void print_balance_row(const Ledger *ledger, const char *id, const Registry *reg) {
    char label[64];
    ledger_account_label(id, reg, label, sizeof(label));

    if (ledger->model == MODEL_UTXO) {
        printf("  %-16.16s %-26.26s %12s %10s %6d\n", id, label,
               fmt_amount(ledger_balance(ledger, id)), fmt_amount(ledger_reserved(ledger, id)),
               utxo_count_for(&ledger->utxo, id));
    } else {
        int idx = account_index(&ledger->accounts, id);
        const Account *a = idx >= 0 ? &ledger->accounts.items[idx] : NULL;
        printf("  %-16.16s %-26.26s %12s %10s %6u %8d\n", id, label,
               fmt_amount(a ? a->balance : 0), fmt_amount(a ? a->reserved : 0),
               a ? a->nonce : 0, a ? a->history_len : 0);
    }
}

void ledger_print_balances(const Ledger *ledger, const Registry *reg) {
    if (ledger->model == MODEL_UTXO) {
        printf("\nToken balances (UTXO model: balance = sum of the account's unspent outputs)\n");
        printf("  %-16s %-26s %12s %10s %6s\n", "Account", "Name / role", "Balance", "Locked", "UTXOs");
    } else {
        printf("\nToken balances (account model: balance stored directly per account)\n");
        printf("  %-16s %-26s %12s %10s %6s %8s\n", "Account", "Name / role", "Balance",
               "Reserved", "Nonce", "History");
    }
    printf("  ");
    for (int i = 0; i < 84; i++) putchar('-');
    putchar('\n');

    /* Members first (all start at 0), then every other account. */
    for (int i = 0; i < reg->member_count; i++) {
        print_balance_row(ledger, reg->members[i].member_id, reg);
    }

    if (ledger->model == MODEL_UTXO) {
        for (int i = 0; i < ledger->utxo.count; i++) {
            const char *owner = ledger->utxo.items[i].owner;
            if (is_member(reg, owner)) continue;
            int seen = 0;
            for (int j = 0; j < i && !seen; j++) seen = strcmp(ledger->utxo.items[j].owner, owner) == 0;
            if (!seen) print_balance_row(ledger, owner, reg);
        }
    } else {
        for (int i = 0; i < ledger->accounts.count; i++) {
            if (!is_member(reg, ledger->accounts.items[i].id)) {
                print_balance_row(ledger, ledger->accounts.items[i].id, reg);
            }
        }
    }

    printf("  Total coins in circulation: %s\n", fmt_amount(ledger_total_supply(ledger)));
}

void ledger_print_account_line(const Ledger *ledger, const char *id, const Registry *reg) {
    char label[64];
    ledger_account_label(id, reg, label, sizeof(label));

    if (ledger->model == MODEL_UTXO) {
        printf("    %-14s %-22.22s balance %10s (%d UTXO%s)\n", id, label,
               fmt_amount(ledger_balance(ledger, id)), utxo_count_for(&ledger->utxo, id),
               utxo_count_for(&ledger->utxo, id) == 1 ? "" : "s");
    } else {
        int idx = account_index(&ledger->accounts, id);
        const Account *a = idx >= 0 ? &ledger->accounts.items[idx] : NULL;
        printf("    %-14s %-22.22s balance %10s, nonce %u, %d tx in history\n", id, label,
               fmt_amount(a ? a->balance : 0), a ? a->nonce : 0, a ? a->history_len : 0);
    }
}

void ledger_print_utxo_set(const Ledger *ledger) {
    if (ledger->model == MODEL_UTXO) utxo_print_set(&ledger->utxo);
}

void ledger_describe_admitted(const Ledger *ledger, const Transaction *tx) {
    if (ledger->model == MODEL_UTXO) {
        utxo_print_tx(&ledger->utxo, tx);
        return;
    }

    int idx = account_index(&ledger->accounts, tx->body.sender);
    const Account *s = idx >= 0 ? &ledger->accounts.items[idx] : NULL;
    printf("  Account view of tx %.8s:\n", tx->txid);
    printf("    nonce %u accepted for %s (next valid nonce is now %u)\n", tx->body.nonce,
           tx->body.sender, s ? s->nonce + s->pending_count : 0);
    printf("    on confirmation: debit %s %s (amount %s + fee %s), credit %s %s\n",
           tx->body.sender, fmt_amount(tx_total_cost(tx)), fmt_amount(tx->body.amount),
           fmt_amount(tx->body.fee), tx->body.recipient, fmt_amount(tx->body.amount));
    if (s) {
        printf("    %s balance %s, of which %s is now reserved by pending transactions\n",
               tx->body.sender, fmt_amount(s->balance), fmt_amount(s->reserved));
    }
}

int ledger_print_history(const Ledger *ledger, const char *id) {
    if (ledger->model == MODEL_ACCOUNT) return account_print_history(&ledger->accounts, id);

    if (!ledger_account_exists(ledger, id)) return 0;
    printf("\nTransaction history for %s (UTXO model: taken from the confirmed transaction log;\n"
           "the per-account linked list belongs to the account model)\n", id);
    tx_print_header();
    int n = 0;
    for (int i = 0; i < ledger->log_count; i++) {
        const Transaction *tx = &ledger->log[i];
        if (strcmp(tx->body.sender, id) == 0 || strcmp(tx->body.recipient, id) == 0) {
            tx_print_row(++n, tx->txid, &tx->body);
        }
    }
    if (n == 0) printf("  (no confirmed transactions yet)\n");
    utxo_print_owner(&ledger->utxo, id);
    return 1;
}
