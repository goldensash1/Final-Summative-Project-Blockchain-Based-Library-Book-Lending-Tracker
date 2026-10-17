#include <stdio.h>
#include <string.h>

#include "tokens.h"
#include "input.h"

#define REPLAY_LIST_MAX 8
#define MIN_TXID_PREFIX 4

/* Resolves an account ID typed by the user: members and system accounts are
 * upper case; MINER_<username> keeps the username's case. */
static const char *resolve_account(const App *app, char *input, char *upper, size_t size) {
    trim_spaces(input);
    snprintf(upper, size, "%s", input);
    to_upper_trim(upper);
    if (input[0] && ledger_account_exists(&app->ledger, input)) return input;
    if (upper[0] && (ledger_account_exists(&app->ledger, upper) ||
                     registry_find_member((Registry *)&app->reg, upper))) return upper;
    return NULL;
}

/* Shared by token_transfer and account_transfer. explicit_nonce < 0 uses the
 * sender's next valid account nonce. */
static void submit_transfer(App *app, int argc, char **argv, int ask_nonce) {
    if (!app_require_unlocked(app)) return;
    char sender_id[ID_MAX + 8], recipient_id[ID_MAX + 8], amount_str[32];
    if (!cli_id_arg(argc, argv, 1, "Sender member ID: ", sender_id, sizeof(sender_id)) ||
        !cli_id_arg(argc, argv, 2, "Recipient member ID: ", recipient_id, sizeof(recipient_id))) return;

    Member *sender = registry_find_member(&app->reg, sender_id);
    Member *recipient = registry_find_member(&app->reg, recipient_id);
    if (!valid_entity_id(sender_id) || !valid_entity_id(recipient_id) || !sender || !recipient) {
        printf("ERROR: Member not found (transfers are between registered members)\n");
        return;
    }
    if (strcmp(sender->member_id, recipient->member_id) == 0) {
        printf("ERROR: Sender and recipient must be different members\n");
        return;
    }
    if (!cli_arg(argc, argv, 3, "Amount (coins, e.g. 5 or 2.50): ", amount_str, sizeof(amount_str))) return;
    Amount amount;
    if (!amount_parse(amount_str, &amount) || amount <= 0) {
        printf("ERROR: Invalid amount '%s' (a positive number with at most 2 decimals)\n", amount_str);
        return;
    }

    unsigned int nonce = ledger_next_nonce(&app->ledger, sender->member_id);
    if (ask_nonce) {
        char nonce_str[16], prompt[96];
        snprintf(prompt, sizeof(prompt), "Account nonce for %s (next valid nonce: %u): ",
                 sender->member_id, nonce);
        int entered;
        if (!cli_arg(argc, argv, 4, prompt, nonce_str, sizeof(nonce_str)) ||
            !parse_int(nonce_str, &entered) || entered < 0) {
            printf("ERROR: Invalid nonce\n");
            return;
        }
        nonce = (unsigned int)entered;
    }

    printf("%s (%s): balance %s, %s reserved by pending transactions. Fee: %s coin.\n",
           sender->member_id, sender->full_name, fmt_amount(ledger_balance(&app->ledger, sender->member_id)),
           fmt_amount(ledger_reserved(&app->ledger, sender->member_id)), fmt_amount(TX_FEE));

    PendingRequest req;
    memset(&req, 0, sizeof(req));
    record_init(&req.rec, ACT_TRANSFER);
    snprintf(req.rec.book_id, sizeof(req.rec.book_id), "-");
    snprintf(req.rec.book_title, sizeof(req.rec.book_title), "Token transfer to %s", recipient->member_id);
    snprintf(req.rec.member_id, sizeof(req.rec.member_id), "%s", sender->member_id);
    snprintf(req.rec.member_name, sizeof(req.rec.member_name), "%s", sender->full_name);
    req.priority = PRIORITY_NORMAL;

    char memo[TX_MEMO_MAX];
    snprintf(memo, sizeof(memo), "transfer to %s", recipient->member_id);
    if (!tx_init(&req.tx, TX_TRANSFER, sender->member_id, recipient->member_id, amount, TX_FEE,
                 nonce, memo)) {
        printf("ERROR: Could not create the transaction\n");
        return;
    }
    req.has_tx = 1;
    snprintf(req.rec.tx_id, sizeof(req.rec.tx_id), "%s", req.tx.txid);

    if (app_submit(app, &req, sender->member_id, -1)) {
        printf("ACCEPTED: transfer %.16s...: %s -> %s, amount %s, fee %s, account nonce %u\n",
               req.tx.txid, sender->member_id, recipient->member_id, fmt_amount(amount),
               fmt_amount(TX_FEE), nonce);
        int i = app->pending.count - 1;
        ledger_describe_admitted(&app->ledger, &app->pending.items[i].tx);
        printf("Balances change when the block is mined.\n");
    }
}

void cmd_token_transfer(App *app, int argc, char **argv) {
    submit_transfer(app, argc, argv, 0);
}

void cmd_account_transfer(App *app, int argc, char **argv) {
    if (app->ledger.model != MODEL_ACCOUNT) {
        printf("ERROR: This session uses the UTXO model; account_transfer needs --model account "
               "(use token_transfer instead).\n");
        return;
    }
    submit_transfer(app, argc, argv, 1);
}

