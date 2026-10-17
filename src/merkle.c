#include <stdio.h>
#include <string.h>

#include "merkle.h"

#define MERKLE_MAX_LEAVES 64

/* Leaf digests (raw bytes) of the records. Returns the count, or -1. */
static int merkle_leaves(const LendingRecord *records, int count,
                         unsigned char level[][SHA256_DIGEST_LEN]) {
    if (count < 0 || count > MERKLE_MAX_LEAVES) return -1;
    for (int i = 0; i < count; i++) {
        char hex[SHA256_HEX_LEN + 1];
        record_hash(&records[i], hex);
        if (hex[0] == '\0') return -1;
        for (int b = 0; b < SHA256_DIGEST_LEN; b++) {
            unsigned int byte;
            if (sscanf(hex + 2 * b, "%2x", &byte) != 1) return -1;
            level[i][b] = (unsigned char)byte;
        }
    }
    return count;
}

/* Replaces `level` (n nodes) with its parent level. Returns the new size. */
static int merkle_reduce(unsigned char level[][SHA256_DIGEST_LEN], int n) {
    int parents = 0;
    for (int i = 0; i < n; i += 2) {
        unsigned char pair[2 * SHA256_DIGEST_LEN];
        memcpy(pair, level[i], SHA256_DIGEST_LEN);
        /* Odd node out: pair it with itself. */
        memcpy(pair + SHA256_DIGEST_LEN, level[i + 1 < n ? i + 1 : i], SHA256_DIGEST_LEN);
        if (!sha256_raw(pair, sizeof(pair), level[parents])) return -1;
        parents++;
    }
    return parents;
}

int merkle_root(const LendingRecord *records, int count, char out[SHA256_HEX_LEN + 1]) {
    if (count == 0) {
        snprintf(out, SHA256_HEX_LEN + 1, "%s", ZERO_HASH);
        return 1;
    }
    unsigned char level[MERKLE_MAX_LEAVES][SHA256_DIGEST_LEN];
    int n = merkle_leaves(records, count, level);
    while (n > 1) n = merkle_reduce(level, n);
    if (n != 1) {
        out[0] = '\0';
        return 0;
    }
    bytes_to_hex(level[0], SHA256_DIGEST_LEN, out);
    return 1;
}

void merkle_print_tree(const LendingRecord *records, int count) {
    if (count == 0) {
        printf("  Merkle tree: no records (root = 64 zeros)\n");
        return;
    }
    unsigned char level[MERKLE_MAX_LEAVES][SHA256_DIGEST_LEN];
    int n = merkle_leaves(records, count, level);
    if (n < 0) {
        printf("  Merkle tree: could not hash the records\n");
        return;
    }
    printf("  Merkle tree (leaf = SHA-256 of a signed record; parent = SHA-256(left||right)):\n");
    int depth = 0;
    for (;;) {
        printf("    Level %d%s:", depth, depth == 0 ? " (leaves)" : n == 1 ? " (root)" : "");
        for (int i = 0; i < n; i++) {
            char hex[SHA256_HEX_LEN + 1];
            bytes_to_hex(level[i], SHA256_DIGEST_LEN, hex);
            printf(" %.12s", hex);
        }
        if (depth == 0) {
            printf("   <- ");
            for (int i = 0; i < count; i++) printf("%s%s", i ? ", " : "", records[i].record_id);
        }
        printf("\n");
        if (n == 1) break;
        n = merkle_reduce(level, n);
        if (n < 0) return;
        depth++;
    }
}
