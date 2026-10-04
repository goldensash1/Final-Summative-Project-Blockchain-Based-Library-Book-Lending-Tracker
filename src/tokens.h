#ifndef TOKENS_H
#define TOKENS_H

#include "app.h"

/* Balance table for all members and other accounts (plus the full UTXO set
 * under the UTXO model). */
void tokens_view_balances(App *app);

/* Manual member-to-member transfer. Under the account model the user enters
 * the nonce, so an incorrect or reused nonce can be demonstrated. The
 * transfer becomes a TRANSFER block in the pending pool. */
void tokens_transfer(App *app);

/* Full transaction history of any account (member ID, LIBRARY, MINER_x...). */
void tokens_history(App *app);

/* Re-submits an already confirmed transaction unchanged. It must be
 * rejected: as a double spend (UTXO) or as a reused nonce (account). */
void tokens_replay_test(App *app);

#endif
