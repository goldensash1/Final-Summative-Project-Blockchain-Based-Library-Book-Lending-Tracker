#ifndef MINING_H
#define MINING_H

#include "app.h"

/* Mining is the confirmation step: a lending block only joins the chain (and
 * its reward transaction only changes balances) once a miner has validated
 * it and found a nonce that gives its hash the required leading zeros.
 *
 * All three methods mine the same pending pool, oldest block first:
 *   Solo  - the logged-in user mines alone and keeps the whole reward.
 *   Pool  - simulated miners with random hash rates share the work; rewards
 *           are split by each miner's share of the hash attempts, after a
 *           2% pool fee.
 *   Cloud - the user rents hash power for 1-5 rounds, paying a rental fee
 *           per round; rewards are credited each round minus a maintenance
 *           fee, and an unprofitable rental is flagged. */

/* Lets the user pick solo / pool / cloud and mines the pending pool. */
void mining_menu(App *app);

/* Changes the difficulty used for newly mined blocks (ADMIN only). */
void mining_set_difficulty(App *app);

/* Mines sample blocks at every difficulty (1-4) and reports the average
 * hash attempts and time, showing how difficulty affects mining effort. */
void mining_benchmark(void);

#endif
