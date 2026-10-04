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
#define CLOUD_RENTAL_FEE (20 * COIN)  /* per round, billed by the provider */
#define CLOUD_MAINTENANCE_PERCENT 10  /* of each round's mining reward */

/* Benchmark */
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

/* ======================= Shared confirmation steps ======================= */

/* The miner's validation of a pending block before spending work on it:
 * the librarian's signature must verify and any attached transaction must
 * match the block. (The ledger re-checks balances, inputs and nonces when
 * the transaction is confirmed.) */
static int check_pending_entry(const PendingEntry *e, char *err, size_t err_size) {
    const Block *b = &e->block;
    if (!block_signature_valid(b)) {
        snprintf(err, err_size, "librarian %s's signature on the block does not verify",
                 b->recorded_by);
        return 0;
    }

    int is_return = strcmp(b->action, ACTION_RETURNED) == 0;
    int is_transfer = strcmp(b->action, ACTION_TRANSFER) == 0;
    if (!e->has_tx) {
        if (is_return || is_transfer || b->token_reward != 0 || strcmp(b->tx_id, ZERO_HASH) != 0) {
            snprintf(err, err_size, "block claims a token transaction but none is attached");
            return 0;
        }
        return 1;
    }

    const Transaction *tx = &e->tx;
    if (!tx_id_valid(tx)) {
        snprintf(err, err_size, "attached transaction does not match its SHA-256 ID");
        return 0;
    }
    if (strcmp(tx->txid, b->tx_id) != 0) {
        snprintf(err, err_size, "block's tx_id does not match the attached transaction");
        return 0;
    }
    if (is_return) {
        if (tx->body.type != TX_REWARD || strcmp(tx->body.recipient, b->member_id) != 0 ||
            tx_total_cost(tx) != (Amount)b->token_reward * COIN) {
            snprintf(err, err_size, "reward transaction does not match the block's token reward");
            return 0;
        }
    } else if (is_transfer) {
        if (tx->body.type != TX_TRANSFER || strcmp(tx->body.sender, b->member_id) != 0) {
            snprintf(err, err_size, "transfer transaction does not belong to the block's member");
            return 0;
        }
    } else {
        snprintf(err, err_size, "only RETURNED and TRANSFER blocks may carry a transaction");
        return 0;
    }
    return 1;
}

/* Removes an invalid block from the front of the pool and frees the funds
 * its transaction had reserved. */
static void drop_front(App *app, const char *reason) {
    PendingEntry *e = &app->pending.items[0];
    printf("  DROPPED pending %s block for '%s': %s\n", e->block.action, e->block.book_title, reason);
    if (e->has_tx) ledger_release(&app->ledger, &e->tx);
    pending_remove(&app->pending, 0);
    app_save_pending(app);
}

static void describe_entry(const PendingEntry *e) {
    const Block *b = &e->block;
    printf("%s '%s' / %s (token reward %d", b->action, b->book_title, b->member_name,
           b->token_reward);
    if (e->has_tx) printf(", tx %.8s", e->tx.txid);
    printf(")\n");
}

/* Shows the effect of a confirmed block under the chosen model: the whole
 * UTXO set (UTXO model) or the updated accounts (account model). */
static void show_state_after_block(App *app, const PendingEntry *e) {
    if (app->ledger.model == MODEL_UTXO) {
        ledger_print_utxo_set(&app->ledger);
        return;
    }
    if (!e->has_tx) {
        printf("  No token transaction in this block: balances unchanged.\n");
        return;
    }
    printf("  Account balances updated:\n");
    ledger_print_account_line(&app->ledger, e->tx.body.sender, &app->reg);
    ledger_print_account_line(&app->ledger, e->tx.body.recipient, &app->reg);
}

/* Commits the front pending block once its proof of work is found: appends
 * it to the chain, confirms its transaction in the ledger (undoing the
 * append if that fails), removes it from the pool and saves everything.
 * Returns 1 if confirmed, with the transaction fee in *fee_out. */
