#include <stdio.h>
#include <string.h>

#include "fraud.h"
#include "input.h"

/* Appends "; reason" to buf. */
static void add_reason(char *buf, size_t size, const char *reason) {
    size_t used = strlen(buf);
    snprintf(buf + used, size - used, "%s%s", used ? "; " : "", reason);
}

/* Timestamp of the member's most recent return of the title (chain or
 * pool), or 0. */
static time_t last_return_of(const App *app, const char *member_id, const char *book_id) {
    time_t last = 0;
    for (int b = 0; b < app->chain.count; b++) {
        const Block *blk = &app->chain.blocks[b];
        for (int r = 0; r < blk->record_count; r++) {
            const LendingRecord *rec = &blk->records[r];
            if (rec->action == ACT_RETURNED && strcmp(rec->member_id, member_id) == 0 &&
                strcmp(rec->book_id, book_id) == 0 && rec->timestamp > last) {
                last = rec->timestamp;
            }
        }
    }
    for (int i = 0; i < app->pending.count; i++) {
        const LendingRecord *rec = &app->pending.items[i].rec;
        if (!app->pending.items[i].is_duplicate && rec->action == ACT_RETURNED &&
            strcmp(rec->member_id, member_id) == 0 && strcmp(rec->book_id, book_id) == 0 &&
            rec->timestamp > last) {
            last = rec->timestamp;
        }
    }
    return last;
}

int fraud_check(const App *app, const LendingRecord *rec, const LibraryState *pre,
                char *reason, size_t size) {
    reason[0] = '\0';
    char msg[160];

    if (rec->action == ACT_BORROWED) {
        int held = state_held_count(pre, rec->member_id);
        if (held >= MAX_CONCURRENT_BORROWS) {
            snprintf(msg, sizeof(msg), "excessive concurrent borrows: %s already holds %d copies "
                     "(maximum %d)", rec->member_id, held, MAX_CONCURRENT_BORROWS);
            add_reason(reason, size, msg);
        }
        time_t returned = last_return_of(app, rec->member_id, rec->book_id);
        if (returned && rec->timestamp - returned < REBORROW_WINDOW_SECONDS) {
            snprintf(msg, sizeof(msg), "rapid re-borrow: %s returned %s %.1f hour(s) ago "
                     "(window 24 h)", rec->member_id, rec->book_id,
                     (double)(rec->timestamp - returned) / 3600.0);
            add_reason(reason, size, msg);
        }
    }

    if (rec->action == ACT_RETURNED) {
        const Copy *c = state_find_copy_const(pre, rec->copy_id);
        if (c && rec->condition - c->condition > 1 && rec->reason_code[0] == '\0') {
            snprintf(msg, sizeof(msg), "condition manipulation: %s downgraded %s -> %s (%d levels) "
                     "without a librarian override reason code", rec->copy_id,
                     condition_name(c->condition), condition_name(rec->condition),
                     rec->condition - c->condition);
            add_reason(reason, size, msg);
        }
    }
    return reason[0] != '\0';
}

int fraud_request_id_exists(const App *app, const char *request_id) {
    for (int b = 0; b < app->chain.count; b++) {
        for (int r = 0; r < app->chain.blocks[b].record_count; r++) {
            if (strcmp(app->chain.blocks[b].records[r].record_id, request_id) == 0) return 1;
        }
    }
    return pending_find(&app->pending, request_id) >= 0;
}

/* ======================= Review commands ======================= */

void cmd_fraud_review(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    int shown = 0;
    printf("\nFraud review: requests flagged SUSPICIOUS (withheld from mining)\n");
    for (int i = 0; i < app->pending.count; i++) {
        const PendingRequest *p = &app->pending.items[i];
        if (p->status != REQ_SUSPICIOUS) continue;
        char ts[24];
        format_datetime(p->rec.timestamp, ts, sizeof(ts));
        printf("\n  %s  %s copy %s, member %s (%s), signed by %s, nonce %u, %s\n", p->rec.record_id,
               record_action_name(p->rec.action), p->rec.copy_id, p->rec.member_id,
               p->rec.member_name, p->rec.signer, p->rec.nonce, ts);
        printf("    Signature: %s\n", record_signature_valid(&p->rec) ? "VALID" : "INVALID");
        printf("    Reason   : %s\n", p->flag_reason);
        printf("    Decide   : approve_suspicious %s%s | reject_suspicious %s\n", p->rec.record_id,
               p->is_duplicate ? " (not allowed: duplicate)" : "", p->rec.record_id);
        shown++;
    }
    if (shown == 0) printf("  (no suspicious requests)\n");
    int approved = 0;
    for (int i = 0; i < app->pending.count; i++) approved += app->pending.items[i].approved;
    if (approved) printf("\n%d request(s) were approved after review and are waiting to be mined.\n", approved);
}

