#ifndef REGISTRY_H
#define REGISTRY_H

#include <stddef.h>

#define MAX_BOOKS 200
#define MAX_MEMBERS 200

typedef struct {
    char book_id[20];
    char title[80];
    char author[50];
} Book;

typedef struct {
    char member_id[20];
    char full_name[50];
    char course_code[10];
} Member;

typedef struct {
    Book books[MAX_BOOKS];
    int book_count;
    Member members[MAX_MEMBERS];
    int member_count;
} Registry;

/* Loads books.txt and members.txt into the registry.
 * Returns 1 on success, 0 if either file is missing or empty. */
int registry_load(Registry *reg, const char *books_path, const char *members_path);

/* Returns pointer to the Book with the given id, or NULL if not found. */
Book *registry_find_book(Registry *reg, const char *book_id);

/* Returns pointer to the Member with the given id, or NULL if not found. */
Member *registry_find_member(Registry *reg, const char *member_id);

/* ID format: 3-15 characters, a letter first, then letters and digits only
 * (e.g. ALU001, BK001). IDs are stored upper-case. */
int valid_entity_id(const char *id);

/* A name/title/author/course field: 1..max-1 printable characters, no comma
 * (the registry files are comma-separated). */
int valid_text_field(const char *s, size_t max);

/* Registers a new member or book: validates the fields, checks the ID is
 * unused, adds it to the registry and appends it to the registry file (so it
 * is loaded again on the next start). Returns 1, or 0 with a reason in err. */
int registry_add_member(Registry *reg, const char *path, const char *id, const char *name,
                        const char *course, char *err, size_t err_size);
int registry_add_book(Registry *reg, const char *path, const char *id, const char *title,
                      const char *author, char *err, size_t err_size);

#endif
