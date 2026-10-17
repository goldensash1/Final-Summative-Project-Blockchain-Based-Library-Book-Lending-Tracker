#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mining.h"
#include "input.h"

/* Pool mining */
#define POOL_MIN_MINERS 2
#define POOL_MAX_MINERS 8
#define POOL_DEFAULT_MINERS 4
#define POOL_MIN_HASH_RATE 100        /* attempts per round */
#define POOL_MAX_HASH_RATE 500
#define POOL_FEE_PERCENT 2

/* Cloud mining */
#define CLOUD_MIN_ROUNDS 1
#define CLOUD_MAX_ROUNDS 5
#define CLOUD_HASH_POWER 400          /* rented attempts per round */
#define CLOUD_RENTAL_FEE (20 * COIN)  /* fixed fee per round, billed by the provider */

#define BENCH_TRIALS 20

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void print_line(int width) {
    for (int i = 0; i < width; i++) putchar('-');
    putchar('\n');
}

/* ======================= Building a block ======================= */

/* A block being mined: its records (chosen from the pool), the reward
 * transactions the miner created, and the state after applying them. */
typedef struct {
    Block block;
    char request_ids[MAX_RECORDS_PER_BLOCK][RECORD_ID_MAX];
    Transaction reward_tx[MAX_RECORDS_PER_BLOCK];
    int has_reward[MAX_RECORDS_PER_BLOCK];
    Amount fees;                 /* transaction fees collected by this block */
    LibraryState *work;
} Batch;

static void batch_free(Batch *bt) {
    free(bt->work);
    bt->work = NULL;
}

/* Re-verifies signatures before any work is spent: a request whose
 * signature or signer key fails is dropped (never mined). */
static void drop_unverifiable(App *app) {
    for (int i = 0; i < app->pending.count;) {
        const PendingRequest *p = &app->pending.items[i];
        if (!p->is_duplicate && (!record_signature_valid(&p->rec) || !record_signer_registered(&p->rec))) {
            app_drop_request(app, i, "ECDSA signature invalid or missing");
            continue;
        }
        i++;
    }
}

/* Selects up to batch_size requests in priority order and prepares the
 * block body. A request that is not yet valid (e.g. its signer's earlier
 * nonce is not mined yet) is deferred; selection repeats until no more
 * requests fit. Returns the number of records (0 = nothing to mine). */
static int batch_prepare(App *app, Batch *bt, int verbose) {
    memset(bt, 0, sizeof(*bt));
    drop_unverifiable(app);
    if (app->pending.count == 0) return 0;

    bt->work = malloc(sizeof(LibraryState));
    int *order = malloc(sizeof(int) * (size_t)app->pending.count);
    char *chosen = calloc((size_t)app->pending.count, 1);
    if (!bt->work || !order || !chosen) {
        free(order);
        free(chosen);
        batch_free(bt);
        return 0;
    }
    *bt->work = *app->state;
    pending_priority_order(&app->pending, order);

    Block *b = &bt->block;
    int max = app->meta.batch_size, progress = 1;
    while (progress && b->record_count < max) {
        progress = 0;
        for (int k = 0; k < app->pending.count && b->record_count < max; k++) {
            int idx = order[k];
            const PendingRequest *p = &app->pending.items[idx];
            if (chosen[idx] || p->status == REQ_SUSPICIOUS || p->is_duplicate) continue;
            char err[256];
            if (!state_apply(bt->work, &p->rec, err, sizeof(err))) continue;
            chosen[idx] = 1;
            int slot = b->record_count++;
            b->records[slot] = p->rec;
            snprintf(bt->request_ids[slot], RECORD_ID_MAX, "%s", p->rec.record_id);
            snprintf(b->reward_txids[slot], sizeof(b->reward_txids[slot]), "%s", ZERO_HASH);
            if (p->has_tx) bt->fees += p->tx.body.fee;
            progress = 1;
        }
    }

    if (verbose) {
        for (int k = 0; k < app->pending.count; k++) {
            const PendingRequest *p = &app->pending.items[order[k]];
            if (p->status == REQ_SUSPICIOUS) {
                printf("  Withheld %s (%s): SUSPICIOUS, awaiting fraud review\n", p->rec.record_id,
                       record_action_name(p->rec.action));
            } else if (!chosen[order[k]] && !p->is_duplicate && b->record_count < max) {
                /* The block has room, so this request cannot apply yet
                 * (state_apply fails and changes nothing). */
                char err[256];
                if (!state_apply(bt->work, &p->rec, err, sizeof(err))) {
                    printf("  Deferred %s (%s): %s\n", p->rec.record_id,
                           record_action_name(p->rec.action), err);
                }
            }
        }
    }
    free(order);
    free(chosen);
    if (b->record_count == 0) {
        batch_free(bt);
        return 0;
    }

    /* The miner creates each return's reward: the library treasury pays
     * token_reward coins, the fixed fee goes to the miner and the rest is
     * credited to the member. */
    unsigned int library_nonce = ledger_next_nonce(&app->ledger, LIBRARY_ACCOUNT);
    for (int r = 0; r < b->record_count; r++) {
        const LendingRecord *rec = &b->records[r];
        if (rec->action != ACT_RETURNED || rec->token_reward <= 0) continue;
        char memo[TX_MEMO_MAX];
        snprintf(memo, sizeof(memo), "%s return reward %s", rec->fine_amount > 0 ? "late" : "on-time",
                 rec->record_id);
        if (!tx_init(&bt->reward_tx[r], TX_REWARD, LIBRARY_ACCOUNT, rec->member_id,
                     (Amount)rec->token_reward * COIN - TX_FEE, TX_FEE, library_nonce++, memo)) {
            batch_free(bt);
            return 0;
        }
        bt->has_reward[r] = 1;
        snprintf(b->reward_txids[r], sizeof(b->reward_txids[r]), "%s", bt->reward_tx[r].txid);
        bt->fees += TX_FEE;
    }

    b->timestamp = app_block_time(app);
    if (!block_finalize_body(b)) {
        batch_free(bt);
        return 0;
    }
    return b->record_count;
}