void cmd_token_balance(App *app, int argc, char **argv) {
    char input[LEDGER_ID_MAX + 2], upper[LEDGER_ID_MAX + 2];
    if (!cli_arg(argc, argv, 1, "Account ID (member ID, LIBRARY, MINER_<user>, ...): ", input, sizeof(input))) {
        ledger_print_balances(&app->ledger, &app->reg);
        return;
    }
    const char *id = resolve_account(app, input, upper, sizeof(upper));
    if (!id) {
        printf("ERROR: No account '%s' in the ledger\n", input);
        return;
    }
    printf("%s: balance %s coins (%s model), %s reserved by pending transactions.\n", id,
           fmt_amount(ledger_balance(&app->ledger, id)), ledger_model_name(app->ledger.model),
           fmt_amount(ledger_reserved(&app->ledger, id)));
}

void cmd_utxo_view(App *app, int argc, char **argv) {
    if (app->ledger.model != MODEL_UTXO) {
        printf("ERROR: This session uses the account model. Restart with --model utxo to see the UTXO set.\n");
        return;
    }
    const char *owner = cli_positional(argc, argv, 1);
    if (owner) {
        char upper[LEDGER_ID_MAX];
        snprintf(upper, sizeof(upper), "%s", owner);
        to_upper_trim(upper);
        utxo_print_owner(&app->ledger.utxo, upper);
        return;
    }
    ledger_print_utxo_set(&app->ledger);
}

void cmd_account_balance(App *app, int argc, char **argv) {
    if (app->ledger.model != MODEL_ACCOUNT) {
        printf("ERROR: This session uses the UTXO model. Restart with --model account for account "
               "balances (token_balance works in both).\n");
        return;
    }
    const char *only = cli_positional(argc, argv, 1);
    if (only) {
        char input[LEDGER_ID_MAX + 2], upper[LEDGER_ID_MAX + 2];
        snprintf(input, sizeof(input), "%s", only);
        const char *id = resolve_account(app, input, upper, sizeof(upper));
        if (!id) {
            printf("ERROR: No account '%s'\n", only);
            return;
        }
        ledger_print_account_line(&app->ledger, id, &app->reg);
        return;
    }
    ledger_print_balances(&app->ledger, &app->reg);
}

void cmd_account_nonce(App *app, int argc, char **argv) {
    char input[LEDGER_ID_MAX + 2], upper[LEDGER_ID_MAX + 2];
    if (!cli_arg(argc, argv, 1, "Account ID: ", input, sizeof(input))) return;
    const char *id = resolve_account(app, input, upper, sizeof(upper));
    if (!id) {
        printf("ERROR: No account '%s'\n", input);
        return;
    }
    if (app->ledger.model == MODEL_ACCOUNT) {
        int idx = account_index(&app->ledger.accounts, id);
        unsigned int confirmed = idx >= 0 ? app->ledger.accounts.items[idx].nonce : 0;
        printf("Account %s: nonce %u (confirmed outgoing transactions); next valid transaction "
               "nonce %u.\n", id, confirmed, ledger_next_nonce(&app->ledger, id));
    } else {
        printf("Account %s: next transaction nonce %u (the UTXO model stops replays with spent "
               "inputs; nonces are kept so the log replays under the account model).\n", id,
               ledger_next_nonce(&app->ledger, id));
    }
}

void cmd_transaction_history(App *app, int argc, char **argv) {
    char input[LEDGER_ID_MAX + 2], upper[LEDGER_ID_MAX + 2];
    if (!cli_arg(argc, argv, 1, "Member ID / account: ", input, sizeof(input))) return;
    const char *id = resolve_account(app, input, upper, sizeof(upper));
    if (!id) {
        printf("ERROR: No account '%s' in the ledger\n", input);
        return;
    }
    if (!ledger_print_history(&app->ledger, id)) {
        printf("\nTransaction history for %s: no confirmed transactions yet (balance 0.00)\n", id);
    }
}

void cmd_replay_tx(App *app, int argc, char **argv) {
    printf("\nReplay test: re-submit a CONFIRMED transaction exactly as it was (same inputs, same\n"
           "nonce, same ID). The ledger must reject it.\n");
    int shown = 0;
    printf("\nRecent confirmed transactions:\n");
    tx_print_header();
    for (int i = app->ledger.log_count - 1; i >= 0 && shown < REPLAY_LIST_MAX; i--) {
        const Transaction *tx = &app->ledger.log[i];
        if (tx->body.type == TX_COINBASE) continue;
        tx_print_row(++shown, tx->txid, &tx->body);
    }
    if (shown == 0) {
        printf("  (none yet: return a book or transfer tokens, then mine)\n");
        return;
    }

    char prefix[72];
    if (!cli_arg(argc, argv, 1, "Start of the Tx ID to replay (at least 4 characters): ", prefix,
                 sizeof(prefix))) return;
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
    printf("Re-submitting tx %.16s... (%s %s -> %s, %s coins, nonce %u", replay.txid,
           tx_type_name(replay.body.type), replay.body.sender, replay.body.recipient,
           fmt_amount(replay.body.amount), replay.body.nonce);
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
        printf("UTXO model: the outputs it spends were removed from the UTXO set when it was\n"
               "confirmed, so spending them again is a double spend.\n");
    } else {
        printf("Account model: the sender's nonce moved past %u when it was confirmed, so the same\n"
               "nonce can never be accepted again.\n", replay.body.nonce);
    }
}
