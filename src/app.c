#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "fraud.h"

static const char META_MAGIC[4] = {'L', 'B', 'M', '1'};
static const char BALANCES_MAGIC[4] = {'L', 'B', 'B', '1'};
static const char UTXO_MAGIC[4] = {'L', 'B', 'U', '1'};

/* One row of balances.dat. */
typedef struct {
    char id[LEDGER_ID_MAX];
    Amount balance;
    unsigned int account_nonce;
} BalanceRow;

time_t app_now(const App *app) {
    return time(NULL) + app->meta.clock_offset;
}

time_t app_block_time(const App *app) {
    time_t t = app_now(app);
    time_t tip = chain_tip_time(&app->chain);
    return t > tip ? t : tip + 1;
}

int days_overdue(time_t due_date, time_t now) {
    if (due_date == 0 || now <= due_date) return 0;
    return (int)((now - due_date + SECONDS_PER_DAY - 1) / SECONDS_PER_DAY);
}

void app_librarian_signer(const App *app, char *out, size_t size) {
    snprintf(out, size, "%s%s", LIBRARIAN_SIGNER_PREFIX, app->session.username);
}

void app_miner_account(const App *app, char *out, size_t size) {
    snprintf(out, size, "%s%s", MINER_PREFIX, app->session.username);
}

int app_require_admin(const App *app) {
    if (app->session.role == ROLE_ADMIN) return 1;
    printf("ERROR: Access denied. This command requires the ADMIN role.\n");
    return 0;
}

int app_require_unlocked(const App *app) {
    if (!app->locked) return 1;
    printf("ERROR: Session locked: the chain loaded from disk failed validate_chain. Only viewing,\n"
           "       validate_chain and chain_load are allowed. Restore the files and run chain_load.\n");
    return 0;
}

/* ======================= Persistence ======================= */

static void meta_defaults(ChainMeta *m) {
    memset(m, 0, sizeof(*m));
    m->difficulty = DEFAULT_DIFFICULTY;
    m->block_reward = DEFAULT_BLOCK_REWARD;
    m->batch_size = DEFAULT_BATCH_SIZE;
    m->next_request_seq = 1;
    snprintf(m->tip_hash, sizeof(m->tip_hash), "%s", ZERO_HASH);
}

static void load_meta(App *app) {
    meta_defaults(&app->meta);
    void *records = NULL;
    int count = 0;
    if (storage_load(META_FILE, META_MAGIC, sizeof(ChainMeta), 1, &records, &count) != STORAGE_OK) return;
    if (count == 1) {
        ChainMeta m;
        memcpy(&m, records, sizeof(m));
        m.tip_hash[SHA256_HEX_LEN] = '\0';
        if (m.difficulty >= MIN_DIFFICULTY && m.difficulty <= MAX_DIFFICULTY &&
            m.batch_size >= 1 && m.batch_size <= MAX_RECORDS_PER_BLOCK && m.block_reward >= 0 &&
            m.clock_offset >= 0 && m.next_request_seq >= 1) {
            app->meta = m;
        } else {
            printf("WARNING: %s has invalid values; using defaults\n", META_FILE);
        }
    }
    free(records);
}

/* Every account that appears in the registry or the confirmed log. */
static int collect_accounts(const App *app, BalanceRow **rows_out) {
    int cap = app->reg.member_count + 2 * app->ledger.log_count + 2, n = 0;
    BalanceRow *rows = calloc((size_t)cap, sizeof(BalanceRow));
    if (!rows) return -1;

    const char *ids[2];
    for (int i = -1; i < app->reg.member_count + app->ledger.log_count; i++) {
        int k = 0;
        if (i < 0) ids[k++] = LIBRARY_ACCOUNT;
        else if (i < app->reg.member_count) ids[k++] = app->reg.members[i].member_id;
        else {
            const TxBody *b = &app->ledger.log[i - app->reg.member_count].body;
            if (b->type != TX_COINBASE) ids[k++] = b->sender;
            ids[k++] = b->recipient;
        }
        for (int j = 0; j < k; j++) {
            int seen = 0;
            for (int r = 0; r < n && !seen; r++) seen = strcmp(rows[r].id, ids[j]) == 0;
            if (seen || n >= cap) continue;
            snprintf(rows[n].id, sizeof(rows[n].id), "%s", ids[j]);
            rows[n].balance = ledger_balance(&app->ledger, ids[j]);
            rows[n].account_nonce = ledger_next_nonce(&app->ledger, ids[j]);
            n++;
        }
    }
    *rows_out = rows;
    return n;
}