static void print_batch(const Batch *bt) {
    const Block *b = &bt->block;
    printf("  Block body: %d record(s), Merkle root %.16s..., token_reward %d\n", b->record_count,
           b->merkle_root, b->token_reward);
    for (int r = 0; r < b->record_count; r++) {
        const LendingRecord *rec = &b->records[r];
        printf("    %-10s %-9s %-9s %-7s nonce %-3u signature VALID", rec->record_id,
               record_action_name(rec->action), rec->copy_id, rec->member_id, rec->nonce);
        if (bt->has_reward[r]) {
            printf("  -> reward tx %.8s: %d coins (%s to member + %s fee)", bt->reward_tx[r].txid,
                   rec->token_reward, fmt_amount(bt->reward_tx[r].body.amount),
                   fmt_amount(bt->reward_tx[r].body.fee));
        }
        printf("\n");
    }
}

/* Shows the effect of a confirmed block under the chosen model. */
static void show_state_after_block(App *app, const Block *b) {
    if (app->ledger.model == MODEL_UTXO) {
        ledger_print_utxo_set(&app->ledger);
        return;
    }
    printf("  Account balances updated:\n");
    ledger_print_account_line(&app->ledger, LIBRARY_ACCOUNT, &app->reg);
    for (int r = 0; r < b->record_count; r++) {
        int seen = 0;
        for (int k = 0; k < r; k++) seen |= strcmp(b->records[k].member_id, b->records[r].member_id) == 0;
        if (!seen && strcmp(b->records[r].member_id, "-") != 0) {
            ledger_print_account_line(&app->ledger, b->records[r].member_id, &app->reg);
        }
        /* The other side of a member transfer. */
        const Transaction *tx = ledger_find_tx(&app->ledger, b->records[r].tx_id);
        if (tx && strcmp(tx->body.recipient, LIBRARY_ACCOUNT) != 0) {
            ledger_print_account_line(&app->ledger, tx->body.recipient, &app->reg);
        }
    }
}

/* Appends a mined block and confirms everything in it: the token
 * transactions (rolled back together with the block if any fails), the new
 * inventory / queue / nonce / fine state, and the removal of the confirmed
 * requests from the pool. Returns 1 if confirmed. */
