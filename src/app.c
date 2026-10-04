#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

/* ======================= Startup ======================= */

/* Block 0 anchors the library treasury: the coinbase transaction that
 * creates the reward fund is referenced by the genesis block's tx_id. */
static int create_genesis(App *app) {
    printf("No existing chain found. Creating and mining the genesis block...\n");

    Transaction mint;
    if (!tx_init(&mint, TX_COINBASE, COINBASE_SENDER, LIBRARY_ACCOUNT, LIBRARY_INITIAL_SUPPLY,
                 0, 0, "initial library reward fund")) {
        return 0;
    }

    Block genesis;
    if (!block_init_signed(&genesis, "N/A", "GENESIS BLOCK", "N/A", "SYSTEM", ACTION_GENESIS,
                           0, mint.txid, app->session.username, app->session.signing_key)) {
        printf("ERROR: Could not sign the genesis block\n");
        return 0;
    }

    unsigned long long attempts = pow_mine(&genesis, 0, ZERO_HASH, app->difficulty,
                                           app->session.username);
    if (attempts == 0 || !chain_append(&app->chain, &genesis)) return 0;

    char err[256];
    if (!ledger_confirm(&app->ledger, &mint, 0, err, sizeof(err))) {
        printf("ERROR: Could not fund the library treasury: %s\n", err);
        return 0;
    }
    if (!app_save_all(app)) return 0;

    printf("Genesis block mined at difficulty %d after %llu attempt(s): hash %.16s...\n",
           app->difficulty, attempts, genesis.hash);
    printf("Library treasury funded with %s coins (coinbase tx %.16s..., anchored in block 0).\n",
           fmt_amount(LIBRARY_INITIAL_SUPPLY), mint.txid);
    return 1;
}

/* After a restart the pending transactions are admitted again under the
 * session's model (like a node rebuilding its mempool), so their funds are
 * reserved. Any that are no longer valid are dropped. */
static void readmit_pending(App *app) {
    int dropped = 0;
    for (int i = 0; i < app->pending.count;) {
        PendingEntry *e = &app->pending.items[i];
        char err[256];
        if (e->has_tx && !ledger_admit(&app->ledger, &e->tx, err, sizeof(err))) {
            printf("WARNING: Pending %s block for '%s' dropped: %s\n",
                   e->block.action, e->block.book_title, err);
            pending_remove(&app->pending, i);
            dropped++;
            continue;
        }
        i++;
    }
    if (dropped) app_save_pending(app);
    if (app->pending.count > 0) {
        printf("%d unconfirmed block(s) waiting in the pending pool.\n", app->pending.count);
    }
}

int app_open(App *app, LedgerModel model) {
    if (!chain_init(&app->chain) || !pending_init(&app->pending) ||
        !ledger_init(&app->ledger, model)) {
        printf("ERROR: Out of memory\n");
        return 0;
    }

    /* Every registered member starts with a balance of 0. */
    for (int i = 0; i < app->reg.member_count; i++) {
        ledger_add_account(&app->ledger, app->reg.members[i].member_id);
    }
    ledger_add_account(&app->ledger, LIBRARY_ACCOUNT);

    StorageResult rc = chain_load(&app->chain, CHAIN_FILE);
    if (rc == STORAGE_ERROR) return 0;
    if (rc == STORAGE_MISSING) {
        if (storage_exists(LEDGER_FILE) || storage_exists(PENDING_FILE)) {
            printf("ERROR: '%s' is missing but '%s' or '%s' still exist. These files belong "
                   "together; run 'make clean' to start a fresh chain.\n",
                   CHAIN_FILE, LEDGER_FILE, PENDING_FILE);
            return 0;
        }
        return create_genesis(app);
    }
    printf("Loaded existing chain with %d block(s).\n", app->chain.count);

    rc = ledger_load(&app->ledger, LEDGER_FILE);
    if (rc == STORAGE_MISSING) {
        printf("ERROR: '%s' exists but the token ledger '%s' is missing. Restore it, or run "
               "'make clean' to start a fresh chain.\n", CHAIN_FILE, LEDGER_FILE);
        return 0;
    }
    if (rc == STORAGE_ERROR) return 0;
    printf("Replayed %d confirmed transaction(s) into the %s model.\n",
           app->ledger.log_count, ledger_model_name(model));

    if (pending_load(&app->pending, PENDING_FILE) == STORAGE_ERROR) return 0;
    readmit_pending(app);
    return 1;
}

void app_close(App *app) {
    chain_free(&app->chain);
    pending_free(&app->pending);
    ledger_free(&app->ledger);
}

/* ======================= Helpers ======================= */

int app_require_admin(const App *app) {
    if (app->session.role == ROLE_ADMIN) return 1;
    printf("ERROR: Access denied. This action requires the ADMIN role.\n");
    return 0;
}

void app_miner_account(const App *app, char *out, size_t size) {
    snprintf(out, size, "%s%s", MINER_PREFIX, app->session.username);
}

int app_save_pending(App *app) {
    if (pending_save(&app->pending, PENDING_FILE)) return 1;
    printf("WARNING: The pending pool is in memory but could not be saved to disk\n");
    return 0;
}

int app_save_all(App *app) {
    /* Ledger first: a chain block that points at a transaction missing from
     * the ledger would be reported by validation. */
    int ok = ledger_save(&app->ledger, LEDGER_FILE);
    ok = chain_save(&app->chain, CHAIN_FILE) && ok;
    ok = pending_save(&app->pending, PENDING_FILE) && ok;
    if (!ok) printf("WARNING: Some data could not be saved to disk\n");
    return ok;
}

