#include <stdio.h>
#include <string.h>

#include "tokens.h"
#include "input.h"

#define REPLAY_LIST_MAX 8
#define MIN_TXID_PREFIX 4

void tokens_view_balances(App *app) {
    ledger_print_balances(&app->ledger, &app->reg);
    if (app->ledger.model == MODEL_UTXO) {
        printf("\n");
        ledger_print_utxo_set(&app->ledger);
    }
    if (app->pending.count > 0) {
        printf("Note: %d pending block(s) are not included until they are mined.\n",
               app->pending.count);
    }
}

void tokens_transfer(App *app) {
    char sender_id[20], recipient_id[20], amount_str[32];
    if (!prompt_id("Sender Member ID: ", sender_id, sizeof(sender_id))) return;
    if (!prompt_id("Recipient Member ID: ", recipient_id, sizeof(recipient_id))) return;

    Member *sender = registry_find_member(&app->reg, sender_id);
    Member *recipient = registry_find_member(&app->reg, recipient_id);
    if (!sender || !recipient) {
        printf("ERROR: Member not found (transfers are between registered members)\n");
        return;
    }
    if (strcmp(sender->member_id, recipient->member_id) == 0) {
        printf("ERROR: Sender and recipient must be different members\n");
        return;
    }

    printf("%s (%s): balance %s, of which %s is reserved by pending transactions. Fee: %s coin.\n",
           sender->member_id, sender->full_name,
           fmt_amount(ledger_balance(&app->ledger, sender->member_id)),
           fmt_amount(ledger_reserved(&app->ledger, sender->member_id)), fmt_amount(TX_FEE));

    if (read_line("Amount to send (coins, e.g. 5 or 2.50): ", amount_str,
                  sizeof(amount_str)) != INPUT_OK) return;
    Amount amount;
    if (!amount_parse(amount_str, &amount) || amount <= 0) {
        printf("ERROR: Invalid amount (use a positive number with at most 2 decimals)\n");
        return;
    }

    unsigned int nonce = ledger_next_nonce(&app->ledger, sender->member_id);
    if (app->ledger.model == MODEL_ACCOUNT) {
        /* Account model: the sender states the nonce; the ledger checks it. */
        char nonce_str[16], prompt[96];
        snprintf(prompt, sizeof(prompt), "Transaction nonce for %s (next valid nonce: %u): ",
                 sender->member_id, nonce);
        if (read_line(prompt, nonce_str, sizeof(nonce_str)) != INPUT_OK) return;
        int entered;
        if (!parse_int(nonce_str, &entered) || entered < 0) {
            printf("ERROR: Invalid nonce\n");
            return;
        }
        nonce = (unsigned int)entered;
    }

    if (!app_ensure_intact(app)) return;

    char memo[TX_MEMO_MAX];
    snprintf(memo, sizeof(memo), "transfer to %s", recipient->member_id);
    Transaction tx;
    if (!tx_init(&tx, TX_TRANSFER, sender->member_id, recipient->member_id, amount, TX_FEE,
                 nonce, memo)) {
        printf("ERROR: Could not create the transaction\n");
        return;
    }

    char err[256];
    if (!ledger_admit(&app->ledger, &tx, err, sizeof(err))) {
        printf("REJECTED: %s\n", err);
        return;
    }

    char title[80];
    snprintf(title, sizeof(title), "Token transfer to %s (%s)", recipient->member_id,
             recipient->full_name);
    if (!app_submit_block(app, "TOKENS", title, sender->member_id, sender->full_name,
                          ACTION_TRANSFER, 0, &tx)) {
        ledger_release(&app->ledger, &tx);
        return;
    }

    printf("ACCEPTED: transfer %.16s...: %s -> %s, amount %s, fee %s\n", tx.txid,
           sender->member_id, recipient->member_id, fmt_amount(amount), fmt_amount(TX_FEE));
    ledger_describe_admitted(&app->ledger, &tx);
    printf("Transfer block added to the PENDING POOL (%d waiting). Balances change after mining.\n",
           app->pending.count);
}