static int batch_commit(App *app, Batch *bt) {
    Block *b = &bt->block;
    if (!chain_append(&app->chain, b)) return 0;

    int log_before = app->ledger.log_count;
    for (int r = 0; r < b->record_count; r++) {
        char err[256];
        int idx = pending_find_genuine(&app->pending, bt->request_ids[r]);
        int ok = idx >= 0;
        if (ok && app->pending.items[idx].has_tx) {
            Transaction tx = app->pending.items[idx].tx;
            ok = ledger_confirm(&app->ledger, &tx, 1, err, sizeof(err));
        }
        if (ok && bt->has_reward[r]) ok = ledger_confirm(&app->ledger, &bt->reward_tx[r], 0, err, sizeof(err));
        if (!ok) {
            printf("  REJECTED at confirmation (%s): %s. The block is discarded.\n",
                   bt->request_ids[r], idx < 0 ? "request vanished" : err);
            chain_pop(&app->chain);
            ledger_rollback(&app->ledger, log_before);
            if (idx >= 0) pending_remove(&app->pending, idx);
            app_readmit_pending(app);
            app_revalidate_pool(app);
            app_save_all(app);
            return 0;
        }
    }

    for (int r = 0; r < b->record_count; r++) {
        const LendingRecord *rec = &b->records[r];
        int idx = pending_find_genuine(&app->pending, bt->request_ids[r]);
        if (idx >= 0) {
            app->pending.items[idx].status = REQ_CONFIRMED;
            pending_remove(&app->pending, idx);
        }
        /* Report automatic reservation offers made by this block. */
        if (rec->action == ACT_RETURNED) {
            const Copy *c = state_find_copy_const(bt->work, rec->copy_id);
            if (c && c->state == COPY_RESERVED) {
                printf("  RESERVATION OFFER: returned copy %s is now RESERVED for %s (first in the "
                       "queue for %s).\n", c->copy_id, c->reserved_for, c->book_id);
            }
        }
    }
    *app->state = *bt->work;
    app_save_all(app);

    printf("  CONFIRMED as block #%d: %d request(s) status CONFIRMED and removed from the pool.\n",
           b->index, b->record_count);
    show_state_after_block(app, b);
    return 1;
}

static int mining_ready(App *app) {
    if (!app_require_unlocked(app)) return 0;
    int pending = 0;
    for (int i = 0; i < app->pending.count; i++) {
        pending += app->pending.items[i].status == REQ_PENDING && !app->pending.items[i].is_duplicate;
    }
    if (pending == 0) {
        printf("Nothing to mine: the pending pool has no PENDING requests%s.\n",
               app->pending.count ? " (only SUSPICIOUS ones, see fraud_review)" : "");
        return 0;
    }
    return 1;
}

/* ======================= Solo mining ======================= */

