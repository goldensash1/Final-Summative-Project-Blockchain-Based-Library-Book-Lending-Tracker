#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "transaction.h"
#include "crypto.h"

static void copy_field(char *dst, size_t dst_size, const char *src) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

int tx_init(Transaction *tx, TxType type, const char *sender, const char *recipient,
            Amount amount, Amount fee, unsigned int nonce, const char *memo) {
    memset(tx, 0, sizeof(*tx));
    if (strlen(sender) >= LEDGER_ID_MAX || strlen(recipient) >= LEDGER_ID_MAX) return 0;

    TxBody *b = &tx->body;
    b->type = type;
    copy_field(b->sender, sizeof(b->sender), sender);
    copy_field(b->recipient, sizeof(b->recipient), recipient);
    b->amount = amount;
    b->fee = fee;
    b->nonce = nonce;
    b->timestamp = time(NULL);
    copy_field(b->memo, sizeof(b->memo), memo ? memo : "");

    tx_compute_id(b, tx->txid);
    return tx->txid[0] != '\0';
}

void tx_compute_id(const TxBody *body, char out[65]) {
    char buf[384];
    int n = snprintf(buf, sizeof(buf), "%d|%s|%s|%lld|%lld|%u|%ld|%s",
                     (int)body->type, body->sender, body->recipient,
                     body->amount, body->fee, body->nonce,
                     (long)body->timestamp, body->memo);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        out[0] = '\0';   /* never matches a real ID */
        return;
    }
    sha256_hex((const unsigned char *)buf, (size_t)n, out);
}

int tx_id_valid(const Transaction *tx) {
    char recomputed[65];
    tx_compute_id(&tx->body, recomputed);
    return recomputed[0] != '\0' && strcmp(recomputed, tx->txid) == 0;
}

Amount tx_total_cost(const Transaction *tx) {
    return tx->body.amount + tx->body.fee;
}

void tx_sanitize(Transaction *tx) {
    tx->txid[sizeof(tx->txid) - 1] = '\0';
    tx->body.sender[sizeof(tx->body.sender) - 1] = '\0';
    tx->body.recipient[sizeof(tx->body.recipient) - 1] = '\0';
    tx->body.memo[sizeof(tx->body.memo) - 1] = '\0';
    if (tx->input_count < 0 || tx->input_count > MAX_TX_INPUTS) tx->input_count = 0;
    if (tx->output_count < 0 || tx->output_count > MAX_TX_OUTPUTS) tx->output_count = 0;
    for (int i = 0; i < tx->input_count; i++) {
        tx->inputs[i].txid[sizeof(tx->inputs[i].txid) - 1] = '\0';
    }
    for (int i = 0; i < tx->output_count; i++) {
        tx->outputs[i].owner[sizeof(tx->outputs[i].owner) - 1] = '\0';
    }
}

const char *tx_type_name(TxType type) {
    switch (type) {
        case TX_COINBASE: return "COINBASE";
        case TX_REWARD:   return "REWARD";
        case TX_TRANSFER: return "TRANSFER";
    }
    return "UNKNOWN";
}

void tx_print_header(void) {
    printf("  %-3s %-10s %-9s %-14s %-14s %10s %6s %5s  %-19s %s\n",
           "#", "Tx ID", "Type", "Sender", "Recipient", "Amount", "Fee", "Nonce",
           "Time", "Memo");
    printf("  ");
    for (int i = 0; i < 124; i++) putchar('-');
    putchar('\n');
}

void tx_print_row(int n, const char *txid, const TxBody *body) {
    char ts[32];
    struct tm *tm_info = localtime(&body->timestamp);
    if (!tm_info || strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info) == 0) {
        snprintf(ts, sizeof(ts), "%ld", (long)body->timestamp);
    }

    char nonce[16];
    if (body->type == TX_COINBASE) snprintf(nonce, sizeof(nonce), "-");
    else snprintf(nonce, sizeof(nonce), "%u", body->nonce);

    printf("  %-3d %-10.8s %-9s %-14.14s %-14.14s %10s %6s %5s  %-19s %s\n",
           n, txid, tx_type_name(body->type), body->sender, body->recipient,
           fmt_amount(body->amount), fmt_amount(body->fee), nonce, ts, body->memo);
}

void amount_format(Amount a, char *buf, size_t size) {
    const char *sign = a < 0 ? "-" : "";
    unsigned long long abs = a < 0 ? (unsigned long long)(-(a + 1)) + 1ULL : (unsigned long long)a;
    snprintf(buf, size, "%s%llu.%02llu", sign, abs / (unsigned long long)COIN,
             abs % (unsigned long long)COIN);
}

const char *fmt_amount(Amount a) {
    static char bufs[8][32];
    static int next = 0;
    char *buf = bufs[next];
    next = (next + 1) % 8;
    amount_format(a, buf, sizeof(bufs[0]));
    return buf;
}

int amount_parse(const char *s, Amount *out) {
    while (isspace((unsigned char)*s)) s++;
    if (!isdigit((unsigned char)*s)) return 0;

    long long whole = 0;
    while (isdigit((unsigned char)*s)) {
        whole = whole * 10 + (*s - '0');
        if (whole > MAX_AMOUNT / COIN) return 0;
        s++;
    }

    long long frac = 0;
    if (*s == '.') {
        s++;
        int digits = 0;
        while (isdigit((unsigned char)*s) && digits < 2) {
            frac = frac * 10 + (*s - '0');
            digits++;
            s++;
        }
        if (digits == 0 || isdigit((unsigned char)*s)) return 0; /* "5." or 3+ decimals */
        if (digits == 1) frac *= 10;
    }

    while (isspace((unsigned char)*s)) s++;
    if (*s != '\0') return 0;

    *out = whole * COIN + frac;
    return 1;
}