static int save_balances(const App *app) {
    BalanceRow *rows = NULL;
    int n = collect_accounts(app, &rows);
    if (n < 0) return 0;
    int ok = storage_save(BALANCES_FILE, BALANCES_MAGIC, rows, sizeof(BalanceRow), n);
    free(rows);
    if (ok && app->ledger.model == MODEL_UTXO) {
        /* Only confirmed outputs: pending change is rebuilt from pending.dat. */
        const UtxoSet *set = &app->ledger.utxo;
        Utxo *confirmed = calloc((size_t)(set->count > 0 ? set->count : 1), sizeof(Utxo));
        int m = 0;
        if (!confirmed) return 0;
        for (int i = 0; i < set->count; i++) {
            if (set->items[i].confirmed) {
                confirmed[m] = set->items[i];
                confirmed[m].reserved_by[0] = '\0';
                m++;
            }
        }
        ok = storage_save(UTXO_FILE, UTXO_MAGIC, confirmed, sizeof(Utxo), m);
        free(confirmed);
    }
    return ok;
}

/* Cross-checks balances.dat against the balances replayed from ledger.dat. */
static void check_balance_snapshot(const App *app) {
    void *records = NULL;
    int count = 0;
    StorageResult rc = storage_load(BALANCES_FILE, BALANCES_MAGIC, sizeof(BalanceRow), 100000,
                                    &records, &count);
    if (rc != STORAGE_OK) return;
    const BalanceRow *rows = records;
    int mismatches = 0;
    for (int i = 0; i < count; i++) {
        char id[LEDGER_ID_MAX];
        snprintf(id, sizeof(id), "%.*s", LEDGER_ID_MAX - 1, rows[i].id);
        if (ledger_balance(&app->ledger, id) != rows[i].balance) {
            if (mismatches++ < 3) {
                printf("WARNING: %s says %s has %s coins, the replayed ledger says %s\n",
                       BALANCES_FILE, id, fmt_amount(rows[i].balance),
                       fmt_amount(ledger_balance(&app->ledger, id)));
            }
        }
    }
    if (mismatches) printf("WARNING: %d balance(s) in %s were altered; using the ledger.\n",
                           mismatches, BALANCES_FILE);
    free(records);
}

int app_save_pending(App *app) {
    int ok = pending_save(&app->pending, PENDING_FILE);
    ok = storage_save(META_FILE, META_MAGIC, &app->meta, sizeof(ChainMeta), 1) && ok;
    if (!ok) printf("WARNING: The pending pool could not be saved to disk\n");
    return ok;
}

int app_save_all(App *app) {
    if (app->locked) {
        printf("ERROR: Not saving: the chain in memory failed validation.\n");
        return 0;
    }
    app->meta.chain_height = app->chain.count;
    snprintf(app->meta.tip_hash, sizeof(app->meta.tip_hash), "%s", chain_tip_hash(&app->chain));
    app->meta.saved_at = time(NULL);

    /* Ledger first: a block that points at a transaction missing from the
     * ledger would be reported by validation. */
    int ok = ledger_save(&app->ledger, LEDGER_FILE);
    ok = chain_save(&app->chain, CHAIN_FILE) && ok;
    ok = pending_save(&app->pending, PENDING_FILE) && ok;
    ok = state_save(app->state, STATE_FILE) && ok;
    ok = save_balances(app) && ok;
    ok = storage_save(META_FILE, META_MAGIC, &app->meta, sizeof(ChainMeta), 1) && ok;
    if (!ok) printf("WARNING: Some data could not be saved to disk\n");
    return ok;
}

