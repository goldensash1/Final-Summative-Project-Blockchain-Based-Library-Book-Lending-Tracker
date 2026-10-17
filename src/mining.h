#ifndef MINING_H
#define MINING_H

#include "app.h"

/* Mining is the confirmation step: a request only reaches the chain (and a
 * return's reward only changes a balance) once a miner has put it in a block
 * and found a nonce that gives the block hash the required leading zeros.
 *
 * Every method builds blocks the same way: the top `batch size` PENDING
 * requests of the prioritised pool (SUSPICIOUS ones are withheld), each
 * re-verified (ECDSA signature, signer key, nonce, state transition). The
 * miner creates the reward transaction of every RETURNED record.
 *   mine_solo          one miner mines every block and keeps the whole reward
 *   mine_pool [n]      n simulated miners with random hash rates share the
 *                      work; rewards split by attempts after a 2% pool fee
 *   mine_cloud <r>     rented hash power for r rounds (1-5), with a rental
 *                      fee per round and a profit/loss summary */

void cmd_pool_view(App *app, int argc, char **argv);
void cmd_mine_solo(App *app, int argc, char **argv);
void cmd_mine_pool(App *app, int argc, char **argv);
void cmd_mine_cloud(App *app, int argc, char **argv);
void cmd_difficulty_status(App *app, int argc, char **argv);
void cmd_set_difficulty(App *app, int argc, char **argv);
void cmd_benchmark(App *app, int argc, char **argv);

#endif
