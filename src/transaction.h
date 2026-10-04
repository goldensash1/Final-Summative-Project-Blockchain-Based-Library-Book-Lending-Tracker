#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <stddef.h>
#include <time.h>

/* Token amounts are stored as integer hundredths of a coin (like Bitcoin's
 * satoshis), so fees and pool shares can be split exactly, without
 * floating-point rounding errors. */
typedef long long Amount;
#define COIN 100LL
#define MAX_AMOUNT (1000000000LL * COIN)

#define LEDGER_ID_MAX 40    /* account identifiers: member IDs, LIBRARY, MINER_<user>, ... */
#define TX_MEMO_MAX 64
#define MAX_TX_INPUTS 32
#define MAX_TX_OUTPUTS 2    /* payment + change */

#define COINBASE_SENDER "COINBASE"

typedef enum {
    TX_COINBASE = 0,   /* newly created coins: treasury fund and mining rewards */
    TX_REWARD = 1,     /* library -> member, created automatically by a book return */
    TX_TRANSFER = 2    /* member -> member, entered manually */
} TxType;

/* The model-independent part of a transaction. The transaction ID is the
 * SHA-256 hash of exactly these fields, so both ledger models agree on it. */
typedef struct {
    TxType type;
    char sender[LEDGER_ID_MAX];
    char recipient[LEDGER_ID_MAX];
    Amount amount;              /* credited to the recipient */
    Amount fee;                 /* paid by the sender, collected by the miner */
    unsigned int nonce;         /* sender's outgoing transaction counter */
    time_t timestamp;
    char memo[TX_MEMO_MAX];
} TxBody;

/* UTXO model: a reference to an output of an earlier transaction. */
typedef struct {
    char txid[65];
    int vout;
} TxInput;

typedef struct {
    char owner[LEDGER_ID_MAX];
    Amount amount;
} TxOutput;

typedef struct {
    char txid[65];              /* SHA-256 of body */
    TxBody body;
    /* UTXO-model view, filled in by the UTXO ledger when it selects coins.
     * Empty under the account model. */
    int input_count;
    TxInput inputs[MAX_TX_INPUTS];
    int output_count;
    TxOutput outputs[MAX_TX_OUTPUTS];
} Transaction;

/* Fills a new transaction and computes its ID. Returns 0 if an identifier
 * is too long. */
int tx_init(Transaction *tx, TxType type, const char *sender, const char *recipient,
            Amount amount, Amount fee, unsigned int nonce, const char *memo);

/* Writes the SHA-256 hex ID of `body` into out (65 bytes). */
void tx_compute_id(const TxBody *body, char out[65]);

/* Returns 1 if tx->txid matches the hash of tx->body (no tampering). */
int tx_id_valid(const Transaction *tx);

/* amount + fee: what the sender pays in total. */
Amount tx_total_cost(const Transaction *tx);

/* Force-terminates every string, so a corrupted file can't cause reads
 * past the end of a field. */
void tx_sanitize(Transaction *tx);

const char *tx_type_name(TxType type);

/* Prints a column header / one row of a transaction history table. */
void tx_print_header(void);
void tx_print_row(int n, const char *txid, const TxBody *body);

/* Formats an amount as "12.50" (or "-3.00"). */
void amount_format(Amount a, char *buf, size_t size);

/* Same, returning one of 8 rotating static buffers, so several amounts can
 * appear in a single printf call. */
const char *fmt_amount(Amount a);

/* Parses "5", "2.5" or "2.50" (at most 2 decimals, no sign).
 * Returns 1 on success. */
int amount_parse(const char *s, Amount *out);

#endif
