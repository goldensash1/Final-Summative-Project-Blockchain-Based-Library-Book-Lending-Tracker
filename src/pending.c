#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pending.h"

#define PENDING_INITIAL_CAPACITY 8
#define MAX_PENDING 100000

static const char PENDING_MAGIC[4] = {'L', 'B', 'P', '2'};

int pending_init(PendingPool *pool) {
    pool->items = malloc(sizeof(PendingEntry) * PENDING_INITIAL_CAPACITY);
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

int pending_push(PendingPool *pool, const PendingEntry *entry) {
    if (pool->count == pool->capacity) {
        int new_capacity = pool->capacity > 0 ? pool->capacity * 2 : PENDING_INITIAL_CAPACITY;
        PendingEntry *grown = realloc(pool->items, sizeof(PendingEntry) * (size_t)new_capacity);
        if (!grown) {
            printf("ERROR: Out of memory while growing the pending pool\n");
            return 0;
        }
        pool->items = grown;
        pool->capacity = new_capacity;
    }
    pool->items[pool->count++] = *entry;
    return 1;
}

void pending_remove(PendingPool *pool, int i) {
    if (i < 0 || i >= pool->count) return;
    memmove(&pool->items[i], &pool->items[i + 1],
            sizeof(PendingEntry) * (size_t)(pool->count - i - 1));
    pool->count--;
}

int pending_save(const PendingPool *pool, const char *path) {
    return storage_save(path, PENDING_MAGIC, pool->items, sizeof(PendingEntry), pool->count);
}

StorageResult pending_load(PendingPool *pool, const char *path) {
    void *records = NULL;
    int count = 0;
    StorageResult rc = storage_load(path, PENDING_MAGIC, sizeof(PendingEntry), MAX_PENDING,
                                    &records, &count);
    if (rc != STORAGE_OK) return rc;

    free(pool->items);
    pool->items = records;
    pool->count = count;
    pool->capacity = count > 0 ? count : 1;

    for (int i = 0; i < count; i++) {
        block_sanitize(&pool->items[i].block);
        tx_sanitize(&pool->items[i].tx);
    }
    return STORAGE_OK;
}

void pending_print(const PendingPool *pool, int difficulty) {
    printf("\nPending pool: %d unconfirmed block(s) waiting to be mined\n", pool->count);
    if (pool->count == 0) {
        printf("  (empty: borrow, return, mark overdue or transfer tokens to add blocks)\n");
        return;
    }

    printf("  %-3s %-7s %-22s %-8s %-16s %-9s %-6s %-9s %-10s %s\n", "#", "Book", "Book Title",
           "Member", "Member Name", "Action", "Reward", "Tx ID", "Signed By", "Recorded At");
    printf("  ");
    for (int i = 0; i < 122; i++) putchar('-');
    putchar('\n');

    for (int i = 0; i < pool->count; i++) {
        const Block *b = &pool->items[i].block;
        char ts[32];
        struct tm *tm_info = localtime(&b->timestamp);
        if (!tm_info || strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info) == 0) {
            snprintf(ts, sizeof(ts), "%ld", (long)b->timestamp);
        }
        printf("  %-3d %-7.7s %-22.22s %-8.8s %-16.16s %-9.9s %-6d %-9.8s %-10.10s %s\n",
               i + 1, b->book_id, b->book_title, b->member_id, b->member_name, b->action,
               b->token_reward, pool->items[i].has_tx ? b->tx_id : "-", b->recorded_by, ts);
    }
    printf("  Mining at difficulty %d: each block needs a hash starting with %d zero(s), "
           "about %.0f attempts on average.\n", difficulty, difficulty,
           pow_expected_attempts(difficulty));
}
