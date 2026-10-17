#ifndef PENDING_H
#define PENDING_H

#include "record.h"
#include "transaction.h"
#include "storage.h"

typedef enum {
    REQ_PENDING = 0,
    REQ_CONFIRMED,     /* included in a mined block (then removed from the pool) */
    REQ_SUSPICIOUS     /* flagged by a fraud heuristic: never mined until approved */
} RequestStatus;

/* Priority classes: lower is served first. */
#define PRIORITY_OVERDUE 0   /* returns / fine settlements on already-overdue copies, overdue flags */
#define PRIORITY_NORMAL 1

/* A signed lending request waiting to be mined. request_id, book_id,
 * copy_id, member_id and action_requested are the fields of `rec`
 * (record_id, book_id, copy_id, member_id, action). */
typedef struct {
    LendingRecord rec;
    int status;               /* RequestStatus */
    int priority;             /* PRIORITY_* */
    time_t waiting_since;     /* reservation date (or submission time): tie-break, oldest first */
    long seq;                 /* submission order */
    int approved;             /* a librarian approved it in fraud review */
    int is_duplicate;         /* replay of an existing request_id (can only be rejected) */
    char flag_reason[160];    /* why it was flagged SUSPICIOUS */
    int has_tx;               /* TRANSFER / FINE_PAID carry a token transaction */
    Transaction tx;
} PendingRequest;

/* The pool keeps submission order in memory and on disk; the priority order
 * is computed whenever it is needed (pool_view, mining). */
typedef struct {
    PendingRequest *items;
    int count;
    int capacity;
} PendingPool;

const char *request_status_name(int status);

int pending_init(PendingPool *pool);
void pending_free(PendingPool *pool);
int pending_push(PendingPool *pool, const PendingRequest *req);
void pending_remove(PendingPool *pool, int i);

/* Index of the request with this ID, or -1. */
int pending_find(const PendingPool *pool, const char *request_id);

/* Same, ignoring replayed duplicates (which share the original's ID). */
int pending_find_genuine(const PendingPool *pool, const char *request_id);

/* Fills order[] with the pool's indices sorted by priority: overdue-related
 * requests first, then oldest reservation/waiting date, then submission
 * order. Returns the count. */
int pending_priority_order(const PendingPool *pool, int *order);

int pending_save(const PendingPool *pool, const char *path);
StorageResult pending_load(PendingPool *pool, const char *path);

/* Prints the pool in priority order (both PENDING and SUSPICIOUS). */
void pending_print(const PendingPool *pool, int batch_size, int difficulty);

#endif
