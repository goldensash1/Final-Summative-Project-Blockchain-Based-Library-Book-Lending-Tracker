#ifndef MERKLE_H
#define MERKLE_H

#include "record.h"

/* Merkle tree over the lending records of a block, built from scratch.
 *
 *   leaf_i  = SHA-256(record_i including its signature)          (record_hash)
 *   parent  = SHA-256(left_child_bytes || right_child_bytes)      (raw 32+32 bytes)
 *
 * When a level has an odd number of nodes the last one is paired with
 * itself (as in Bitcoin). The root of a block with no records is ZERO_HASH.
 * Changing any byte of any record changes its leaf and therefore the root. */

/* Computes the Merkle root of `count` records into out (hex). Returns 1 on
 * success, 0 if hashing failed (out is then empty). */
int merkle_root(const LendingRecord *records, int count, char out[SHA256_HEX_LEN + 1]);

/* Prints every level of the tree, leaves first, for chain_view. */
void merkle_print_tree(const LendingRecord *records, int count);

#endif