int app_submit_block(App *app, const char *book_id, const char *book_title,
                     const char *member_id, const char *member_name, const char *action,
                     int token_reward, const Transaction *tx) {
    PendingEntry entry;
    memset(&entry, 0, sizeof(entry));
    if (!block_init_signed(&entry.block, book_id, book_title, member_id, member_name, action,
                           token_reward, tx ? tx->txid : ZERO_HASH,
                           app->session.username, app->session.signing_key)) {
        printf("ERROR: Failed to create and sign the block\n");
        return 0;
    }
    if (tx) {
        entry.has_tx = 1;
        entry.tx = *tx;
    }
    if (!pending_push(&app->pending, &entry)) return 0;
    app_save_pending(app);
    return 1;
}

int app_pay_coinbase(App *app, const char *account, Amount amount, const char *memo) {
    if (amount <= 0) return 1;

    /* The log position makes every coinbase ID unique, even when two pay the
     * same account the same amount in the same second. */
    Transaction tx;
    if (!tx_init(&tx, TX_COINBASE, COINBASE_SENDER, account, amount, 0,
                 (unsigned int)app->ledger.log_count, memo)) {
        printf("ERROR: Invalid coinbase account '%s'\n", account);
        return 0;
    }

    char err[256];
    if (!ledger_confirm(&app->ledger, &tx, 0, err, sizeof(err))) {
        printf("ERROR: Coinbase payout failed: %s\n", err);
        return 0;
    }
    printf("  Coinbase tx %.8s: +%s coins to %s (%s)\n", tx.txid, fmt_amount(amount), account, memo);
    return 1;
}

/* ======================= Validation ======================= */

/* Checks that a block and the transaction it references agree. Returns the
 * number of problems printed. */
static int check_block_transaction(const App *app, const Block *b) {
    int has_tx = strcmp(b->tx_id, ZERO_HASH) != 0;
    int is_return = strcmp(b->action, ACTION_RETURNED) == 0;
    int is_transfer = strcmp(b->action, ACTION_TRANSFER) == 0;
    int is_genesis = strcmp(b->action, ACTION_GENESIS) == 0;

    if (!has_tx) {
        if (is_return || is_transfer || is_genesis || b->token_reward != 0) {
            printf("  Block #%d: %s block with token reward %d has no transaction\n",
                   b->index, b->action, b->token_reward);
            return 1;
        }
        return 0;
    }

    const Transaction *tx = ledger_find_tx(&app->ledger, b->tx_id);
    if (!tx) {
        printf("  Block #%d: references transaction %.8s, which is not in the ledger\n",
               b->index, b->tx_id);
        return 1;
    }
    if (!tx_id_valid(tx)) {
        printf("  Block #%d: transaction %.8s does not match its SHA-256 ID\n", b->index, b->tx_id);
        return 1;
    }

    const TxBody *t = &tx->body;
    const char *problem = NULL;
    if (is_return) {
        if (t->type != TX_REWARD || strcmp(t->recipient, b->member_id) != 0) {
            problem = "reward transaction is not a reward to this member";
        } else if (t->amount + t->fee != (Amount)b->token_reward * COIN) {
            printf("  Block #%d: token reward %d does not match its reward transaction (%s coins)\n",
                   b->index, b->token_reward, fmt_amount(t->amount + t->fee));
            return 1;
        }
    } else if (is_transfer) {
        if (t->type != TX_TRANSFER || strcmp(t->sender, b->member_id) != 0) {
            problem = "transfer transaction does not belong to this block's member";
        }
    } else if (is_genesis) {
        if (t->type != TX_COINBASE || strcmp(t->recipient, LIBRARY_ACCOUNT) != 0) {
            problem = "genesis transaction is not the library treasury fund";
        }
    } else {
        problem = "only RETURNED, TRANSFER and GENESIS blocks may carry a transaction";
    }

    if (problem) {
        printf("  Block #%d: %s\n", b->index, problem);
        return 1;
    }
    return 0;
}

int app_validate(App *app, int verbose) {
    int problems = 0;

    for (int i = 0; i < app->chain.count; i++) {
        ChainValidationResult r = chain_check_block(&app->chain, i);
        if (r != CHAIN_VALID) {
            printf("  Block #%d: %s\n", i, chain_result_message(r));
            problems++;
        }
        problems += check_block_transaction(app, &app->chain.blocks[i]);
    }

    for (int i = 0; i < app->ledger.log_count; i++) {
        if (!tx_id_valid(&app->ledger.log[i])) {
            printf("  Ledger tx #%d (%.8s): contents do not match its SHA-256 ID\n",
                   i, app->ledger.log[i].txid);
            problems++;
        }
    }

    if (problems > 0) {
        printf("Chain is INVALID: %d problem(s) found.\n", problems);
    } else if (verbose) {
        printf("Chain is VALID. All %d blocks verified: hashes recomputed, links intact, "
               "librarian signatures valid,\nproof-of-work met; all %d ledger transactions "
               "match their SHA-256 IDs and the blocks that reference them.\n",
               app->chain.count, app->ledger.log_count);
    }
    return problems;
}

int app_ensure_intact(App *app) {
    if (app_validate(app, 0) == 0) return 1;
    printf("ERROR: Refusing to add or mine blocks on an invalid chain. Restart the program to "
           "reload the saved chain, or restore '%s' from a trusted copy.\n", CHAIN_FILE);
    return 0;
}