/* ======================= Startup ======================= */

/* Block 0 anchors the library treasury: its transaction_id is the coinbase
 * transaction that creates the reward fund. It carries no records. */
static int create_genesis(App *app) {
    printf("No saved chain found. Creating and mining the genesis block...\n");

    Transaction mint;
    if (!tx_init(&mint, TX_COINBASE, COINBASE_SENDER, LIBRARY_ACCOUNT, LIBRARY_INITIAL_SUPPLY,
                 0, 0, "initial library reward fund")) {
        return 0;
    }

    Block *genesis = calloc(1, sizeof(Block));
    if (!genesis) return 0;
    genesis->timestamp = app_now(app);
    genesis->record_count = 0;
    snprintf(genesis->merkle_root, sizeof(genesis->merkle_root), "%s", ZERO_HASH);
    snprintf(genesis->transaction_id, sizeof(genesis->transaction_id), "%s", mint.txid);

    unsigned long long attempts = pow_mine(genesis, 0, ZERO_HASH, app->meta.difficulty,
                                           app->session.username);
    int ok = attempts > 0 && chain_append(&app->chain, genesis);
    char err[256];
    if (ok && !ledger_confirm(&app->ledger, &mint, 0, err, sizeof(err))) {
        printf("ERROR: Could not fund the library treasury: %s\n", err);
        ok = 0;
    }
    if (ok) {
        printf("Genesis block mined at difficulty %d after %llu attempt(s): hash %.16s...\n",
               app->meta.difficulty, attempts, genesis->hash);
        printf("Library treasury funded with %s coins (coinbase tx %.16s..., anchored in block 0).\n",
               fmt_amount(LIBRARY_INITIAL_SUPPLY), mint.txid);
        ok = app_save_all(app);
    }
    free(genesis);
    return ok;
}

void app_readmit_pending(App *app) {
    for (int i = 0; i < app->reg.member_count; i++) {
        ledger_add_account(&app->ledger, app->reg.members[i].member_id);
    }
    ledger_add_account(&app->ledger, LIBRARY_ACCOUNT);

    for (int i = 0; i < app->pending.count;) {
        PendingRequest *p = &app->pending.items[i];
        char err[256];
        if (p->has_tx && !ledger_admit(&app->ledger, &p->tx, err, sizeof(err))) {
            printf("WARNING: Pending request %s dropped: its transaction is no longer valid: %s\n",
                   p->rec.record_id, err);
            pending_remove(&app->pending, i);
            continue;
        }
        i++;
    }
}

/* Highest request number used on the chain or in the pool, so request IDs
 * never repeat even if meta.dat is lost. */
static long highest_request_seq(const App *app) {
    long max = 0;
    for (int b = 0; b < app->chain.count; b++) {
        for (int r = 0; r < app->chain.blocks[b].record_count; r++) {
            long n = atol(app->chain.blocks[b].records[r].record_id + 3);
            if (n > max) max = n;
        }
    }
    for (int i = 0; i < app->pending.count; i++) {
        long n = atol(app->pending.items[i].rec.record_id + 3);
        if (n > max) max = n;
    }
    return max;
}

static int ensure_member_keys(const App *app) {
    int created = 0;
    for (int i = 0; i < app->reg.member_count; i++) {
        int c = 0;
        if (!keystore_ensure_member(app->reg.members[i].member_id, &c)) return 0;
        created += c;
    }
    if (created) {
        printf("Generated %d member ECDSA P-256 key pair(s) in %s/ (each member signs their own "
               "requests).\n", created, MEMBER_KEYS_DIR);
    }
    return 1;
}