void cmd_mine_solo(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!mining_ready(app)) return;

    char miner_account[LEDGER_ID_MAX];
    app_miner_account(app, miner_account, sizeof(miner_account));
    int d = app->meta.difficulty;

    printf("\n=== SOLO MINING ===\n");
    printf("Miner: %s (reward account %s). Difficulty %d: hash must start with %d zero(s), "
           "~%.0f attempts expected per block.\n", app->session.username, miner_account, d, d,
           pow_expected_attempts(d));
    printf("Block size: up to %d record(s), taken in priority order.\n", app->meta.batch_size);

    int confirmed = 0, first_index = -1, last_index = -1;
    Amount fees = 0;
    unsigned long long total_attempts = 0;
    double total_ms = 0;

    Batch bt;
    while (batch_prepare(app, &bt, confirmed == 0)) {
        printf("\n[Mining block #%d]\n", app->chain.count);
        print_batch(&bt);

        PowJob job;
        if (!pow_begin(&job, &bt.block, app->chain.count, chain_tip_hash(&app->chain), d,
                       app->session.username)) {
            printf("ERROR: Could not start proof-of-work\n");
            batch_free(&bt);
            return;
        }
        double t0 = now_ms();
        unsigned long long attempts = 0;
        int r = 0;
        for (unsigned long long nonce = 0; r == 0; nonce++) {
            attempts++;
            r = pow_try(&job, nonce, bt.block.hash);
            if (r == 1) bt.block.nonce = nonce;
        }
        pow_end(&job);
        double ms = now_ms() - t0;
        if (r == POW_ERROR) {
            printf("ERROR: Hashing failed\n");
            batch_free(&bt);
            return;
        }

        total_attempts += attempts;
        total_ms += ms;
        printf("  Proof of work found: nonce %llu after %llu hash attempts (%.2f ms)\n",
               bt.block.nonce, attempts, ms);
        printf("  Block hash: %s\n", bt.block.hash);

        Amount block_fees = bt.fees;
        if (batch_commit(app, &bt)) {
            confirmed++;
            fees += block_fees;
            if (first_index < 0) first_index = bt.block.index;
            last_index = bt.block.index;
        }
        batch_free(&bt);
    }

    if (confirmed == 0) {
        printf("\nNo blocks were confirmed, so no mining reward is paid.\n");
        return;
    }

    Amount block_rewards = (Amount)confirmed * app->meta.block_reward;
    printf("\n--- Solo mining summary ---\n");
    printf("Blocks confirmed    : %d (#%d to #%d)\n", confirmed, first_index, last_index);
    printf("Hash attempts       : %llu in total, %.0f per block on average (expected ~%.0f at "
           "difficulty %d)\n", total_attempts, (double)total_attempts / confirmed,
           pow_expected_attempts(d), d);
    printf("Mining time         : %.2f ms\n", total_ms);
    printf("Block rewards       : %d x %s = %s coins\n", confirmed, fmt_amount(app->meta.block_reward),
           fmt_amount(block_rewards));
    printf("Transaction fees    : %s coins\n", fmt_amount(fees));
    printf("Paid to solo miner  : %s coins (the solo miner keeps 100%%, on top of the members' "
           "token rewards)\n", fmt_amount(block_rewards + fees));

    char memo[TX_MEMO_MAX];
    snprintf(memo, sizeof(memo), "solo mining reward blocks #%d-#%d", first_index, last_index);
    app_pay_coinbase(app, miner_account, block_rewards + fees, memo);
    app_save_all(app);
    ledger_print_account_line(&app->ledger, miner_account, &app->reg);
}

/* ======================= Pool mining ======================= */

typedef struct {
    char id[LEDGER_ID_MAX];
    int hash_rate;                 /* attempts per round */
    unsigned long long attempts;   /* total contribution */
    int blocks_found;
    Amount reward;
} PoolMiner;

/* Mines one block as a pool. Miner k of n tries nonces k, k+n, k+2n, ...
 * (no repeated work). In each round miner k makes hash_rate attempts spread
 * evenly over the round; attempts are taken in time order and the first
 * valid hash ends the search, so each miner's attempt count follows its hash
 * rate. Returns the winning miner, or -1 on error. */
static int pool_mine_block(PoolMiner *miners, int n, PowJob *job, Block *b,
                           int *rounds_out, unsigned long long *attempts_out) {
    unsigned long long next_nonce[POOL_MAX_MINERS];
    for (int k = 0; k < n; k++) next_nonce[k] = (unsigned long long)k;

    unsigned long long attempts = 0;
    int winner = -1, rounds = 0;
    while (winner < 0) {
        rounds++;
        int done[POOL_MAX_MINERS] = {0};
        while (winner < 0) {
            int pick = -1;
            double earliest = 2.0;
            for (int k = 0; k < n; k++) {
                if (done[k] >= miners[k].hash_rate) continue;
                double t = (done[k] + 0.5) / miners[k].hash_rate;
                if (t < earliest) {
                    earliest = t;
                    pick = k;
                }
            }
            if (pick < 0) break;   /* everyone used this round's attempts */

            done[pick]++;
            miners[pick].attempts++;
            attempts++;
            unsigned long long nonce = next_nonce[pick];
            next_nonce[pick] += (unsigned long long)n;
            int r = pow_try(job, nonce, b->hash);
            if (r == POW_ERROR) return -1;
            if (r == 1) {
                winner = pick;
                b->nonce = nonce;
            }
        }
    }
    *rounds_out = rounds;
    *attempts_out = attempts;
    return winner;
}

