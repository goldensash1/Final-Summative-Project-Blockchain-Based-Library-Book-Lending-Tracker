#ifndef TOKENS_H
#define TOKENS_H

#include "app.h"

/* Token and ledger commands. A transfer is a TRANSFER request signed with
 * the sender's member key; its transaction is admitted (funds reserved) when
 * it enters the pending pool and applied when it is mined.
 *   token_transfer <from> <to> <amount>      under the session's model
 *   account_transfer <from> <to> <amount> [nonce]   account model, explicit nonce
 *   token_balance <id> / utxo_view [owner] / account_balance [id] / account_nonce <id>
 *   transaction_history <id>
 *   replay_tx <txid-prefix>                  double-spend / nonce replay test */
void cmd_token_transfer(App *app, int argc, char **argv);
void cmd_account_transfer(App *app, int argc, char **argv);
void cmd_token_balance(App *app, int argc, char **argv);
void cmd_utxo_view(App *app, int argc, char **argv);
void cmd_account_balance(App *app, int argc, char **argv);
void cmd_account_nonce(App *app, int argc, char **argv);
void cmd_transaction_history(App *app, int argc, char **argv);
void cmd_replay_tx(App *app, int argc, char **argv);

#endif