int app_open(App *app, LedgerModel model, int cli_difficulty, int cli_batch) {
    app->locked = 0;
    app->state = calloc(1, sizeof(LibraryState));
    LibraryState *replayed = calloc(1, sizeof(LibraryState));
    LibraryState *saved = calloc(1, sizeof(LibraryState));
    int ok = 0;
    if (!app->state || !replayed || !saved || !chain_init(&app->chain) ||
        !pending_init(&app->pending) || !ledger_init(&app->ledger, model)) {
        printf("ERROR: Out of memory\n");
        goto done;
    }
    for (int i = 0; i < app->reg.member_count; i++) {
        ledger_add_account(&app->ledger, app->reg.members[i].member_id);
    }
    ledger_add_account(&app->ledger, LIBRARY_ACCOUNT);

    /* All copies for a brand-new chain; a saved chain is replayed below,
     * which brings later copies in at the block they were added. */
    if (!state_load_copies(app->state, COPIES_FILE, &app->reg, 1 << 30)) goto done;
    printf("Loaded %d physical copies from %s.\n", app->state->copy_count, COPIES_FILE);
    if (!ensure_member_keys(app)) goto done;

    load_meta(app);
    if (cli_difficulty > 0) app->meta.difficulty = cli_difficulty;
    if (cli_batch > 0) app->meta.batch_size = cli_batch;

    StorageResult rc = chain_load(&app->chain, CHAIN_FILE);
    if (rc == STORAGE_ERROR) goto done;
    if (rc == STORAGE_MISSING) {
        if (storage_exists(LEDGER_FILE) || storage_exists(PENDING_FILE)) {
            printf("ERROR: '%s' is missing but '%s' or '%s' still exist. These files belong "
                   "together; run 'make clean' to start a fresh chain.\n",
                   CHAIN_FILE, LEDGER_FILE, PENDING_FILE);
            goto done;
        }
        ok = create_genesis(app);
        goto done;
    }
    printf("Loaded saved chain with %d block(s) from %s.\n", app->chain.count, CHAIN_FILE);

    rc = ledger_load(&app->ledger, LEDGER_FILE);
    if (rc == STORAGE_MISSING) {
        printf("ERROR: '%s' exists but the token ledger '%s' is missing. Restore it, or run "
               "'make clean' to start a fresh chain.\n", CHAIN_FILE, LEDGER_FILE);
        goto done;
    }
    if (rc == STORAGE_ERROR) goto done;
    printf("Replayed %d confirmed token transaction(s) into the %s model.\n",
           app->ledger.log_count, ledger_model_name(model));
    check_balance_snapshot(app);

    if (pending_load(&app->pending, PENDING_FILE) == STORAGE_ERROR) goto done;
    StorageResult src = state_load(saved, STATE_FILE);

    /* The chain must pass validate_chain before anything else may happen. */
    printf("\nRunning validate_chain on the chain loaded from disk...\n");
    app_validate(app, 1, replayed);

    if (!app->locked) {
        if (src == STORAGE_OK && state_compare(replayed, saved, 0) != 0) {
            printf("WARNING: %s does not match the state rebuilt from the chain:\n", STATE_FILE);
            state_compare(replayed, saved, 1);
            printf("         The chain is the source of truth; using the rebuilt state.\n");
        } else if (src != STORAGE_OK) {
            printf("NOTE: %s missing; inventory, queues and nonces rebuilt from the chain.\n", STATE_FILE);
        }
        *app->state = *replayed;
        app_readmit_pending(app);
        app_revalidate_pool(app);
    }

    long highest = highest_request_seq(app);
    if (app->meta.next_request_seq <= highest) app->meta.next_request_seq = highest + 1;
    if (app->pending.count > 0) {
        printf("%d request(s) waiting in the pending pool.\n", app->pending.count);
    }
    ok = 1;

done:
    free(replayed);
    free(saved);
    return ok;
}