static void print_pool_table(const PoolMiner *miners, int n, unsigned long long total_attempts,
                             Amount distributed) {
    const char *sep = "+-----------+-----------+------------+----------+--------+-------------+";
    printf("%s\n", sep);
    printf("| %-9s | %-9s | %10s | %8s | %6s | %11s |\n", "Miner ID", "Hash rate", "Attempts",
           "Share %", "Blocks", "Reward");
    printf("%s\n", sep);
    int total_blocks = 0;
    for (int k = 0; k < n; k++) {
        double share = 100.0 * (double)miners[k].attempts / (double)total_attempts;
        char rate[16];
        snprintf(rate, sizeof(rate), "%d/rnd", miners[k].hash_rate);
        printf("| %-9s | %9s | %10llu | %7.2f%% | %6d | %11s |\n", miners[k].id, rate,
               miners[k].attempts, share, miners[k].blocks_found, fmt_amount(miners[k].reward));
        total_blocks += miners[k].blocks_found;
    }
    printf("%s\n", sep);
    printf("| %-9s | %9s | %10llu | %7.2f%% | %6d | %11s |\n", "TOTAL", "", total_attempts,
           100.0, total_blocks, fmt_amount(distributed));
    printf("%s\n", sep);
}

void cmd_mine_pool(App *app, int argc, char **argv) {
    if (!mining_ready(app)) return;
    int n = POOL_DEFAULT_MINERS;
    const char *arg = cli_positional(argc, argv, 1);
    if (arg && (!parse_int(arg, &n) || n < POOL_MIN_MINERS || n > POOL_MAX_MINERS)) {
        printf("ERROR: Number of miners must be %d-%d\n", POOL_MIN_MINERS, POOL_MAX_MINERS);
        return;
    }

    PoolMiner miners[POOL_MAX_MINERS];
    memset(miners, 0, sizeof(miners));
    for (int k = 0; k < n; k++) {
        snprintf(miners[k].id, sizeof(miners[k].id), "%s%d", POOL_MINER_PREFIX, k + 1);
        miners[k].hash_rate = POOL_MIN_HASH_RATE + rand() % (POOL_MAX_HASH_RATE - POOL_MIN_HASH_RATE + 1);
    }

    int d = app->meta.difficulty;
    printf("\n=== POOL MINING ===\n");
    printf("%d miners. Difficulty %d (~%.0f attempts expected per block). Pool fee: %d%%.\n", n, d,
           pow_expected_attempts(d), POOL_FEE_PERCENT);
    printf("Randomly assigned hash rates (attempts per round):");
    for (int k = 0; k < n; k++) printf(" %s=%d", miners[k].id, miners[k].hash_rate);
    printf("\n");

    int confirmed = 0;
    Amount fees = 0;
    unsigned long long total_attempts = 0;

    Batch bt;
    while (batch_prepare(app, &bt, confirmed == 0)) {
        printf("\n[Pool mining block #%d]\n", app->chain.count);
        print_batch(&bt);

        PowJob job;
        if (!pow_begin(&job, &bt.block, app->chain.count, chain_tip_hash(&app->chain), d, "POOL")) {
            printf("ERROR: Could not start proof-of-work\n");
            batch_free(&bt);
            return;
        }
        int rounds = 0;
        unsigned long long attempts = 0;
        int winner = pool_mine_block(miners, n, &job, &bt.block, &rounds, &attempts);
        pow_end(&job);
        if (winner < 0) {
            printf("ERROR: Hashing failed\n");
            batch_free(&bt);
            return;
        }
        miners[winner].blocks_found++;
        total_attempts += attempts;
        printf("  Solved in round %d by %s: nonce %llu, %llu pool hash attempts for this block\n",
               rounds, miners[winner].id, bt.block.nonce, attempts);
        printf("  Block hash: %s\n", bt.block.hash);

        Amount block_fees = bt.fees;
        if (batch_commit(app, &bt)) {
            confirmed++;
            fees += block_fees;
        }
        batch_free(&bt);
    }

    if (confirmed == 0 || total_attempts == 0) {
        printf("\nNo blocks were confirmed, so no mining reward is paid.\n");
        return;
    }

    /* Reward sharing: the 2% pool fee first, then
     *   share_k = miner_attempts_k / total_attempts * (block rewards + fees - pool fee)
     * in integer coin-cents; the rounding remainder goes to the operator. */
    Amount total_reward = (Amount)confirmed * app->meta.block_reward + fees;
    Amount pool_fee = total_reward * POOL_FEE_PERCENT / 100;
    Amount distributable = total_reward - pool_fee;
    Amount distributed = 0;
    for (int k = 0; k < n; k++) {
        miners[k].reward = distributable * (Amount)miners[k].attempts / (Amount)total_attempts;
        distributed += miners[k].reward;
    }
    Amount remainder = distributable - distributed;

    printf("\n--- Pool reward distribution ---\n");
    printf("Total reward     : %s coins (%d block(s) x %s + %s transaction fees)\n",
           fmt_amount(total_reward), confirmed, fmt_amount(app->meta.block_reward), fmt_amount(fees));
    printf("Pool fee (%d%%)    : %s coins -> %s\n", POOL_FEE_PERCENT, fmt_amount(pool_fee),
           POOL_OPERATOR_ACCOUNT);
    printf("Shared by miners : %s coins, by share of hash attempts\n", fmt_amount(distributable));
    print_pool_table(miners, n, total_attempts, distributed);
    if (remainder > 0) {
        printf("Rounding remainder of %s coins goes to %s.\n", fmt_amount(remainder), POOL_OPERATOR_ACCOUNT);
    }

    printf("\nPayouts:\n");
    for (int k = 0; k < n; k++) app_pay_coinbase(app, miners[k].id, miners[k].reward, "pool mining share");
    app_pay_coinbase(app, POOL_OPERATOR_ACCOUNT, pool_fee + remainder, "pool fee");
    app_save_all(app);
}