static int confirm_front(App *app, const Block *mined, Amount *fee_out, int show_state) {
    PendingEntry e = app->pending.items[0];
    *fee_out = 0;

    int ok = chain_append(&app->chain, mined);
    if (ok && e.has_tx) {
        char err[256];
        Transaction tx = e.tx;
        if (ledger_confirm(&app->ledger, &tx, 1, err, sizeof(err))) {
            *fee_out = tx.body.fee;
        } else {
            printf("  REJECTED at confirmation: %s\n", err);
            chain_pop(&app->chain);
            ok = 0;
        }
    }
    if (!ok && e.has_tx) ledger_release(&app->ledger, &e.tx);

    pending_remove(&app->pending, 0);
    app_save_all(app);

    if (ok) {
        printf("  CONFIRMED as block #%d and added to the chain.\n", mined->index);
        if (show_state) show_state_after_block(app, &e);
    }
    return ok;
}

/* ======================= Solo mining ======================= */

static void mine_solo(App *app) {
    char miner_account[LEDGER_ID_MAX];
    app_miner_account(app, miner_account, sizeof(miner_account));
    int d = app->difficulty;

    printf("\n=== SOLO MINING ===\n");
    printf("Miner: %s (reward account %s)\n", app->session.username, miner_account);
    printf("Difficulty: %d (hash must start with %d zero(s); ~%.0f attempts expected per block)\n",
           d, d, pow_expected_attempts(d));
    printf("Pending blocks: %d\n", app->pending.count);

    int confirmed = 0, first_index = -1, last_index = -1;
    Amount fees = 0;
    unsigned long long total_attempts = 0;
    double total_ms = 0;

    while (app->pending.count > 0) {
        PendingEntry *e = &app->pending.items[0];
        printf("\n[Mining] ");
        describe_entry(e);

        char err[256];
        if (!check_pending_entry(e, err, sizeof(err))) {
            drop_front(app, err);
            continue;
        }
        printf("  Validated: librarian signature OK%s\n",
               e->has_tx ? ", reward/transfer transaction OK" : "");

        /* Proof of work: increment the nonce and re-hash until the hash
         * starts with `d` zeros. */
        Block b = e->block;
        PowJob job;
        if (!pow_begin(&job, &b, app->chain.count, chain_tip_hash(&app->chain), d,
                       app->session.username)) {
            printf("ERROR: Could not start proof-of-work\n");
            return;
        }
        double t0 = now_ms();
        unsigned long long attempts = 0;
        int r = 0;
        for (unsigned long long nonce = 0; r == 0; nonce++) {
            attempts++;
            r = pow_try(&job, nonce, b.hash);
            if (r == 1) b.nonce = nonce;
        }
        pow_end(&job);
        double ms = now_ms() - t0;
        if (r == POW_ERROR) {
            printf("ERROR: Hashing failed\n");
            return;
        }

        total_attempts += attempts;
        total_ms += ms;
        printf("  Proof of work found: nonce %llu after %llu hash attempts (%.2f ms)\n",
               b.nonce, attempts, ms);
        printf("  Block hash: %s\n", b.hash);

        Amount fee;
        if (confirm_front(app, &b, &fee, 1)) {
            confirmed++;
            fees += fee;
            if (first_index < 0) first_index = b.index;
            last_index = b.index;
        }
    }

    if (confirmed == 0) {
        printf("\nNo blocks were confirmed, so no mining reward is paid.\n");
        return;
    }

    Amount block_rewards = (Amount)confirmed * BLOCK_REWARD;
    printf("\n--- Solo mining summary ---\n");
    printf("Blocks confirmed    : %d (#%d to #%d)\n", confirmed, first_index, last_index);
    printf("Hash attempts       : %llu in total, %.0f per block on average (expected ~%.0f at "
           "difficulty %d)\n", total_attempts, (double)total_attempts / confirmed,
           pow_expected_attempts(d), d);
    printf("Mining time         : %.2f ms\n", total_ms);
    printf("Block rewards       : %d x %s = %s coins\n", confirmed, fmt_amount(BLOCK_REWARD),
           fmt_amount(block_rewards));
    printf("Transaction fees    : %s coins\n", fmt_amount(fees));
    printf("Paid to solo miner  : %s coins (the solo miner keeps 100%%)\n",
           fmt_amount(block_rewards + fees));

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

/* Mines one block as a pool. All miners hash at the same time, each on its
 * own nonces (miner k of n tries k, k+n, k+2n, ..., so no work is repeated).
 * A round is one time unit in which miner k makes hash_rate attempts, spread
 * evenly. The attempts of all miners are taken in time order, and the first
 * valid hash ends the search for everyone, so each miner's attempt count
 * follows its hash rate. Returns the winning miner, or -1 on error. */
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
            /* The miner whose next attempt comes earliest in this round. */
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
            if (pick < 0) break;   /* everyone used their attempts: next round */

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

static void mine_pool(App *app) {
    int n;
    if (!prompt_int_range("Number of miners in the pool (2-8) [4]: ", POOL_MIN_MINERS,
                          POOL_MAX_MINERS, POOL_DEFAULT_MINERS, &n)) return;

    PoolMiner miners[POOL_MAX_MINERS];
    memset(miners, 0, sizeof(miners));
    for (int k = 0; k < n; k++) {
        snprintf(miners[k].id, sizeof(miners[k].id), "%s%d", POOL_MINER_PREFIX, k + 1);
        miners[k].hash_rate = POOL_MIN_HASH_RATE + rand() % (POOL_MAX_HASH_RATE - POOL_MIN_HASH_RATE + 1);
    }

    int d = app->difficulty;
    printf("\n=== POOL MINING ===\n");
    printf("Difficulty: %d (~%.0f attempts expected per block). Pool fee: %d%%.\n", d,
           pow_expected_attempts(d), POOL_FEE_PERCENT);
    printf("Miners and their randomly assigned hash rates (attempts per round):\n");
    for (int k = 0; k < n; k++) printf("  %s: %d\n", miners[k].id, miners[k].hash_rate);

    int confirmed = 0;
    Amount fees = 0;
    unsigned long long total_attempts = 0;

    while (app->pending.count > 0) {
        PendingEntry *e = &app->pending.items[0];
        printf("\n[Pool mining] ");
        describe_entry(e);

        char err[256];
        if (!check_pending_entry(e, err, sizeof(err))) {
            drop_front(app, err);
            continue;
        }

        Block b = e->block;
        PowJob job;
        if (!pow_begin(&job, &b, app->chain.count, chain_tip_hash(&app->chain), d, "POOL")) {
            printf("ERROR: Could not start proof-of-work\n");
            return;
        }
        int rounds = 0;
        unsigned long long attempts = 0;
        int winner = pool_mine_block(miners, n, &job, &b, &rounds, &attempts);
        pow_end(&job);
        if (winner < 0) {
            printf("ERROR: Hashing failed\n");
            return;
        }

        miners[winner].blocks_found++;
        total_attempts += attempts;
        printf("  Solved in round %d by %s: nonce %llu, %llu pool attempts for this block\n",
               rounds, miners[winner].id, b.nonce, attempts);
        printf("  Block hash: %s\n", b.hash);

        Amount fee;
        if (confirm_front(app, &b, &fee, 1)) {
            confirmed++;
            fees += fee;
        }
    }

    if (confirmed == 0 || total_attempts == 0) {
        printf("\nNo blocks were confirmed, so no mining reward is paid.\n");
        return;
    }

    /* Reward sharing: 2% pool fee first, the rest split by contribution:
     *   share = miner_attempts / total_attempts * (total reward - pool fee)
     * Integer coin-cents are used, and the rounding remainder goes to the
     * operator, so every cent is accounted for. */
    Amount total_reward = (Amount)confirmed * BLOCK_REWARD + fees;
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
           fmt_amount(total_reward), confirmed, fmt_amount(BLOCK_REWARD), fmt_amount(fees));
    printf("Pool fee (%d%%)    : %s coins -> %s\n", POOL_FEE_PERCENT, fmt_amount(pool_fee),
           POOL_OPERATOR_ACCOUNT);
    printf("Shared by miners : %s coins, by share of hash attempts\n", fmt_amount(distributable));
    print_pool_table(miners, n, total_attempts, distributed);
    if (remainder > 0) {
        printf("Rounding remainder of %s coins goes to %s.\n", fmt_amount(remainder),
               POOL_OPERATOR_ACCOUNT);
    }

    printf("\nPayouts:\n");
    for (int k = 0; k < n; k++) {
        app_pay_coinbase(app, miners[k].id, miners[k].reward, "pool mining share");
    }
    app_pay_coinbase(app, POOL_OPERATOR_ACCOUNT, pool_fee + remainder, "pool fee");
    app_save_all(app);
}

