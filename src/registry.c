#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "registry.h"

/* Splits a CSV line "a,b,c" (no quoted fields, matches the simple
 * books.txt / members.txt format described in the assignment). */
static int split_csv(char *line, char *fields[], int max_fields) {
    int count = 0;
    char *tok = strtok(line, ",\r\n");
    while (tok != NULL && count < max_fields) {
        fields[count++] = tok;
        tok = strtok(NULL, ",\r\n");
    }
    return count;
}

static int is_blank_line(const char *line) {
    for (const char *p = line; *p; p++) {
        if (!isspace((unsigned char)*p)) return 0;
    }
    return 1;
}

/* Trims leading/trailing whitespace in place and returns the new start. */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* IDs are matched case-insensitively: store them upper-cased, the same way
 * user input is normalised in main.c. */
static void to_upper(char *s) {
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static void copy_field(char *dst, size_t dst_size, const char *src) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* Reads `path` line by line, calling `add` with 3 trimmed fields for every
 * well-formed line. Returns the number of records added, or -1 if the file
 * could not be opened. */
static int load_csv3(const char *path, const char *what,
                     int (*add)(void *ctx, char *f[3], int line_no), void *ctx) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("ERROR: Could not open %s registry file '%s'\n", what, path);
        return -1;
    }

    char line[256];
    int line_no = 0, added = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        if (is_blank_line(line)) continue;

        char line_copy[256];
        copy_field(line_copy, sizeof(line_copy), line);

        char *fields[3];
        if (split_csv(line_copy, fields, 3) < 3) {
            printf("WARNING: Skipping malformed line %d in %s\n", line_no, path);
            continue;
        }
        for (int i = 0; i < 3; i++) fields[i] = trim(fields[i]);
        to_upper(fields[0]);

        if (fields[0][0] == '\0') {
            printf("WARNING: Skipping line %d in %s (empty ID)\n", line_no, path);
            continue;
        }
        added += add(ctx, fields, line_no);
    }
    fclose(fp);
    return added;
}

static int add_book(void *ctx, char *f[3], int line_no) {
    Registry *reg = ctx;
    if (reg->book_count >= MAX_BOOKS) {
        printf("WARNING: Book registry full (max %d), ignoring line %d\n", MAX_BOOKS, line_no);
        return 0;
    }
    if (!valid_entity_id(f[0]) || !valid_text_field(f[1], sizeof(reg->books[0].title)) ||
        !valid_text_field(f[2], sizeof(reg->books[0].author))) {
        printf("WARNING: Invalid book on line %d skipped (ID must be 3-15 letters/digits starting "
               "with a letter; title max 79, author max 49 characters)\n", line_no);
        return 0;
    }
    if (registry_find_book(reg, f[0])) {
        printf("WARNING: Duplicate book ID '%s' on line %d ignored\n", f[0], line_no);
        return 0;
    }
    Book *b = &reg->books[reg->book_count++];
    copy_field(b->book_id, sizeof(b->book_id), f[0]);
    copy_field(b->title, sizeof(b->title), f[1]);
    copy_field(b->author, sizeof(b->author), f[2]);
    return 1;
}

static int add_member(void *ctx, char *f[3], int line_no) {
    Registry *reg = ctx;
    if (reg->member_count >= MAX_MEMBERS) {
        printf("WARNING: Member registry full (max %d), ignoring line %d\n", MAX_MEMBERS, line_no);
        return 0;
    }
    if (!valid_entity_id(f[0]) || !valid_text_field(f[1], sizeof(reg->members[0].full_name)) ||
        !valid_text_field(f[2], sizeof(reg->members[0].course_code))) {
        printf("WARNING: Invalid member on line %d skipped (ID must be 3-15 letters/digits starting "
               "with a letter; name max 49, course code max 9 characters)\n", line_no);
        return 0;
    }
    if (registry_find_member(reg, f[0])) {
        printf("WARNING: Duplicate member ID '%s' on line %d ignored\n", f[0], line_no);
        return 0;
    }
    Member *m = &reg->members[reg->member_count++];
    copy_field(m->member_id, sizeof(m->member_id), f[0]);
    copy_field(m->full_name, sizeof(m->full_name), f[1]);
    copy_field(m->course_code, sizeof(m->course_code), f[2]);
    return 1;
}