/* ======================= Cloud mining ======================= */

typedef struct {
    unsigned long long attempts;
    int blocks;
    Amount reward;       /* mining reward earned this round */
    Amount rental_fee;
} CloudRound;

static void print_cloud_summary(const CloudRound *rounds, int count) {
    printf("\n--- Cloud mining: multi-round earnings summary ---\n");
    printf("%-5s %9s %6s %10s %10s %12s %10s %10s  %s\n", "Round", "Attempts", "Blocks", "Reward",
           "Rental", "Cum.Reward", "Cum.Fees", "Cum.Net", "Status");
    print_line(92);

    Amount cum_reward = 0, cum_fees = 0;
    int unprofitable_rounds = 0;
    for (int i = 0; i < count; i++) {
        const CloudRound *r = &rounds[i];
        cum_reward += r->reward;
        cum_fees += r->rental_fee;
        int unprofitable = cum_fees > cum_reward;
        unprofitable_rounds += unprofitable;
        printf("%-5d %9llu %6d %10s %10s %12s %10s %10s  %s\n", i + 1, r->attempts, r->blocks,
               fmt_amount(r->reward), fmt_amount(r->rental_fee), fmt_amount(cum_reward),
               fmt_amount(cum_fees), fmt_amount(cum_reward - cum_fees),
               unprofitable ? "FEES > REWARDS" : "ok");
    }
    print_line(92);

    Amount net = cum_reward - cum_fees;
    printf("Gross earnings   : %s coins\n", fmt_amount(cum_reward));
    printf("Total fees paid  : %s coins (%d round(s) x %s rental)\n", fmt_amount(cum_fees), count,
           fmt_amount(CLOUD_RENTAL_FEE));
    printf("Net profit       : %s coins\n", fmt_amount(net));
    if (net < 0) printf("WARNING: This rental was UNPROFITABLE overall: the fees exceeded the mining rewards.\n");
    else if (unprofitable_rounds > 0) printf("Note: fees exceeded rewards in %d round(s), but the rental ended in profit.\n", unprofitable_rounds);
    else printf("The rental was profitable in every round.\n");
}

