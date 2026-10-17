#ifndef FRAUD_H
#define FRAUD_H

#include "app.h"

/* Misuse and fraud detection. A request that triggers a heuristic stays in
 * the pending pool with status SUSPICIOUS, is never mined while in that
 * status, and must be approved (approve_suspicious) or rejected
 * (reject_suspicious) by a librarian.
 *
 *   Excessive concurrent borrows  the member already holds MAX_CONCURRENT_BORROWS copies
 *   Rapid re-borrow pattern       same member re-borrows the same title within 24 h of returning it
 *   Duplicate request             the request_id already exists on the chain or in the pool
 *   Condition manipulation        a return downgrades the copy's condition by more than one
 *                                 level without a librarian override reason code */

/* Runs the heuristics against the state before the request (`pre`). Returns
 * 1 and writes the reasons if any heuristic triggers. */
int fraud_check(const App *app, const LendingRecord *rec, const LibraryState *pre,
                char *reason, size_t size);

/* 1 if request_id already exists on the chain or in the pending pool. */
int fraud_request_id_exists(const App *app, const char *request_id);

/* CLI commands */
void cmd_fraud_review(App *app, int argc, char **argv);
void cmd_approve_suspicious(App *app, int argc, char **argv);
void cmd_reject_suspicious(App *app, int argc, char **argv);
void cmd_replay_request(App *app, int argc, char **argv);

#endif