int registry_load(Registry *reg, const char *books_path, const char *members_path) {
    reg->book_count = 0;
    reg->member_count = 0;

    int books = load_csv3(books_path, "book", add_book, reg);
    if (books == 0) printf("ERROR: Book registry '%s' is empty\n", books_path);

    int members = load_csv3(members_path, "member", add_member, reg);
    if (members == 0) printf("ERROR: Member registry '%s' is empty\n", members_path);

    return books > 0 && members > 0;
}

Book *registry_find_book(Registry *reg, const char *book_id) {
    for (int i = 0; i < reg->book_count; i++) {
        if (strcmp(reg->books[i].book_id, book_id) == 0) return &reg->books[i];
    }
    return NULL;
}

Member *registry_find_member(Registry *reg, const char *member_id) {
    for (int i = 0; i < reg->member_count; i++) {
        if (strcmp(reg->members[i].member_id, member_id) == 0) return &reg->members[i];
    }
    return NULL;
}

/* ======================= Registration ======================= */

int valid_entity_id(const char *id) {
    size_t len = strlen(id);
    if (len < 3 || len > 15 || !isalpha((unsigned char)id[0])) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)id[i])) return 0;
    }
    return 1;
}

int valid_text_field(const char *s, size_t max) {
    size_t len = strlen(s);
    if (len == 0 || len >= max) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isprint((unsigned char)s[i]) || s[i] == ',') return 0;
    }
    return 1;
}

static int append_line(const char *path, const char *a, const char *b, const char *c) {
    FILE *fp = fopen(path, "a");
    if (!fp) return 0;
    int ok = fprintf(fp, "%s,%s,%s\n", a, b, c) > 0;
    return fclose(fp) == 0 && ok;
}

int registry_add_member(Registry *reg, const char *path, const char *id, const char *name,
                        const char *course, char *err, size_t err_size) {
    if (!valid_entity_id(id)) {
        snprintf(err, err_size, "invalid member ID '%s' (3-15 letters/digits, starting with a letter)", id);
        return 0;
    }
    if (registry_find_member(reg, id)) {
        snprintf(err, err_size, "member ID %s is already registered", id);
        return 0;
    }
    if (!valid_text_field(name, sizeof(reg->members[0].full_name)) ||
        !valid_text_field(course, sizeof(reg->members[0].course_code))) {
        snprintf(err, err_size, "name (max 49) and course code (max 9) must be non-empty, without commas");
        return 0;
    }
    if (reg->member_count >= MAX_MEMBERS) {
        snprintf(err, err_size, "member registry is full");
        return 0;
    }
    if (!append_line(path, id, name, course)) {
        snprintf(err, err_size, "could not write %s", path);
        return 0;
    }
    Member *m = &reg->members[reg->member_count++];
    memset(m, 0, sizeof(*m));
    copy_field(m->member_id, sizeof(m->member_id), id);
    copy_field(m->full_name, sizeof(m->full_name), name);
    copy_field(m->course_code, sizeof(m->course_code), course);
    return 1;
}

int registry_add_book(Registry *reg, const char *path, const char *id, const char *title,
                      const char *author, char *err, size_t err_size) {
    if (!valid_entity_id(id)) {
        snprintf(err, err_size, "invalid book ID '%s' (3-15 letters/digits, starting with a letter)", id);
        return 0;
    }
    if (registry_find_book(reg, id)) {
        snprintf(err, err_size, "book ID %s is already registered", id);
        return 0;
    }
    if (!valid_text_field(title, sizeof(reg->books[0].title)) ||
        !valid_text_field(author, sizeof(reg->books[0].author))) {
        snprintf(err, err_size, "title (max 79) and author (max 49) must be non-empty, without commas");
        return 0;
    }
    if (reg->book_count >= MAX_BOOKS) {
        snprintf(err, err_size, "book registry is full");
        return 0;
    }
    if (!append_line(path, id, title, author)) {
        snprintf(err, err_size, "could not write %s", path);
        return 0;
    }
    Book *b = &reg->books[reg->book_count++];
    memset(b, 0, sizeof(*b));
    copy_field(b->book_id, sizeof(b->book_id), id);
    copy_field(b->title, sizeof(b->title), title);
    copy_field(b->author, sizeof(b->author), author);
    return 1;
}