void app_close(App *app) {
    chain_free(&app->chain);
    pending_free(&app->pending);
    ledger_free(&app->ledger);
    free(app->state);
    app->state = NULL;
}

int app_reload(App *app) {
    LedgerModel model = app->ledger.model;
    app_close(app);
    if (!registry_load(&app->reg, BOOKS_FILE, MEMBERS_FILE)) return 0;
    printf("Reloading the chain and all state from disk...\n");
    return app_open(app, model, 0, 0);
}

/* ======================= Validation ======================= */

static int check_record_transactions(const App *app, const Block *b, int r) {
    const LendingRecord *rec = &b->records[r];
    int problems = 0;

    if (rec->action == ACT_RETURNED && rec->token_reward > 0) {
        const Transaction *tx = ledger_find_tx(&app->ledger, b->reward_txids[r]);
        if (!tx || !tx_id_valid(tx) || tx->body.type != TX_REWARD ||
            strcmp(tx->body.recipient, rec->member_id) != 0 ||
            tx_total_cost(tx) != (Amount)rec->token_reward * COIN) {
            printf("  Block #%d: record %s: reward transaction %.8s is missing or does not pay "
                   "%d coins to %s\n", b->index, rec->record_id, b->reward_txids[r],
                   rec->token_reward, rec->member_id);
            problems++;
        }
        int expected = rec->fine_amount > 0 ? REWARD_LATE_COINS : REWARD_ON_TIME_COINS;
        if (rec->token_reward != expected) {
            printf("  Block #%d: record %s: token_reward %d but the return was %s\n", b->index,
                   rec->record_id, rec->token_reward, rec->fine_amount > 0 ? "late" : "on time");
            problems++;
        }
    }

    int carries = strcmp(rec->tx_id, ZERO_HASH) != 0;
    if (rec->action == ACT_TRANSFER && !carries) {
        printf("  Block #%d: TRANSFER record %s carries no transaction\n", b->index, rec->record_id);
        return problems + 1;
    }
    if (!carries) return problems;

    const Transaction *tx = ledger_find_tx(&app->ledger, rec->tx_id);
    const char *problem = NULL;
    if (!tx) problem = "its token transaction is not in the ledger";
    else if (!tx_id_valid(tx)) problem = "its token transaction does not match its SHA-256 ID";
    else if (strcmp(tx->body.sender, rec->member_id) != 0) problem = "the transaction is not sent by the record's member";
    else if (rec->action == ACT_FINE_PAID &&
             (strcmp(tx->body.recipient, LIBRARY_ACCOUNT) != 0 ||
              tx->body.amount != (Amount)rec->fine_amount * COIN)) {
        problem = "the fine payment does not pay fine_amount to the library";
    } else if (rec->action != ACT_FINE_PAID && rec->action != ACT_TRANSFER) {
        problem = "only TRANSFER and FINE_PAID records carry a transaction";
    }
    if (problem) {
        printf("  Block #%d: record %s: %s\n", b->index, rec->record_id, problem);
        problems++;
    }
    return problems;
}