/* ======================= Cloud mining ======================= */

typedef struct {
    unsigned long long attempts;
    int blocks;
    Amount reward;          /* gross mining reward earned this round */
    Amount rental_fee;
    Amount maintenance_fee;
} CloudRound;

static void print_cloud_summary(const CloudRound *rounds, int count) {
    printf("\n--- Cloud mining: multi-round earnings summary ---\n");
    printf("%-5s %9s %6s %10s %10s %10s %12s %10s %10s  %s\n", "Round", "Attempts", "Blocks",
           "Reward", "Rental", "Maint.", "Cum.Reward", "Cum.Fees", "Cum.Net", "Status");
    print_line(104);

    Amount cum_reward = 0, cum_fees = 0, total_rental = 0, total_maint = 0;
    int unprofitable_rounds = 0;
    for (int i = 0; i < count; i++) {
        const CloudRound *r = &rounds[i];
        cum_reward += r->reward;
        cum_fees += r->rental_fee + r->maintenance_fee;
        total_rental += r->rental_fee;
        total_maint += r->maintenance_fee;
        int unprofitable = cum_fees > cum_reward;
        unprofitable_rounds += unprofitable;
        printf("%-5d %9llu %6d %10s %10s %10s %12s %10s %10s  %s\n", i + 1, r->attempts, r->blocks,
               fmt_amount(r->reward), fmt_amount(r->rental_fee), fmt_amount(r->maintenance_fee),
               fmt_amount(cum_reward), fmt_amount(cum_fees), fmt_amount(cum_reward - cum_fees),
               unprofitable ? "UNPROFITABLE" : "ok");
    }
    print_line(104);

    Amount net = cum_reward - cum_fees;
    printf("Gross earnings   : %s coins\n", fmt_amount(cum_reward));
    printf("Total fees paid  : %s coins (rental %s + maintenance %s)\n", fmt_amount(cum_fees),
           fmt_amount(total_rental), fmt_amount(total_maint));
    printf("Net profit       : %s coins\n", fmt_amount(net));
    if (net < 0) {
        printf("WARNING: This rental was UNPROFITABLE overall: the fees exceeded the mining rewards.\n");
    } else if (unprofitable_rounds > 0) {
        printf("Note: the rental was unprofitable in %d round(s) but ended with a profit.\n",
               unprofitable_rounds);
    } else {
        printf("The rental was profitable in every round.\n");
    }
}