void tokens_history(App *app) {
    char input[LEDGER_ID_MAX + 2];
    if (read_line("Account ID (member ID, LIBRARY, MINER_<user>, POOL_M1, ...): ", input,
                  sizeof(input)) != INPUT_OK) return;
    trim_spaces(input);

    /* Member and system IDs are upper case; MINER_<username> keeps the
     * username's case. Try the text as typed first, then upper-cased. */
    char upper[sizeof(input)];
    snprintf(upper, sizeof(upper), "%s", input);
    to_upper_trim(upper);

    const char *id = NULL;
    if (input[0] && ledger_account_exists(&app->ledger, input)) id = input;
    else if (upper[0] && (ledger_account_exists(&app->ledger, upper) ||
                          registry_find_member(&app->reg, upper))) id = upper;

    if (!id) {
        printf("ERROR: No account '%s' in the ledger\n", input);
        return;
    }

    if (!ledger_print_history(&app->ledger, id)) {
        /* A registered member with no confirmed transactions yet. */
        printf("\nTransaction history for %s: no confirmed transactions yet (balance 0.00)\n", id);
    }
}

void tokens_replay_test(App *app) {
    printf("\nReplay test: re-submit a CONFIRMED transaction exactly as it was (same inputs,\n"
           "same nonce, same ID). A secure ledger must reject it.\n");

    /* Show the most recent reward / transfer transactions. */
    int shown = 0;
    printf("\nRecent confirmed transactions:\n");
    tx_print_header();
    for (int i = app->ledger.log_count - 1; i >= 0 && shown < REPLAY_LIST_MAX; i--) {
        const Transaction *tx = &app->ledger.log[i];
        if (tx->body.type == TX_COINBASE) continue;
        tx_print_row(++shown, tx->txid, &tx->body);
    }
    if (shown == 0) {
        printf("  (none yet: return a book or transfer tokens, then mine the pending pool)\n");
        return;
    }

    char prefix[72];
    if (read_line("Enter the start of the Tx ID to replay (at least 4 characters): ", prefix,
                  sizeof(prefix)) != INPUT_OK) return;
    trim_spaces(prefix);
    size_t len = strlen(prefix);
    if (len < MIN_TXID_PREFIX) {
        printf("ERROR: Enter at least %d characters of the transaction ID\n", MIN_TXID_PREFIX);
        return;
    }

    const Transaction *match = NULL;
    for (int i = 0; i < app->ledger.log_count; i++) {
        const Transaction *tx = &app->ledger.log[i];
        if (tx->body.type == TX_COINBASE || strncmp(tx->txid, prefix, len) != 0) continue;
        if (match) {
            printf("ERROR: '%s' matches more than one transaction; type more characters\n", prefix);
            return;
        }
        match = tx;
    }
    if (!match) {
        printf("ERROR: No confirmed reward or transfer transaction starts with '%s'\n", prefix);
        return;
    }

    Transaction replay = *match;
    printf("Re-submitting tx %.16s... (%s %s -> %s, %s coins, nonce %u",
           replay.txid, tx_type_name(replay.body.type), replay.body.sender,
           replay.body.recipient, fmt_amount(replay.body.amount), replay.body.nonce);
    if (app->ledger.model == MODEL_UTXO && replay.input_count > 0) {
        printf(", spending input %.8s:%d", replay.inputs[0].txid, replay.inputs[0].vout);
    }
    printf(")\n");

    char err[256];
    if (ledger_admit(&app->ledger, &replay, err, sizeof(err))) {
        ledger_release(&app->ledger, &replay);
        printf("UNEXPECTED: the replayed transaction was accepted (it has been released again)\n");
        return;
    }

    printf("REJECTED: %s\n", err);
    if (app->ledger.model == MODEL_UTXO) {
        printf("UTXO model: the outputs this transaction spends were removed from the UTXO set\n"
               "when it was confirmed, so spending them again is a double spend.\n");
    } else {
        printf("Account model: the sender's nonce moved past %u when this transaction was\n"
               "confirmed, so the same nonce can never be accepted again.\n", replay.body.nonce);
    }
}
