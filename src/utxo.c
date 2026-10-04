#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utxo.h"

#define UTXO_INITIAL_CAPACITY 32

static void copy_field(char *dst, size_t dst_size, const char *src) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

int utxo_set_init(UtxoSet *set) {
    set->items = malloc(sizeof(Utxo) * UTXO_INITIAL_CAPACITY);
    set->count = 0;
    set->capacity = set->items ? UTXO_INITIAL_CAPACITY : 0;
    return set->items != NULL;
}

void utxo_set_free(UtxoSet *set) {
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static int utxo_reserve(UtxoSet *set, int needed) {
    int new_capacity = set->capacity > 0 ? set->capacity : UTXO_INITIAL_CAPACITY;
    while (new_capacity < needed) new_capacity *= 2;
    if (new_capacity == set->capacity) return 1;

    Utxo *grown = realloc(set->items, sizeof(Utxo) * (size_t)new_capacity);
    if (!grown) return 0;
    set->items = grown;
    set->capacity = new_capacity;
    return 1;
}

static int utxo_find(const UtxoSet *set, const char *txid, int vout) {
    for (int i = 0; i < set->count; i++) {
        if (set->items[i].vout == vout && strcmp(set->items[i].txid, txid) == 0) return i;
    }
    return -1;
}

/* Capacity must already be reserved. */
static void utxo_add(UtxoSet *set, const char *txid, int vout, const char *owner, Amount amount,
                     int confirmed) {
    Utxo *u = &set->items[set->count++];
    memset(u, 0, sizeof(*u));
    copy_field(u->txid, sizeof(u->txid), txid);
    u->vout = vout;
    copy_field(u->owner, sizeof(u->owner), owner);
    u->amount = amount;
    u->confirmed = confirmed;
}

/* Keeps creation order, so coin selection stays deterministic. */
static void utxo_remove_at(UtxoSet *set, int i) {
    memmove(&set->items[i], &set->items[i + 1], sizeof(Utxo) * (size_t)(set->count - i - 1));
    set->count--;
}

static int is_reserved_by_other(const Utxo *u, const Transaction *tx) {
    return u->reserved_by[0] != '\0' && strcmp(u->reserved_by, tx->txid) != 0;
}

/* Coin selection: takes the sender's oldest free outputs until they cover
 * `needed`. Unconfirmed change is only used when `allow_unconfirmed`.
 * Returns 1 if enough was found, 0 if not, -1 if it would need more than
 * MAX_TX_INPUTS inputs. */
static int select_inputs(const UtxoSet *set, Transaction *tx, Amount needed, Amount *selected,
                         int allow_unconfirmed) {
    Amount total = 0;
    tx->input_count = 0;
    for (int i = 0; i < set->count && total < needed; i++) {
        const Utxo *u = &set->items[i];
        if (strcmp(u->owner, tx->body.sender) != 0 || is_reserved_by_other(u, tx)) continue;
        if (!u->confirmed && !allow_unconfirmed) continue;
        if (tx->input_count == MAX_TX_INPUTS) return -1;

        TxInput *in = &tx->inputs[tx->input_count++];
        copy_field(in->txid, sizeof(in->txid), u->txid);
        in->vout = u->vout;
        total += u->amount;
    }
    *selected = total;
    return total >= needed;
}

/* Validates explicitly given inputs: each must exist in the set (i.e. not
 * already spent), belong to the sender, not be claimed by another pending
 * transaction, and appear only once. */
static int check_inputs(const UtxoSet *set, const Transaction *tx, Amount *input_sum,
                        int allow_unconfirmed, char *err, size_t err_size) {
    Amount sum = 0;
    for (int i = 0; i < tx->input_count; i++) {
        const TxInput *in = &tx->inputs[i];
        for (int j = 0; j < i; j++) {
            if (tx->inputs[j].vout == in->vout && strcmp(tx->inputs[j].txid, in->txid) == 0) {
                snprintf(err, err_size, "double spend rejected: input %.8s:%d is listed twice",
                         in->txid, in->vout);
                return 0;
            }
        }

        int idx = utxo_find(set, in->txid, in->vout);
        if (idx < 0) {
            snprintf(err, err_size, "double spend rejected: input %.8s:%d is already spent "
                     "(it is no longer in the UTXO set)", in->txid, in->vout);
            return 0;
        }
        const Utxo *u = &set->items[idx];
        if (strcmp(u->owner, tx->body.sender) != 0) {
            snprintf(err, err_size, "input %.8s:%d belongs to %s, not to the sender %s",
                     in->txid, in->vout, u->owner, tx->body.sender);
            return 0;
        }
        if (is_reserved_by_other(u, tx)) {
            snprintf(err, err_size, "double spend rejected: input %.8s:%d is already being "
                     "spent by pending transaction %.8s", in->txid, in->vout, u->reserved_by);
            return 0;
        }
        if (!u->confirmed && !allow_unconfirmed) {
            snprintf(err, err_size, "input %.8s:%d comes from a transaction that is not "
                     "confirmed yet", in->txid, in->vout);
            return 0;
        }
        sum += u->amount;
    }
    *input_sum = sum;
    return 1;
}

/* Output 0 pays the recipient. If the inputs are worth more than
 * amount + fee, output 1 returns the excess to the sender as change.
 * Whatever is left over (inputs - outputs) is the fee for the miner. */
static void build_outputs(Transaction *tx, Amount input_sum) {
    Amount change = input_sum - tx->body.amount - tx->body.fee;
    tx->output_count = 0;

    TxOutput *pay = &tx->outputs[tx->output_count++];
    copy_field(pay->owner, sizeof(pay->owner), tx->body.recipient);
    pay->amount = tx->body.amount;

    if (change > 0) {
        TxOutput *chg = &tx->outputs[tx->output_count++];
        copy_field(chg->owner, sizeof(chg->owner), tx->body.sender);
        chg->amount = change;
    }
}

static void insufficient_funds(const UtxoSet *set, const Transaction *tx,
                               char *err, size_t err_size) {
    Amount spendable = 0;
    for (int i = 0; i < set->count; i++) {
        const Utxo *u = &set->items[i];
        if (strcmp(u->owner, tx->body.sender) == 0 && u->reserved_by[0] == '\0') {
            spendable += u->amount;
        }
    }
    Amount locked = utxo_locked(set, tx->body.sender);
    snprintf(err, err_size, "insufficient funds: %s has %s spendable (%s locked by pending "
             "transactions) but needs %s (amount %s + fee %s)",
             tx->body.sender, fmt_amount(spendable), fmt_amount(locked),
             fmt_amount(tx_total_cost(tx)), fmt_amount(tx->body.amount),
             fmt_amount(tx->body.fee));
}

/* Shared by admit and apply: choose or check the inputs, then build the
 * outputs. Admission may chain on the sender's unconfirmed change;
 * confirmation needs every input to be confirmed already. */
static int prepare_spend(const UtxoSet *set, Transaction *tx, int allow_unconfirmed,
                         char *err, size_t err_size) {
    if (tx->body.amount <= 0 || tx->body.fee < 0) {
        snprintf(err, err_size, "invalid amount or fee");
        return 0;
    }

    Amount needed = tx_total_cost(tx);
    Amount input_sum = 0;
    if (tx->input_count == 0) {
        int r = select_inputs(set, tx, needed, &input_sum, allow_unconfirmed);
        if (r < 0) {
            tx->input_count = 0;
            snprintf(err, err_size, "too many small outputs needed (more than %d inputs)",
                     MAX_TX_INPUTS);
            return 0;
        }
        if (r == 0) {
            tx->input_count = 0;
            insufficient_funds(set, tx, err, err_size);
            return 0;
        }
    } else if (!check_inputs(set, tx, &input_sum, allow_unconfirmed, err, err_size)) {
        return 0;
    }

    if (input_sum < needed) {
        snprintf(err, err_size, "insufficient inputs: inputs total %s but amount + fee is %s",
                 fmt_amount(input_sum), fmt_amount(needed));
        return 0;
    }

    build_outputs(tx, input_sum);
    return 1;
}

int utxo_admit(UtxoSet *set, Transaction *tx, char *err, size_t err_size) {
    if (tx->body.type == TX_COINBASE) return 1;
    if (!utxo_reserve(set, set->count + 1)) {
        snprintf(err, err_size, "out of memory");
        return 0;
    }
    if (!prepare_spend(set, tx, 1, err, err_size)) return 0;

    for (int i = 0; i < tx->input_count; i++) {
        int idx = utxo_find(set, tx->inputs[i].txid, tx->inputs[i].vout);
        copy_field(set->items[idx].reserved_by, sizeof(set->items[idx].reserved_by), tx->txid);
    }

    /* The change comes back to the sender straight away (unconfirmed), so a
     * second reward can be paid from it while this one waits to be mined. */
    if (tx->output_count == 2) {
        utxo_add(set, tx->txid, 1, tx->outputs[1].owner, tx->outputs[1].amount, 0);
    }
    return 1;
}

void utxo_release(UtxoSet *set, const Transaction *tx) {
    for (int i = 0; i < set->count;) {
        Utxo *u = &set->items[i];
        if (!u->confirmed && strcmp(u->txid, tx->txid) == 0) {
            utxo_remove_at(set, i);   /* its unconfirmed change */
            continue;
        }
        if (strcmp(u->reserved_by, tx->txid) == 0) u->reserved_by[0] = '\0';
        i++;
    }
}

int utxo_apply(UtxoSet *set, Transaction *tx, char *err, size_t err_size) {
    if (!utxo_reserve(set, set->count + MAX_TX_OUTPUTS)) {
        snprintf(err, err_size, "out of memory");
        return 0;
    }

    if (tx->body.type == TX_COINBASE) {
        /* Newly created coins: no inputs, one output. */
        tx->input_count = 0;
        tx->output_count = 1;
        copy_field(tx->outputs[0].owner, sizeof(tx->outputs[0].owner), tx->body.recipient);
        tx->outputs[0].amount = tx->body.amount;
        utxo_add(set, tx->txid, 0, tx->body.recipient, tx->body.amount, 1);
        return 1;
    }

    /* Validate everything before changing anything. */
    if (!prepare_spend(set, tx, 0, err, err_size)) return 0;

    for (int i = 0; i < tx->input_count; i++) {
        utxo_remove_at(set, utxo_find(set, tx->inputs[i].txid, tx->inputs[i].vout));
    }
    for (int v = 0; v < tx->output_count; v++) {
        int idx = utxo_find(set, tx->txid, v);
        if (idx >= 0) set->items[idx].confirmed = 1;   /* change added at admission */
        else utxo_add(set, tx->txid, v, tx->outputs[v].owner, tx->outputs[v].amount, 1);
    }
    return 1;
}

Amount utxo_balance(const UtxoSet *set, const char *owner) {
    Amount total = 0;
    for (int i = 0; i < set->count; i++) {
        const Utxo *u = &set->items[i];
        if (u->confirmed && strcmp(u->owner, owner) == 0) total += u->amount;
    }
    return total;
}

Amount utxo_locked(const UtxoSet *set, const char *owner) {
    Amount total = 0;
    for (int i = 0; i < set->count; i++) {
        const Utxo *u = &set->items[i];
        if (u->confirmed && u->reserved_by[0] != '\0' && strcmp(u->owner, owner) == 0) {
            total += u->amount;
        }
    }
    return total;
}

int utxo_count_for(const UtxoSet *set, const char *owner) {
    int n = 0;
    for (int i = 0; i < set->count; i++) {
        if (set->items[i].confirmed && strcmp(set->items[i].owner, owner) == 0) n++;
    }
    return n;
}

Amount utxo_total(const UtxoSet *set) {
    Amount total = 0;
    for (int i = 0; i < set->count; i++) {
        if (set->items[i].confirmed) total += set->items[i].amount;
    }
    return total;
}

int utxo_pending_tx_count(const UtxoSet *set, const char *owner) {
    int n = 0;
    for (int i = 0; i < set->count; i++) {
        const Utxo *u = &set->items[i];
        if (u->reserved_by[0] == '\0' || strcmp(u->owner, owner) != 0) continue;

        int seen = 0;
        for (int j = 0; j < i && !seen; j++) {
            seen = strcmp(set->items[j].owner, owner) == 0 &&
                   strcmp(set->items[j].reserved_by, u->reserved_by) == 0;
        }
        if (!seen) n++;
    }
    return n;
}

static void print_utxo_rows(const UtxoSet *set, const char *owner_filter) {
    printf("  %-3s %-16s %-16s %12s  %s\n", "#", "Outpoint", "Owner", "Amount", "Status");
    printf("  ");
    for (int i = 0; i < 76; i++) putchar('-');
    putchar('\n');

    int n = 0;
    for (int i = 0; i < set->count; i++) {
        const Utxo *u = &set->items[i];
        if (owner_filter && strcmp(u->owner, owner_filter) != 0) continue;

        char outpoint[24], status[80];
        snprintf(outpoint, sizeof(outpoint), "%.12s:%d", u->txid, u->vout);
        int used = snprintf(status, sizeof(status), "%s",
                            u->confirmed ? "" : "unconfirmed change, ");
        if (u->reserved_by[0]) {
            snprintf(status + used, sizeof(status) - (size_t)used, "locked by pending tx %.8s",
                     u->reserved_by);
        } else {
            snprintf(status + used, sizeof(status) - (size_t)used, "spendable");
        }

        printf("  %-3d %-16s %-16.16s %12s  %s\n", ++n, outpoint, u->owner,
               fmt_amount(u->amount), status);
    }
    if (n == 0) printf("  (no unspent outputs)\n");
}

void utxo_print_set(const UtxoSet *set) {
    int confirmed = 0;
    for (int i = 0; i < set->count; i++) confirmed += set->items[i].confirmed;
    printf("  UTXO set: %d confirmed unspent output(s), %s coins in total", confirmed,
           fmt_amount(utxo_total(set)));
    if (confirmed < set->count) printf(" (+%d unconfirmed change)", set->count - confirmed);
    printf("\n");
    print_utxo_rows(set, NULL);
}

void utxo_print_owner(const UtxoSet *set, const char *owner) {
    printf("  Unspent outputs owned by %s (balance %s):\n", owner, fmt_amount(utxo_balance(set, owner)));
    print_utxo_rows(set, owner);
}

void utxo_print_tx(const UtxoSet *set, const Transaction *tx) {
    Amount in_sum = 0, out_sum = 0;

    printf("  UTXO view of tx %.8s:\n", tx->txid);
    for (int i = 0; i < tx->input_count; i++) {
        const TxInput *in = &tx->inputs[i];
        int idx = utxo_find(set, in->txid, in->vout);
        Amount amt = idx >= 0 ? set->items[idx].amount : 0;
        in_sum += amt;
        printf("    input  %.12s:%d  %-14s %12s\n", in->txid, in->vout,
               idx >= 0 ? set->items[idx].owner : "(spent)", fmt_amount(amt));
    }
    for (int v = 0; v < tx->output_count; v++) {
        const TxOutput *out = &tx->outputs[v];
        out_sum += out->amount;
        printf("    output [%d]            %-14s %12s%s\n", v, out->owner, fmt_amount(out->amount),
               v == 1 ? "  (change back to sender)" : "");
    }
    printf("    fee = inputs %s - outputs %s = %s (collected by the miner)\n",
           fmt_amount(in_sum), fmt_amount(out_sum), fmt_amount(in_sum - out_sum));
}