static void mine_cloud(App *app) {
    char renter[LEDGER_ID_MAX], miner_name[MINER_NAME_MAX];
    app_miner_account(app, renter, sizeof(renter));
    snprintf(miner_name, sizeof(miner_name), "CLOUD:%s", app->session.username);
    int d = app->difficulty;

    printf("\n=== CLOUD MINING ===\n");
    printf("Rental plan: %d hash attempts per round, rental fee %s coins per round,\n",
           CLOUD_HASH_POWER, fmt_amount(CLOUD_RENTAL_FEE));
    printf("maintenance fee %d%% of each round's mining reward. Rewards go to %s.\n",
           CLOUD_MAINTENANCE_PERCENT, renter);
    printf("At difficulty %d a block needs ~%.0f attempts, i.e. ~%.2f block(s) per round.\n", d,
           pow_expected_attempts(d), CLOUD_HASH_POWER / pow_expected_attempts(d));

    int rounds;
    if (!prompt_int_range("Rental duration in rounds (1-5): ", CLOUD_MIN_ROUNDS, CLOUD_MAX_ROUNDS,
                          CLOUD_MIN_ROUNDS - 1, &rounds)) return;

    CloudRound history[CLOUD_MAX_ROUNDS];
    memset(history, 0, sizeof(history));
    Amount cum_reward = 0, cum_fees = 0;

    /* The block being mined, kept across rounds so partial work carries
     * over to the next round. */
    int job_active = 0;
    PowJob job;
    Block b;
    unsigned long long next_nonce = 0;

    for (int round = 1; round <= rounds; round++) {
        CloudRound *r = &history[round - 1];
        printf("\n-- Round %d of %d --\n", round, rounds);

        int budget = CLOUD_HASH_POWER;
        while (budget > 0) {
            if (!job_active) {
                if (app->pending.count == 0) {
                    printf("  Pending pool is empty: the rented hash power is idle for the rest "
                           "of this round.\n");
                    break;
                }
                char err[256];
                if (!check_pending_entry(&app->pending.items[0], err, sizeof(err))) {
                    drop_front(app, err);
                    continue;
                }
                printf("  Working on: ");
                describe_entry(&app->pending.items[0]);
                b = app->pending.items[0].block;
                if (!pow_begin(&job, &b, app->chain.count, chain_tip_hash(&app->chain), d,
                               miner_name)) {
                    printf("ERROR: Could not start proof-of-work\n");
                    return;
                }
                job_active = 1;
                next_nonce = 0;
            }

            budget--;
            r->attempts++;
            int found = pow_try(&job, next_nonce, b.hash);
            if (found == POW_ERROR) {
                pow_end(&job);
                printf("ERROR: Hashing failed\n");
                return;
            }
            if (found == 1) {
                b.nonce = next_nonce;
                pow_end(&job);
                job_active = 0;
                printf("  Proof of work found: nonce %llu, hash %.20s...\n", b.nonce, b.hash);
                Amount fee;
                if (confirm_front(app, &b, &fee, 1)) {
                    r->blocks++;
                    r->reward += BLOCK_REWARD + fee;
                }
            }
            next_nonce++;
        }
        if (job_active) {
            printf("  Out of hash power for this round after %llu attempts on the current block; "
                   "the search continues next round.\n", next_nonce);
        }

        /* Fees for this round, and payout of the reward minus maintenance. */
        r->rental_fee = CLOUD_RENTAL_FEE;
        r->maintenance_fee = r->reward * CLOUD_MAINTENANCE_PERCENT / 100;
        cum_reward += r->reward;
        cum_fees += r->rental_fee + r->maintenance_fee;

        printf("  Round %d: %llu attempts, %d block(s) confirmed, reward %s, rental fee %s, "
               "maintenance %s\n", round, r->attempts, r->blocks, fmt_amount(r->reward),
               fmt_amount(r->rental_fee), fmt_amount(r->maintenance_fee));
        app_pay_coinbase(app, renter, r->reward - r->maintenance_fee, "cloud mining reward");
        app_pay_coinbase(app, CLOUD_HOST_ACCOUNT, r->maintenance_fee, "cloud maintenance fee");
        printf("  Cumulative: rewards %s, fees %s, net %s\n", fmt_amount(cum_reward),
               fmt_amount(cum_fees), fmt_amount(cum_reward - cum_fees));
        if (cum_fees > cum_reward) {
            printf("  WARNING: after round %d the cumulative fees (%s) exceed the cumulative "
                   "rewards (%s): this rental is UNPROFITABLE.\n", round, fmt_amount(cum_fees),
                   fmt_amount(cum_reward));
        }
    }

    if (job_active) {
        pow_end(&job);
        printf("\nThe rental ended before the current block was solved; it stays in the "
               "pending pool.\n");
    }
    app_save_all(app);

    if (app->ledger.model == MODEL_UTXO) {
        printf("\n");
        ledger_print_utxo_set(&app->ledger);
    }
    print_cloud_summary(history, rounds);
    printf("(Rental fees are billed by the provider off-chain; the ledger receives the reward "
           "minus maintenance.)\n");
    ledger_print_account_line(&app->ledger, renter, &app->reg);
}