int app_validate(App *app, int verbose, LibraryState *replayed) {
    int problems = 0, records = 0;

    LibraryState *replay = replayed ? replayed : calloc(1, sizeof(LibraryState));
    if (!replay) return 1;
    int replay_ok = state_load_copies(replay, COPIES_FILE, &app->reg, 0);
    if (!replay_ok) problems++;

    for (int i = 0; i < app->chain.count; i++) {
        const Block *b = &app->chain.blocks[i];
        int mask = chain_check_block(&app->chain, i, 1);
        for (int bit = mask; bit; bit &= bit - 1) problems++;

        if (i == 0) {
            const Transaction *mint = ledger_find_tx(&app->ledger, b->transaction_id);
            if (!mint || mint->body.type != TX_COINBASE ||
                strcmp(mint->body.recipient, LIBRARY_ACCOUNT) != 0) {
                printf("  Block #0: transaction_id is not the library treasury coinbase\n");
                problems++;
            }
            continue;
        }

        /* Copies added with add_copy exist from the block after which they
         * were added. */
        if (replay_ok) state_load_copies_added(replay, COPIES_FILE, &app->reg, i, i);

        for (int r = 0; r < b->record_count && r < MAX_RECORDS_PER_BLOCK; r++) {
            const LendingRecord *rec = &b->records[r];
            records++;

            /* Referential integrity with the registries. */
            Book *book = registry_find_book(&app->reg, rec->book_id);
            int needs_book = rec->action != ACT_TRANSFER && rec->action != ACT_FINE_PAID;
            if ((needs_book && !book) ||
                (strcmp(rec->member_id, "-") != 0 && !registry_find_member(&app->reg, rec->member_id))) {
                printf("  Block #%d: record %s refers to a book or member that is not registered\n",
                       i, rec->record_id);
                problems++;
            }

            /* Replay through the state machine: proves every nonce was used
             * exactly once and in order, and every transition was legal. */
            char err[256];
            if (replay_ok && !state_apply(replay, rec, err, sizeof(err))) {
                printf("  Block #%d: record %s (%s) is not a valid transition: %s\n", i,
                       rec->record_id, record_action_name(rec->action), err);
                problems++;
            }
            problems += check_record_transactions(app, b, r);
        }
    }

    if (replay_ok) state_load_copies_added(replay, COPIES_FILE, &app->reg, app->chain.count, 1 << 30);

    for (int i = 0; i < app->ledger.log_count; i++) {
        if (!tx_id_valid(&app->ledger.log[i])) {
            printf("  Ledger tx #%d (%.8s): contents do not match its SHA-256 ID\n",
                   i, app->ledger.log[i].txid);
            problems++;
        }
    }

    if (problems > 0) {
        printf("validate_chain: Chain is INVALID: %d problem(s) found.\n", problems);
        app->locked = 1;
    } else {
        app->locked = 0;
        if (verbose) {
            printf("validate_chain: Chain is VALID. %d block(s), %d lending record(s): hashes "
                   "recomputed, previous-hash links\nintact, proof of work met, timestamps "
                   "increasing, Merkle roots recomputed, every ECDSA signature\nverified against "
                   "the signer's registered key, every nonce and state transition replayed, and\n"
                   "all %d ledger transactions match their IDs and records.\n",
                   app->chain.count, records, app->ledger.log_count);
        }
    }
    if (!replayed) free(replay);
    return problems;
}

/* ======================= Pending pool ======================= */

void app_projected_state(const App *app, LibraryState *out) {
    *out = *app->state;
    for (int i = 0; i < app->pending.count; i++) {
        const PendingRequest *p = &app->pending.items[i];
        if (p->is_duplicate) continue;
        char err[256];
        state_apply(out, &p->rec, err, sizeof(err));
    }
}

void app_drop_request(App *app, int i, const char *reason) {
    PendingRequest *p = &app->pending.items[i];
    printf("  DROPPED request %s (%s %s): %s\n", p->rec.record_id, record_action_name(p->rec.action),
           p->rec.copy_id, reason);
    if (p->has_tx) ledger_release(&app->ledger, &p->tx);
    pending_remove(&app->pending, i);
}

static EVP_PKEY *signing_key_for(App *app, const char *signer) {
    char own[SIGNER_MAX];
    app_librarian_signer(app, own, sizeof(own));
    if (strncmp(signer, LIBRARIAN_SIGNER_PREFIX, strlen(LIBRARIAN_SIGNER_PREFIX)) == 0) {
        return strcmp(signer, own) == 0 ? app->session.signing_key : NULL;
    }
    return keystore_member_key(signer);
}

