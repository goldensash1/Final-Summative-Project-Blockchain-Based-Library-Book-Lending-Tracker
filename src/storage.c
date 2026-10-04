#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "storage.h"

int storage_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

int storage_save(const char *path, const char magic[4],
                 const void *records, size_t record_size, int count) {
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        printf("ERROR: Could not open '%s' for writing\n", tmp_path);
        return 0;
    }

    uint32_t size32 = (uint32_t)record_size;
    int32_t count32 = (int32_t)count;
    int ok = fwrite(magic, 1, 4, fp) == 4 &&
             fwrite(&size32, sizeof(size32), 1, fp) == 1 &&
             fwrite(&count32, sizeof(count32), 1, fp) == 1 &&
             (count == 0 ||
              fwrite(records, record_size, (size_t)count, fp) == (size_t)count);
    if (fclose(fp) != 0) ok = 0;

    if (!ok || rename(tmp_path, path) != 0) {
        printf("ERROR: Failed to save '%s'\n", path);
        remove(tmp_path);
        return 0;
    }
    return 1;
}

StorageResult storage_load(const char *path, const char magic[4], size_t record_size,
                           int max_count, void **records_out, int *count_out) {
    *records_out = NULL;
    *count_out = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return STORAGE_MISSING;

    char file_magic[4];
    if (fread(file_magic, 1, 4, fp) != 4 || memcmp(file_magic, magic, 4) != 0) {
        fclose(fp);
        printf("ERROR: '%s' is not in the expected format (it may have been created by an "
               "older version of the program). Run 'make clean' to start a fresh chain.\n", path);
        return STORAGE_ERROR;
    }

    uint32_t size32 = 0;
    int32_t count32 = 0;
    if (fread(&size32, sizeof(size32), 1, fp) != 1 ||
        fread(&count32, sizeof(count32), 1, fp) != 1 ||
        size32 != (uint32_t)record_size || count32 < 0 || count32 > max_count) {
        fclose(fp);
        printf("ERROR: '%s' has an invalid header (corrupted, or created by a different "
               "build). Run 'make clean' to start a fresh chain.\n", path);
        return STORAGE_ERROR;
    }

    void *records = malloc(record_size * (size_t)(count32 > 0 ? count32 : 1));
    if (!records) {
        fclose(fp);
        printf("ERROR: Out of memory while loading '%s'\n", path);
        return STORAGE_ERROR;
    }

    if (count32 > 0 && fread(records, record_size, (size_t)count32, fp) != (size_t)count32) {
        free(records);
        fclose(fp);
        printf("ERROR: '%s' appears truncated or corrupted\n", path);
        return STORAGE_ERROR;
    }
    fclose(fp);

    *records_out = records;
    *count_out = count32;
    return STORAGE_OK;
}
