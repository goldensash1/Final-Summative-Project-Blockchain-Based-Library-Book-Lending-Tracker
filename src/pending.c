#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pending.h"
#include "blockchain.h"

#define PENDING_INITIAL_CAPACITY 8
#define MAX_PENDING 100000

static const char PENDING_MAGIC[4] = {'L', 'B', 'P', '3'};

const char *request_status_name(int status) {
    switch (status) {
        case REQ_PENDING: return "PENDING";
        case REQ_CONFIRMED: return "CONFIRMED";
        case REQ_SUSPICIOUS: return "SUSPICIOUS";
    }
    return "?";
}

int pending_init(PendingPool *pool) {
    pool->items = malloc(sizeof(PendingRequest) * PENDING_INITIAL_CAPACITY);
    pool->count = 0;
    pool->capacity = pool->items ? PENDING_INITIAL_CAPACITY : 0;
    return pool->items != NULL;
}

void pending_free(PendingPool *pool) {
    free(pool->items);
    pool->items = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

int pending_push(PendingPool *pool, const PendingRequest *req) {
    if (pool->count == pool->capacity) {
        int new_capacity = pool->capacity > 0 ? pool->capacity * 2 : PENDING_INITIAL_CAPACITY;
        PendingRequest *grown = realloc(pool->items, sizeof(PendingRequest) * (size_t)new_capacity);
        if (!grown) {
            printf("ERROR: Out of memory while growing the pending pool\n");
            return 0;
        }
        pool->items = grown;
        pool->capacity = new_capacity;
    }
    pool->items[pool->count++] = *req;
    return 1;
}

void pending_remove(PendingPool *pool, int i) {
    if (i < 0 || i >= pool->count) return;
    memmove(&pool->items[i], &pool->items[i + 1],
            sizeof(PendingRequest) * (size_t)(pool->count - i - 1));
    pool->count--;
}

int pending_find(const PendingPool *pool, const char *request_id) {
    for (int i = 0; i < pool->count; i++) {
        if (strcmp(pool->items[i].rec.record_id, request_id) == 0) return i;
    }
    return -1;
}

int pending_find_genuine(const PendingPool *pool, const char *request_id) {
    for (int i = 0; i < pool->count; i++) {
        if (!pool->items[i].is_duplicate && strcmp(pool->items[i].rec.record_id, request_id) == 0) return i;
    }
    return -1;
}

/* qsort has no context argument, so the pool being sorted is passed here. */
static const PendingPool *g_sort_pool;

static int compare_priority(const void *a, const void *b) {
    const PendingRequest *x = &g_sort_pool->items[*(const int *)a];
    const PendingRequest *y = &g_sort_pool->items[*(const int *)b];
    if (x->priority != y->priority) return x->priority - y->priority;
    if (x->waiting_since != y->waiting_since) return x->waiting_since < y->waiting_since ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

int pending_priority_order(const PendingPool *pool, int *order) {
    for (int i = 0; i < pool->count; i++) order[i] = i;
    g_sort_pool = pool;
    qsort(order, (size_t)pool->count, sizeof(int), compare_priority);
    return pool->count;
}

int pending_save(const PendingPool *pool, const char *path) {
    return storage_save(path, PENDING_MAGIC, pool->items, sizeof(PendingRequest), pool->count);
}

StorageResult pending_load(PendingPool *pool, const char *path) {
    void *records = NULL;
    int count = 0;
    StorageResult rc = storage_load(path, PENDING_MAGIC, sizeof(PendingRequest), MAX_PENDING,
                                    &records, &count);
    if (rc != STORAGE_OK) return rc;

    free(pool->items);
    pool->items = records;
    pool->count = count;
    pool->capacity = count > 0 ? count : 1;
    for (int i = 0; i < count; i++) {
        record_sanitize(&pool->items[i].rec);
        tx_sanitize(&pool->items[i].tx);
        pool->items[i].flag_reason[sizeof(pool->items[i].flag_reason) - 1] = '\0';
    }
    return STORAGE_OK;
}

void pending_print(const PendingPool *pool, int batch_size, int difficulty) {
    int suspicious = 0;
    for (int i = 0; i < pool->count; i++) suspicious += pool->items[i].status == REQ_SUSPICIOUS;

    printf("\nPending pool: %d request(s) (%d PENDING, %d SUSPICIOUS), in mining priority order\n",
           pool->count, pool->count - suspicious, suspicious);
    if (pool->count == 0) {
        printf("  (empty)\n");
        return;
    }

    printf("  %-3s %-10s %-9s %-9s %-7s %-16s %-9s %-16s %5s %-10s\n", "#", "Request", "Action",
           "Copy", "Member", "Title", "Priority", "Waiting since", "Nonce", "Status");
    printf("  ");
    for (int i = 0; i < 112; i++) putchar('-');
    putchar('\n');

    int *order = malloc(sizeof(int) * (size_t)pool->count);
    if (!order) return;
    pending_priority_order(pool, order);
    int rank = 0;
    for (int k = 0; k < pool->count; k++) {
        const PendingRequest *p = &pool->items[order[k]];
        char since[24];
        format_datetime(p->waiting_since, since, sizeof(since));
        printf("  %-3d %-10s %-9s %-9.9s %-7.7s %-16.16s %-9s %-16s %5u %-10s\n", ++rank,
               p->rec.record_id, record_action_name(p->rec.action), p->rec.copy_id,
               p->rec.member_id, p->rec.book_title,
               p->priority == PRIORITY_OVERDUE ? "OVERDUE" : "normal", since, p->rec.nonce,
               request_status_name(p->status));
        if (p->status == REQ_SUSPICIOUS) printf("        flagged: %s\n", p->flag_reason);
        else if (p->approved) printf("        approved in fraud review (%s)\n", p->flag_reason);
        if (p->has_tx) {
            printf("        carries tx %.16s...: %s -> %s, %s coins + %s fee\n", p->tx.txid,
                   p->tx.body.sender, p->tx.body.recipient, fmt_amount(p->tx.body.amount),
                   fmt_amount(p->tx.body.fee));
        }
    }
    free(order);
    printf("  Mining takes the top %d PENDING request(s) per block (SUSPICIOUS ones are withheld) at "
           "difficulty %d (~%.0f attempts).\n", batch_size, difficulty, pow_expected_attempts(difficulty));
}