/* ======================= Menu, difficulty, benchmark ======================= */

void mining_menu(App *app) {
    if (app->pending.count == 0) {
        printf("The pending pool is empty: nothing to mine. Borrow, return, mark overdue or "
               "transfer tokens first.\n");
        return;
    }
    if (!app_ensure_intact(app)) return;

    pending_print(&app->pending, app->difficulty);
    printf("\nMining method:\n");
    printf("  1. Solo mining  (you mine alone and keep the full reward)\n");
    printf("  2. Pool mining  (simulated miners share the work; rewards split by attempts, %d%% fee)\n",
           POOL_FEE_PERCENT);
    printf("  3. Cloud mining (rent hash power for 1-5 rounds)\n");
    printf("  0. Cancel\n");

    int choice;
    if (!prompt_int_range("Choose a method: ", 0, 3, -1, &choice)) return;
    switch (choice) {
        case 1: mine_solo(app); break;
        case 2: mine_pool(app); break;
        case 3: mine_cloud(app); break;
        default: printf("Mining cancelled.\n"); break;
    }
}

void mining_set_difficulty(App *app) {
    if (!app_require_admin(app)) return;

    char prompt[64];
    snprintf(prompt, sizeof(prompt), "New difficulty (%d-%d, current %d): ", MIN_DIFFICULTY,
             MAX_DIFFICULTY, app->difficulty);
    int d;
    if (!prompt_int_range(prompt, MIN_DIFFICULTY, MAX_DIFFICULTY, MIN_DIFFICULTY - 1, &d)) return;

    app->difficulty = d;
    printf("Difficulty set to %d: new blocks need a hash starting with %d zero(s) "
           "(~%.0f attempts each).\n", d, d, pow_expected_attempts(d));
    printf("Blocks already on the chain keep the difficulty they were mined at (stored in each "
           "block).\n");
}