int app_revalidate_pool(App *app) {
    LibraryState *work = malloc(sizeof(LibraryState));
    if (!work) return 0;
    *work = *app->state;

    int changes = 0;
    char dropped_senders[16][LEDGER_ID_MAX];
    int dropped_count = 0;

    for (int i = 0; i < app->pending.count;) {
        PendingRequest *p = &app->pending.items[i];
        if (p->is_duplicate) {
            i++;
            continue;
        }

        char err[256];
        unsigned int expected = state_nonce(work, p->rec.signer);
        const char *reason = NULL;
        if (p->rec.nonce < expected) {
            reason = "nonce already used (replay)";
        } else if (p->rec.nonce > expected) {
            /* An earlier request of this signer was removed: re-sign with
             * the nonce that is now next, so the sequence has no gap. */
            EVP_PKEY *key = signing_key_for(app, p->rec.signer);
            unsigned int old = p->rec.nonce;
            p->rec.nonce = expected;
            if (key && record_sign(&p->rec, p->rec.signer, key)) {
                printf("  Request %s re-signed by %s with nonce %u (was %u).\n", p->rec.record_id,
                       p->rec.signer, expected, old);
                changes++;
            } else {
                reason = "nonce gap and the signer's key is not available";
            }
        }
        if (!reason && (!record_signature_valid(&p->rec) || !record_signer_registered(&p->rec))) {
            reason = "invalid or missing ECDSA signature";
        }
        if (!reason && p->has_tx) {
            for (int k = 0; k < dropped_count && !reason; k++) {
                if (strcmp(dropped_senders[k], p->tx.body.sender) == 0) {
                    reason = "an earlier transaction of the same sender was dropped (account nonce gap)";
                }
            }
        }
        if (!reason && !state_apply(work, &p->rec, err, sizeof(err))) reason = err;

        if (reason) {
            if (p->has_tx && dropped_count < 16) {
                snprintf(dropped_senders[dropped_count++], LEDGER_ID_MAX, "%s", p->tx.body.sender);
            }
            app_drop_request(app, i, reason);
            changes++;
            continue;
        }
        i++;
    }
    free(work);
    if (changes) app_save_pending(app);
    return changes;
}

int app_submit(App *app, PendingRequest *req, const char *signer, long explicit_nonce) {
    if (!app_require_unlocked(app)) return 0;

    LibraryState *proj = malloc(sizeof(LibraryState));
    if (!proj) return 0;
    app_projected_state(app, proj);

    LendingRecord *rec = &req->rec;
    snprintf(rec->record_id, sizeof(rec->record_id), "REQ%06ld", app->meta.next_request_seq);
    rec->timestamp = app_now(app);
    unsigned int expected = state_nonce(proj, signer);
    rec->nonce = explicit_nonce >= 0 ? (unsigned int)explicit_nonce : expected;
    req->status = REQ_PENDING;
    req->seq = app->meta.next_request_seq;
    if (req->waiting_since == 0) req->waiting_since = rec->timestamp;

    int ok = 0;
    char err[256], reason[sizeof(req->flag_reason)];
    EVP_PKEY *key = signing_key_for(app, signer);
    if (!key || !record_sign(rec, signer, key)) {
        printf("REJECTED: %s has no signing key available\n", signer);
        goto done;
    }
    /* Every request is verified against the signer's public key before it
     * may enter the pool. */
    if (!record_signature_valid(rec) || !record_signer_registered(rec)) {
        printf("REJECTED: ECDSA signature of %s does not verify\n", signer);
        goto done;
    }
    if (rec->nonce != expected) {
        printf("REJECTED: nonce %u is not %s's expected next nonce %u (%s).\n", rec->nonce, signer,
               expected, rec->nonce < expected ? "replay of a used nonce" : "nonces must not skip");
        goto done;
    }

    int suspicious = fraud_check(app, rec, proj, reason, sizeof(reason));
    if (!state_apply(proj, rec, err, sizeof(err))) {
        printf("REJECTED: %s\n", err);
        goto done;
    }
    if (req->has_tx) {
        if (strcmp(rec->tx_id, req->tx.txid) != 0) {
            printf("REJECTED: the record does not reference its transaction\n");
            goto done;
        }
        if (!ledger_admit(&app->ledger, &req->tx, err, sizeof(err))) {
            printf("REJECTED: token transaction: %s\n", err);
            goto done;
        }
    }

    if (suspicious) {
        req->status = REQ_SUSPICIOUS;
        snprintf(req->flag_reason, sizeof(req->flag_reason), "%s", reason);
    }
    if (!pending_push(&app->pending, req)) {
        if (req->has_tx) ledger_release(&app->ledger, &req->tx);
        goto done;
    }
    app->meta.next_request_seq++;
    app_save_pending(app);

    printf("%s: request %s (%s) signed by %s with nonce %u, ECDSA signature verified, added to the "
           "pending pool%s.\n", suspicious ? "FLAGGED" : "SUBMITTED", rec->record_id,
           record_action_name(rec->action), signer, rec->nonce,
           req->priority == PRIORITY_OVERDUE ? " [priority: overdue-related]" : "");
    if (suspicious) {
        printf("  SUSPICIOUS: %s\n  It is withheld from mining until a librarian runs "
               "approve_suspicious %s or reject_suspicious %s.\n", reason, rec->record_id,
               rec->record_id);
    }
    ok = 1;

done:
    free(proj);
    return ok;
}