void cmd_mine_cloud(App *app, int argc, char **argv) {
    if (!mining_ready(app)) return;
    char buf[16];
    int rounds;
    if (!cli_arg(argc, argv, 1, "Rental duration in rounds (1-5): ", buf, sizeof(buf)) ||
        !parse_int(buf, &rounds) || rounds < CLOUD_MIN_ROUNDS || rounds > CLOUD_MAX_ROUNDS) {
        printf("ERROR: The rental duration must be %d to %d rounds\n", CLOUD_MIN_ROUNDS, CLOUD_MAX_ROUNDS);
        return;
    }

    char renter[LEDGER_ID_MAX], miner_name[MINER_NAME_MAX];
    app_miner_account(app, renter, sizeof(renter));
    snprintf(miner_name, sizeof(miner_name), "CLOUD:%s", app->session.username);
    int d = app->meta.difficulty;

    printf("\n=== CLOUD MINING ===\n");
    printf("Rental: %d rounds x %d hash attempts, fixed rental fee %s coins per round. Rewards go to %s.\n",
           rounds, CLOUD_HASH_POWER, fmt_amount(CLOUD_RENTAL_FEE), renter);
    printf("At difficulty %d a block needs ~%.0f attempts, i.e. ~%.2f block(s) per round.\n", d,
           pow_expected_attempts(d), CLOUD_HASH_POWER / pow_expected_attempts(d));

    CloudRound history[CLOUD_MAX_ROUNDS];
    memset(history, 0, sizeof(history));
    Amount cum_reward = 0, cum_fees = 0;

    /* The block being mined is kept across rounds, so partial work carries
     * over to the next round. */
    int job_active = 0;
    PowJob job;
    Batch bt;
    memset(&bt, 0, sizeof(bt));
    unsigned long long next_nonce = 0;

    for (int round = 1; round <= rounds; round++) {
        CloudRound *r = &history[round - 1];
        printf("\n-- Round %d of %d --\n", round, rounds);

        int budget = CLOUD_HASH_POWER;
        while (budget > 0) {
            if (!job_active) {
                if (!batch_prepare(app, &bt, 0)) {
                    printf("  No eligible requests left: the rented hash power is idle for the rest "
                           "of this round.\n");
                    break;
                }
                printf("  Working on block #%d:\n", app->chain.count);
                print_batch(&bt);
                if (!pow_begin(&job, &bt.block, app->chain.count, chain_tip_hash(&app->chain), d,
                               miner_name)) {
                    printf("ERROR: Could not start proof-of-work\n");
                    batch_free(&bt);
                    return;
                }
                job_active = 1;
                next_nonce = 0;
            }

            budget--;
            r->attempts++;
            int found = pow_try(&job, next_nonce, bt.block.hash);
            if (found == POW_ERROR) {
                pow_end(&job);
                batch_free(&bt);
                printf("ERROR: Hashing failed\n");
                return;
            }
            if (found == 1) {
                bt.block.nonce = next_nonce;
                pow_end(&job);
                job_active = 0;
                printf("  Proof of work found: nonce %llu after %llu attempts, hash %.20s...\n",
                       bt.block.nonce, next_nonce + 1, bt.block.hash);
                Amount block_fees = bt.fees;
                if (batch_commit(app, &bt)) {
                    r->blocks++;
                    r->reward += app->meta.block_reward + block_fees;
                }
                batch_free(&bt);
            }
            next_nonce++;
        }
        if (job_active) {
            printf("  Out of hash power for this round after %llu attempts on the current block; "
                   "the search continues next round.\n", next_nonce);
        }

        r->rental_fee = CLOUD_RENTAL_FEE;
        cum_reward += r->reward;
        cum_fees += r->rental_fee;
        printf("  Round %d: %llu attempts, %d block(s), reward %s, rental fee %s\n", round,
               r->attempts, r->blocks, fmt_amount(r->reward), fmt_amount(r->rental_fee));
        app_pay_coinbase(app, renter, r->reward, "cloud mining reward");
        printf("  Cumulative: rewards %s, fees %s, net %s\n", fmt_amount(cum_reward),
               fmt_amount(cum_fees), fmt_amount(cum_reward - cum_fees));
        if (cum_fees > cum_reward) {
            printf("  WARNING: after round %d the cumulative fees (%s) exceed the cumulative rewards "
                   "(%s): this rental is UNPROFITABLE so far.\n", round, fmt_amount(cum_fees),
                   fmt_amount(cum_reward));
        }
    }

    if (job_active) {
        pow_end(&job);
        batch_free(&bt);
        printf("\nThe rental ended before the current block was solved; its requests stay in the "
               "pending pool.\n");
    }
    app_save_all(app);
    print_cloud_summary(history, rounds);
    printf("(Rental fees are paid to the cloud provider off-chain; the ledger receives the mining rewards.)\n");
    ledger_print_account_line(&app->ledger, renter, &app->reg);
}

/* ======================= Pool view, difficulty, benchmark ======================= */

