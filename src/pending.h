#ifndef PENDING_H
#define PENDING_H

#include "blockchain.h"
#include "transaction.h"
#include "storage.h"

/* An unconfirmed lending block waiting to be mined. The librarian has
 * already signed its content; the miner will later set its index,
 * previous_hash, difficulty and nonce. A RETURNED block carries the reward
 * transaction it created (a TRANSFER block carries the transfer). */
typedef struct {
    Block block;
    int has_tx;
    Transaction tx;
} PendingEntry;

/* FIFO queue of pending blocks (persisted to pending.dat). */
typedef struct {
    PendingEntry *items;
    int count;
    int capacity;
} PendingPool;

int pending_init(PendingPool *pool);
void pending_free(PendingPool *pool);

/* Appends an entry at the back. Returns 1 on success. */
int pending_push(PendingPool *pool, const PendingEntry *entry);

/* Removes entry i, keeping the order of the rest. */
void pending_remove(PendingPool *pool, int i);

int pending_save(const PendingPool *pool, const char *path);

/* Loads the pool. Returns STORAGE_MISSING if there is no file yet. */
StorageResult pending_load(PendingPool *pool, const char *path);

/* Prints every block waiting to be mined. */
void pending_print(const PendingPool *pool, int difficulty);

#endif