static int find_suspicious(App *app, int argc, char **argv) {
    char id[RECORD_ID_MAX + 8];
    if (!cli_id_arg(argc, argv, 1, "Request ID: ", id, sizeof(id))) return -1;
    int i = -1;
    for (int k = 0; k < app->pending.count && i < 0; k++) {
        /* A replay shares its request_id with the original: prefer the flagged one. */
        if (strcmp(app->pending.items[k].rec.record_id, id) == 0 &&
            app->pending.items[k].status == REQ_SUSPICIOUS) i = k;
    }
    if (i < 0) i = pending_find(&app->pending, id);
    if (i < 0) {
        printf("ERROR: No request %s in the pending pool\n", id);
        return -1;
    }
    if (app->pending.items[i].status != REQ_SUSPICIOUS) {
        printf("ERROR: Request %s is not SUSPICIOUS (status %s)\n", id,
               request_status_name(app->pending.items[i].status));
        return -1;
    }
    return i;
}

void cmd_approve_suspicious(App *app, int argc, char **argv) {
    int i = find_suspicious(app, argc, argv);
    if (i < 0) return;
    PendingRequest *p = &app->pending.items[i];
    if (p->is_duplicate) {
        printf("ERROR: %s duplicates an existing request_id; a replay can only be rejected.\n",
               p->rec.record_id);
        return;
    }
    p->status = REQ_PENDING;
    p->approved = 1;
    printf("APPROVED: %s by librarian %s. It is PENDING again and will be mined in priority order.\n",
           p->rec.record_id, app->session.username);
    app_save_pending(app);
}

void cmd_reject_suspicious(App *app, int argc, char **argv) {
    int i = find_suspicious(app, argc, argv);
    if (i < 0) return;
    char id[RECORD_ID_MAX];
    snprintf(id, sizeof(id), "%s", app->pending.items[i].rec.record_id);
    app_drop_request(app, i, "rejected in fraud review");
    printf("REJECTED: %s discarded by librarian %s; it will never be mined.\n", id,
           app->session.username);
    app_revalidate_pool(app);
    app_save_pending(app);
}

/* Re-submits an existing record byte-for-byte (genuine signature, old
 * request_id and nonce), as an attacker replaying a captured request would. */
void cmd_replay_request(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    char id[RECORD_ID_MAX + 8];
    if (!cli_id_arg(argc, argv, 1, "Request ID to replay: ", id, sizeof(id))) return;

    const LendingRecord *found = NULL;
    for (int b = 0; b < app->chain.count && !found; b++) {
        for (int r = 0; r < app->chain.blocks[b].record_count; r++) {
            if (strcmp(app->chain.blocks[b].records[r].record_id, id) == 0) {
                found = &app->chain.blocks[b].records[r];
            }
        }
    }
    int in_pool = pending_find(&app->pending, id);
    if (!found && in_pool >= 0) found = &app->pending.items[in_pool].rec;
    if (!found) {
        printf("ERROR: No request %s on the chain or in the pool\n", id);
        return;
    }

    PendingRequest req;
    memset(&req, 0, sizeof(req));
    req.rec = *found;
    req.priority = PRIORITY_NORMAL;
    req.waiting_since = app_now(app);
    req.seq = app->meta.next_request_seq++;
    req.is_duplicate = 1;
    req.status = REQ_SUSPICIOUS;
    snprintf(req.flag_reason, sizeof(req.flag_reason),
             "duplicate request: request_id %s already exists %s (nonce %u of %s already used)",
             id, in_pool >= 0 ? "in the pending pool" : "on the chain", found->nonce, found->signer);

    printf("Replaying %s exactly as captured: the ECDSA signature is %s (a replay carries a genuine "
           "signature),\nso only the duplicate request_id and the reused nonce reveal it.\n", id,
           record_signature_valid(&req.rec) ? "VALID" : "INVALID");
    if (!pending_push(&app->pending, &req)) return;
    app_save_pending(app);
    printf("FLAGGED: %s\n  The replay is held as SUSPICIOUS for fraud_review and will never be mined.\n",
           req.flag_reason);
}
