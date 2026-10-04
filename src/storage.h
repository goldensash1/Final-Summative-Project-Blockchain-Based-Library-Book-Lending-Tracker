#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>

typedef enum {
    STORAGE_OK = 0,
    STORAGE_MISSING,   /* the file does not exist yet */
    STORAGE_ERROR      /* unreadable, wrong format or truncated (message printed) */
} StorageResult;

/* Returns 1 if `path` exists. */
int storage_exists(const char *path);

/* Saves `count` fixed-size records to `path`.
 * File layout: 4-byte magic | uint32 record size | int32 record count | records.
 * The data is written to a temp file, then renamed, so a crash can never
 * leave a half-written file. Returns 1 on success. */
int storage_save(const char *path, const char magic[4],
                 const void *records, size_t record_size, int count);

/* Loads records written by storage_save. On STORAGE_OK, *records_out is a
 * malloc'd array (the caller frees it) holding *count_out records. The magic
 * and record size are checked, so files from another version are refused. */
StorageResult storage_load(const char *path, const char magic[4], size_t record_size,
                           int max_count, void **records_out, int *count_out);

#endif