void mining_benchmark(void) {
    printf("\nDifficulty benchmark: mining %d sample blocks at each difficulty...\n", BENCH_TRIALS);

    Block sample;
    memset(&sample, 0, sizeof(sample));
    snprintf(sample.book_title, sizeof(sample.book_title), "BENCHMARK");
    snprintf(sample.action, sizeof(sample.action), "BENCH");
    snprintf(sample.tx_id, sizeof(sample.tx_id), "%s", ZERO_HASH);

    printf("\n%-10s %-9s %12s %12s %10s %10s %12s\n", "Difficulty", "Target", "Expected",
           "Avg tries", "Min", "Max", "Avg time ms");
    print_line(81);

    for (int d = MIN_DIFFICULTY; d <= MAX_DIFFICULTY; d++) {
        unsigned long long total = 0, min = 0, max = 0;
        double ms = 0;
        for (int t = 0; t < BENCH_TRIALS; t++) {
            sample.timestamp = (time_t)(1000 * d + t);   /* a different puzzle each time */
            double t0 = now_ms();
            unsigned long long attempts = pow_mine(&sample, t, ZERO_HASH, d, "benchmark");
            ms += now_ms() - t0;
            if (attempts == 0) {
                printf("ERROR: Hashing failed\n");
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
    printf("\nEach extra leading zero makes a valid hash 16 times rarer, so the expected work\n"
           "(and time) grows 16x per difficulty level. Single runs vary a lot because mining\n"
           "is random: min and max show that spread.\n");
}