void cmd_pool_view(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    pending_print(&app->pending, app->meta.batch_size, app->meta.difficulty);
}

void cmd_difficulty_status(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    int d = app->meta.difficulty;
    printf("\nMining difficulty: %d (valid range %d-%d)\n", d, MIN_DIFFICULTY, MAX_DIFFICULTY);
    printf("  Target          : block hash must start with %.*s (%d zero hex digit(s))\n", d, "0000", d);
    printf("  Expected effort : ~%.0f hash attempts per block (16^%d)\n", pow_expected_attempts(d), d);
    printf("  Block reward    : %s coins per block + transaction fees\n", fmt_amount(app->meta.block_reward));
    printf("  Block size      : up to %d record(s) per block (fixed-size batching)\n", app->meta.batch_size);
    printf("  Chain height    : %d block(s); tip %.16s...\n", app->chain.count, chain_tip_hash(&app->chain));
    if (app->chain.count > 1) {
        printf("  Recent blocks   :");
        for (int i = app->chain.count > 6 ? app->chain.count - 5 : 1; i < app->chain.count; i++) {
            printf(" #%d d=%d nonce=%llu;", i, app->chain.blocks[i].difficulty, app->chain.blocks[i].nonce);
        }
        printf("\n");
    }
    printf("Change it with: set_difficulty <1-4>   (or start with --difficulty N)\n");
}

void cmd_set_difficulty(App *app, int argc, char **argv) {
    if (!app_require_admin(app)) return;
    char buf[16];
    int d;
    if (!cli_arg(argc, argv, 1, "New difficulty (1-4): ", buf, sizeof(buf)) || !parse_int(buf, &d) ||
        d < MIN_DIFFICULTY || d > MAX_DIFFICULTY) {
        printf("ERROR: Difficulty must be a number from %d to %d\n", MIN_DIFFICULTY, MAX_DIFFICULTY);
        return;
    }
    app->meta.difficulty = d;
    app_save_pending(app);   /* also saves meta.dat */
    printf("Difficulty set to %d: new blocks need a hash starting with %d zero(s) (~%.0f attempts each).\n",
           d, d, pow_expected_attempts(d));
    printf("Blocks already on the chain keep the difficulty they were mined at (stored in each block).\n");
}

void cmd_benchmark(App *app, int argc, char **argv) {
    (void)app;
    (void)argc;
    (void)argv;
    printf("\nDifficulty benchmark: mining %d sample blocks at each difficulty...\n", BENCH_TRIALS);
    Block *sample = calloc(1, sizeof(Block));
    if (!sample) return;
    snprintf(sample->merkle_root, sizeof(sample->merkle_root), "%s", ZERO_HASH);
    snprintf(sample->transaction_id, sizeof(sample->transaction_id), "%s", ZERO_HASH);

    printf("\n%-10s %-9s %12s %12s %10s %10s %12s\n", "Difficulty", "Target", "Expected",
           "Avg tries", "Min", "Max", "Avg time ms");
    print_line(81);
    for (int d = MIN_DIFFICULTY; d <= MAX_DIFFICULTY; d++) {
        unsigned long long total = 0, min = 0, max = 0;
        double ms = 0;
        for (int t = 0; t < BENCH_TRIALS; t++) {
            sample->timestamp = (time_t)(1000 * d + t);   /* a different puzzle each time */
            double t0 = now_ms();
            unsigned long long attempts = pow_mine(sample, t, ZERO_HASH, d, "benchmark");
            ms += now_ms() - t0;
            if (attempts == 0) {
                printf("ERROR: Hashing failed\n");
                free(sample);
                return;
            }
            total += attempts;
            if (t == 0 || attempts < min) min = attempts;
            if (attempts > max) max = attempts;
        }
        char target[MAX_DIFFICULTY + 4];
        snprintf(target, sizeof(target), "%.*s...", d, "0000");
        printf("%-10d %-9s %12.0f %12.0f %10llu %10llu %12.3f\n", d, target,
               pow_expected_attempts(d), (double)total / BENCH_TRIALS, min, max, ms / BENCH_TRIALS);
    }
    free(sample);
    printf("\nEach extra leading zero makes a valid hash 16 times rarer, so the expected work (and\n"
           "time) grows 16x per difficulty level.\n");
}