/* ======================= Overdue detection ======================= */

int app_overdue_scan(App *app, int verbose) {
    if (app->locked || !app->session.signing_key) return 0;
    LibraryState *proj = malloc(sizeof(LibraryState));
    if (!proj) return 0;
    app_projected_state(app, proj);

    time_t now = app_now(app);
    char signer[SIGNER_MAX];
    app_librarian_signer(app, signer, sizeof(signer));

    int flagged = 0;
    for (int i = 0; i < proj->copy_count; i++) {
        const Copy *c = &proj->copies[i];
        if (c->state != COPY_BORROWED || c->overdue_flagged || c->due_date >= now) continue;

        const Book *book = registry_find_book(&app->reg, c->book_id);
        const Member *m = registry_find_member(&app->reg, c->current_holder_id);
        if (!book || !m) continue;

        int days = days_overdue(c->due_date, now);
        PendingRequest req;
        memset(&req, 0, sizeof(req));
        record_init(&req.rec, ACT_OVERDUE);
        snprintf(req.rec.book_id, sizeof(req.rec.book_id), "%s", c->book_id);
        snprintf(req.rec.copy_id, sizeof(req.rec.copy_id), "%s", c->copy_id);
        snprintf(req.rec.book_title, sizeof(req.rec.book_title), "%s", book->title);
        snprintf(req.rec.member_id, sizeof(req.rec.member_id), "%s", m->member_id);
        snprintf(req.rec.member_name, sizeof(req.rec.member_name), "%s", m->full_name);
        req.rec.due_date = c->due_date;
        req.rec.fine_amount = days * FINE_PER_DAY;
        req.priority = PRIORITY_OVERDUE;

        printf("OVERDUE DETECTED: copy %s ('%s') held by %s (%s) is %d day(s) past its due date; "
               "fine accrued so far: %d coin(s).\n", c->copy_id, book->title, m->full_name,
               m->member_id, days, req.rec.fine_amount);
        if (app_submit(app, &req, signer, -1)) flagged++;
    }
    if (verbose && flagged == 0) printf("Overdue scan: no newly overdue copies.\n");
    free(proj);
    return flagged;
}

int app_pay_coinbase(App *app, const char *account, Amount amount, const char *memo) {
    if (amount <= 0) return 1;

    /* The log position makes every coinbase ID unique. */
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
