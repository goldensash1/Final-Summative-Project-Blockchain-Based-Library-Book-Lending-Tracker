#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "state.h"

static const char STATE_MAGIC[4] = {'L', 'B', 'S', '1'};
static const char *COPY_STATE_NAMES[] = {"AVAILABLE", "BORROWED", "RESERVED", "LOST"};

static void copy_field(char *dst, size_t dst_size, const char *src) {
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

const char *copy_state_name(int state) {
    return state >= COPY_AVAILABLE && state <= COPY_LOST ? COPY_STATE_NAMES[state] : "?";
}

/* ======================= Copies ======================= */

int valid_copy_id_format(const char *id) {
    const char *dash = strstr(id, "-C");
    if (!dash || dash == id || strlen(id) >= COPY_ID_MAX) return 0;
    for (const char *p = id; p < dash; p++) {
        if (!isalnum((unsigned char)*p)) return 0;
    }
    const char *digits = dash + 2;
    if (*digits == '\0') return 0;
    for (const char *p = digits; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
    }
    return 1;
}

/* The book part of a copy ID must name the copy's own title. */
static int copy_id_matches_book(const char *copy_id, const char *book_id) {
    size_t n = strlen(book_id);
    return strncmp(copy_id, book_id, n) == 0 && copy_id[n] == '-' && copy_id[n + 1] == 'C';
}

static int add_copy_entry(LibraryState *st, const char *copy_id, const char *book_id, int condition) {
    if (st->copy_count >= MAX_COPIES) return 0;
    Copy *c = &st->copies[st->copy_count++];
    memset(c, 0, sizeof(*c));
    copy_field(c->copy_id, sizeof(c->copy_id), copy_id);
    copy_field(c->book_id, sizeof(c->book_id), book_id);
    c->condition = condition;
    c->state = condition == COND_LOST ? COPY_LOST : COPY_AVAILABLE;
    return 1;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* Reads copies.txt: copy_id,book_id,condition[,added_at_block]. A copy
 * added while the program runs (add_copy / register_book) records the index
 * of the next block, so replaying the chain only brings it into existence
 * from that block on. Adds the copies whose block is in [min_h, max_h] to st.
 * Returns the number added, or -1 if the file cannot be opened. */
static int read_copies_file(LibraryState *st, const char *path, const Registry *reg,
                            int min_h, int max_h, int warn) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("ERROR: Could not open copy inventory file '%s'\n", path);
        return -1;
    }

    char line[256];
    int line_no = 0, added = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char *fields[4] = {NULL, NULL, NULL, NULL};
        int n = 0;
        for (char *tok = strtok(line, ",\r\n"); tok && n < 4; tok = strtok(NULL, ",\r\n")) {
            fields[n++] = trim(tok);
        }
        if (n == 0 || (n == 1 && fields[0][0] == '\0')) continue;   /* blank line */
        int height = n == 4 ? atoi(fields[3]) : 0;
        if (height < min_h || height > max_h) continue;
        if (n < 2) {
            if (warn) printf("WARNING: Skipping malformed line %d in %s\n", line_no, path);
            continue;
        }
        for (char *p = fields[0]; *p; p++) *p = (char)toupper((unsigned char)*p);
        for (char *p = fields[1]; *p; p++) *p = (char)toupper((unsigned char)*p);
        int condition = n >= 3 ? condition_parse(fields[2]) : COND_GOOD;

        if (!valid_copy_id_format(fields[0]) || !copy_id_matches_book(fields[0], fields[1])) {
            if (warn) printf("WARNING: Line %d in %s: invalid copy ID '%s' (expected %s-C<n>)\n",
                             line_no, path, fields[0], fields[1]);
        } else if (!registry_find_book((Registry *)reg, fields[1])) {
            if (warn) printf("WARNING: Line %d in %s: copy %s refers to unknown book '%s'\n",
                             line_no, path, fields[0], fields[1]);
        } else if (condition == COND_NONE) {
            if (warn) printf("WARNING: Line %d in %s: unknown condition '%s'\n", line_no, path, fields[2]);
        } else if (state_find_copy(st, fields[0])) {
            if (warn) printf("WARNING: Duplicate copy ID '%s' on line %d ignored\n", fields[0], line_no);
        } else if (!add_copy_entry(st, fields[0], fields[1], condition)) {
            if (warn) printf("WARNING: Copy inventory full (max %d)\n", MAX_COPIES);
        } else {
            added++;
        }
    }
    fclose(fp);
    return added;
}

int state_load_copies(LibraryState *st, const char *path, const Registry *reg, int max_height) {
    memset(st, 0, sizeof(*st));
    int added = read_copies_file(st, path, reg, 0, max_height, 1);
    if (added < 0) return 0;
    if (st->copy_count == 0) {
        printf("ERROR: Copy inventory '%s' is empty\n", path);
        return 0;
    }
    return 1;
}

int state_load_copies_added(LibraryState *st, const char *path, const Registry *reg,
                            int min_height, int max_height) {
    return read_copies_file(st, path, reg, min_height, max_height, 0);
}

int state_add_copy(LibraryState *st, const char *path, const char *copy_id,
                   const char *book_id, int condition, int added_at_block) {
    if (state_find_copy(st, copy_id) || !add_copy_entry(st, copy_id, book_id, condition)) return 0;
    FILE *fp = fopen(path, "a");
    if (!fp) return 0;
    int ok = fprintf(fp, "%s,%s,%s,%d\n", copy_id, book_id, condition_name(condition), added_at_block) > 0;
    return fclose(fp) == 0 && ok;
}

void state_next_copy_id(const LibraryState *st, const char *book_id, char *out, size_t size) {
    int max = 0;
    for (int i = 0; i < st->copy_count; i++) {
        const Copy *c = &st->copies[i];
        if (strcmp(c->book_id, book_id) != 0) continue;
        int n = atoi(c->copy_id + strlen(book_id) + 2);
        if (n > max) max = n;
    }
    snprintf(out, size, "%s-C%d", book_id, max + 1);
}

Copy *state_find_copy(LibraryState *st, const char *copy_id) {
    for (int i = 0; i < st->copy_count; i++) {
        if (strcmp(st->copies[i].copy_id, copy_id) == 0) return &st->copies[i];
    }
    return NULL;
}

const Copy *state_find_copy_const(const LibraryState *st, const char *copy_id) {
    return state_find_copy((LibraryState *)st, copy_id);
}

int state_count_copies(const LibraryState *st, const char *book_id, int state) {
    int n = 0;
    for (int i = 0; i < st->copy_count; i++) {
        const Copy *c = &st->copies[i];
        if (strcmp(c->book_id, book_id) == 0 && (state < 0 || c->state == state)) n++;
    }
    return n;
}

const Copy *state_borrowable_copy(const LibraryState *st, const char *book_id, const char *member_id) {
    const Copy *available = NULL;
    for (int i = 0; i < st->copy_count; i++) {
        const Copy *c = &st->copies[i];
        if (strcmp(c->book_id, book_id) != 0) continue;
        if (c->state == COPY_RESERVED && strcmp(c->reserved_for, member_id) == 0) return c;
        if (c->state == COPY_AVAILABLE && !available) available = c;
    }
    return available;
}

/* ======================= Queues, nonces, fines ======================= */

int state_queue_position(const LibraryState *st, const char *book_id, const char *member_id) {
    int pos = 0;
    for (int i = 0; i < st->queue_count; i++) {
        if (strcmp(st->queue[i].book_id, book_id) != 0) continue;
        pos++;
        if (strcmp(st->queue[i].member_id, member_id) == 0) return pos;
    }
    return 0;
}

int state_queue_length(const LibraryState *st, const char *book_id) {
    int n = 0;
    for (int i = 0; i < st->queue_count; i++) n += strcmp(st->queue[i].book_id, book_id) == 0;
    return n;
}

/* Index of the head of the title's queue, or -1. */
static int queue_head(const LibraryState *st, const char *book_id) {
    for (int i = 0; i < st->queue_count; i++) {
        if (strcmp(st->queue[i].book_id, book_id) == 0) return i;
    }
    return -1;
}

int state_held_count(const LibraryState *st, const char *member_id) {
    int n = 0;
    for (int i = 0; i < st->copy_count; i++) {
        const Copy *c = &st->copies[i];
        n += c->state == COPY_BORROWED && strcmp(c->current_holder_id, member_id) == 0;
    }
    return n;
}

unsigned int state_nonce(const LibraryState *st, const char *signer) {
    for (int i = 0; i < st->nonce_count; i++) {
        if (strcmp(st->nonces[i].signer, signer) == 0) return st->nonces[i].nonce;
    }
    return 0;
}

static int bump_nonce(LibraryState *st, const char *signer) {
    for (int i = 0; i < st->nonce_count; i++) {
        if (strcmp(st->nonces[i].signer, signer) == 0) {
            st->nonces[i].nonce++;
            return 1;
        }
    }
    if (st->nonce_count >= MAX_SIGNERS) return 0;
    SignerNonce *n = &st->nonces[st->nonce_count++];
    memset(n, 0, sizeof(*n));
    copy_field(n->signer, sizeof(n->signer), signer);
    n->nonce = 1;
    return 1;
}

static MemberFines *find_fines(const LibraryState *st, const char *member_id) {
    for (int i = 0; i < st->fine_count; i++) {
        if (strcmp(st->fines[i].member_id, member_id) == 0) return (MemberFines *)&st->fines[i];
    }
    return NULL;
}

static MemberFines *ensure_fines(LibraryState *st, const char *member_id) {
    MemberFines *f = find_fines(st, member_id);
    if (f || st->fine_count >= MAX_MEMBERS) return f;
    f = &st->fines[st->fine_count++];
    memset(f, 0, sizeof(*f));
    copy_field(f->member_id, sizeof(f->member_id), member_id);
    return f;
}

int state_fines_owed(const LibraryState *st, const char *member_id) {
    const MemberFines *f = find_fines(st, member_id);
    return f ? f->owed : 0;
}

int state_fines_paid(const LibraryState *st, const char *member_id) {
    const MemberFines *f = find_fines(st, member_id);
    return f ? f->paid : 0;
}

/* ======================= State machine ======================= */

#define FAIL(...) do { snprintf(err, err_size, __VA_ARGS__); return 0; } while (0)

/* Checks the transition; returns 1 if `rec` may be applied. */
static int check_transition(const LibraryState *st, const LendingRecord *rec, const Copy *c,
                            char *err, size_t err_size) {
    if (rec->fine_amount < 0) FAIL("fine_amount must not be negative");
    if (rec->token_reward < 0) FAIL("token_reward must not be negative");

    int needs_copy = rec->action == ACT_BORROWED || rec->action == ACT_RETURNED ||
                     rec->action == ACT_RENEWED || rec->action == ACT_OVERDUE ||
                     rec->action == ACT_LOST;
    if (needs_copy) {
        if (!c) FAIL("copy '%s' is not in the inventory", rec->copy_id);
        if (strcmp(c->book_id, rec->book_id) != 0) FAIL("copy %s is not a copy of %s", c->copy_id, rec->book_id);
    }
    int holder = c && strcmp(c->current_holder_id, rec->member_id) == 0;

    switch (rec->action) {
        case ACT_BORROWED:
            if (c->state == COPY_RESERVED && strcmp(c->reserved_for, rec->member_id) != 0) {
                FAIL("copy %s is RESERVED for %s (first in the reservation queue)",
                     c->copy_id, c->reserved_for);
            }
            if (c->state != COPY_AVAILABLE && c->state != COPY_RESERVED) {
                FAIL("copy %s is %s, not AVAILABLE", c->copy_id, copy_state_name(c->state));
            }
            if (rec->due_date <= rec->timestamp) FAIL("due date must be after the borrow date");
            return 1;

        case ACT_RETURNED:
            if (c->state != COPY_BORROWED) FAIL("copy %s is %s, not BORROWED", c->copy_id, copy_state_name(c->state));
            if (!holder) FAIL("copy %s is held by %s, not %s", c->copy_id, c->current_holder_id, rec->member_id);
            if (rec->condition < COND_NEW || rec->condition > COND_WORN) {
                FAIL("a returned copy must be NEW, GOOD or WORN (use mark_lost for lost copies)");
            }
            if (rec->condition < c->condition) {
                FAIL("condition cannot improve on return (%s -> %s)", condition_name(c->condition),
                     condition_name(rec->condition));
            }
            return 1;

        case ACT_RESERVED: {
            int circulating = 0, available = 0;
            for (int i = 0; i < st->copy_count; i++) {
                const Copy *x = &st->copies[i];
                if (strcmp(x->book_id, rec->book_id) != 0) continue;
                if (x->state != COPY_LOST) circulating++;
                if (x->state == COPY_AVAILABLE) available++;
                if (x->state == COPY_BORROWED && strcmp(x->current_holder_id, rec->member_id) == 0) {
                    FAIL("%s already holds copy %s of this title", rec->member_id, x->copy_id);
                }
                if (x->state == COPY_RESERVED && strcmp(x->reserved_for, rec->member_id) == 0) {
                    FAIL("copy %s is already being held for %s: borrow it", x->copy_id, rec->member_id);
                }
            }
            if (circulating == 0) FAIL("title %s has no copies in circulation", rec->book_id);
            if (available > 0) FAIL("%d copy(ies) of %s are AVAILABLE: borrow instead of reserving", available, rec->book_id);
            if (state_queue_position(st, rec->book_id, rec->member_id)) {
                FAIL("%s is already in the reservation queue for %s", rec->member_id, rec->book_id);
            }
            if (st->queue_count >= MAX_RESERVATIONS) FAIL("reservation table full");
            return 1;
        }

        case ACT_RENEWED:
            if (c->state != COPY_BORROWED || !holder) FAIL("copy %s is not on loan to %s", c->copy_id, rec->member_id);
            if (c->renew_count >= MAX_RENEWALS) FAIL("copy %s was already renewed the maximum %d times", c->copy_id, MAX_RENEWALS);
            if (state_queue_length(st, c->book_id) > 0) {
                FAIL("another member has reserved %s (%d in queue): renewal not allowed",
                     c->book_id, state_queue_length(st, c->book_id));
            }
            if (c->overdue_flagged || rec->timestamp > c->due_date) FAIL("copy %s is overdue: return it instead", c->copy_id);
            if (rec->due_date <= c->due_date) FAIL("the new due date must be later than the current one");
            return 1;

        case ACT_OVERDUE:
            if (c->state != COPY_BORROWED || !holder) FAIL("copy %s is not on loan to %s", c->copy_id, rec->member_id);
            if (c->overdue_flagged) FAIL("copy %s is already flagged OVERDUE", c->copy_id);
            if (c->due_date >= rec->timestamp) FAIL("copy %s is not past its due date", c->copy_id);
            return 1;

        case ACT_LOST:
            if (rec->reason_code[0] == '\0') FAIL("marking a copy LOST requires a reason code");
            if (c->state != COPY_AVAILABLE && c->state != COPY_BORROWED) {
                FAIL("only AVAILABLE or BORROWED copies can be marked LOST (copy %s is %s)",
                     c->copy_id, copy_state_name(c->state));
            }
            if (c->state == COPY_BORROWED && !holder) FAIL("copy %s is held by %s", c->copy_id, c->current_holder_id);
            return 1;

        case ACT_FINE_PAID:
            if (rec->fine_amount <= 0) FAIL("fine payment must be positive");
            if (state_fines_owed(st, rec->member_id) < rec->fine_amount) {
                FAIL("%s owes %d coin(s) in fines, cannot settle %d", rec->member_id,
                     state_fines_owed(st, rec->member_id), rec->fine_amount);
            }
            return 1;

        case ACT_TRANSFER:
            return 1;
    }
    FAIL("unknown action %d", rec->action);
}

int state_apply(LibraryState *st, const LendingRecord *rec, char *err, size_t err_size) {
    /* Replay protection: exactly the signer's next nonce. */
    unsigned int expected = state_nonce(st, rec->signer);
    if (rec->nonce != expected) {
        FAIL("nonce %u is not %s's expected nonce %u (%s)", rec->nonce, rec->signer, expected,
             rec->nonce < expected ? "replayed or already used" : "out of order");
    }

    Copy *c = strcmp(rec->copy_id, "-") != 0 ? state_find_copy(st, rec->copy_id) : NULL;
    if (!check_transition(st, rec, c, err, err_size)) return 0;
    if (rec->fine_amount > 0 && !find_fines(st, rec->member_id) && st->fine_count >= MAX_MEMBERS) {
        FAIL("fine table full");
    }
    int known_signer = 0;
    for (int i = 0; i < st->nonce_count; i++) known_signer |= strcmp(st->nonces[i].signer, rec->signer) == 0;
    if (!known_signer && st->nonce_count >= MAX_SIGNERS) FAIL("nonce table full");

    switch (rec->action) {
        case ACT_BORROWED:
            c->state = COPY_BORROWED;
            copy_field(c->current_holder_id, sizeof(c->current_holder_id), rec->member_id);
            c->due_date = rec->due_date;
            c->borrowed_at = rec->timestamp;
            c->renew_count = 0;
            c->overdue_flagged = 0;
            memset(c->reserved_for, 0, sizeof(c->reserved_for));
            c->reserved_since = 0;
            break;

        case ACT_RETURNED: {
            c->condition = rec->condition;
            memset(c->current_holder_id, 0, sizeof(c->current_holder_id));
            c->due_date = 0;
            c->overdue_flagged = 0;
            c->renew_count = 0;
            /* Offer the copy to the member who has waited longest. */
            int head = queue_head(st, c->book_id);
            if (head >= 0) {
                c->state = COPY_RESERVED;
                copy_field(c->reserved_for, sizeof(c->reserved_for), st->queue[head].member_id);
                c->reserved_since = st->queue[head].reservation_date;
                memmove(&st->queue[head], &st->queue[head + 1],
                        sizeof(Reservation) * (size_t)(st->queue_count - head - 1));
                st->queue_count--;
                memset(&st->queue[st->queue_count], 0, sizeof(Reservation));
            } else {
                c->state = COPY_AVAILABLE;
            }
            if (rec->fine_amount > 0) ensure_fines(st, rec->member_id)->owed += rec->fine_amount;
            break;
        }

        case ACT_RESERVED: {
            Reservation *r = &st->queue[st->queue_count++];
            memset(r, 0, sizeof(*r));
            copy_field(r->book_id, sizeof(r->book_id), rec->book_id);
            copy_field(r->member_id, sizeof(r->member_id), rec->member_id);
            r->reservation_date = rec->timestamp;
            break;
        }

        case ACT_RENEWED:
            c->due_date = rec->due_date;
            c->renew_count++;
            break;

        case ACT_OVERDUE:
            c->overdue_flagged = 1;
            break;

        case ACT_LOST:
            c->state = COPY_LOST;
            c->condition = COND_LOST;
            memset(c->current_holder_id, 0, sizeof(c->current_holder_id));
            c->due_date = 0;
            c->overdue_flagged = 0;
            if (rec->fine_amount > 0) ensure_fines(st, rec->member_id)->owed += rec->fine_amount;
            break;

        case ACT_FINE_PAID: {
            MemberFines *f = ensure_fines(st, rec->member_id);
            f->owed -= rec->fine_amount;
            f->paid += rec->fine_amount;
            break;
        }

        case ACT_TRANSFER:
            break;
    }

    bump_nonce(st, rec->signer);
    return 1;
}

/* ======================= Comparison and persistence ======================= */

int state_compare(const LibraryState *a, const LibraryState *b, int print) {
    int diffs = 0;
    if (a->copy_count != b->copy_count) {
        diffs++;
        if (print) printf("  copy count differs (%d vs %d)\n", a->copy_count, b->copy_count);
    }
    for (int i = 0; i < a->copy_count && i < b->copy_count; i++) {
        const Copy *x = &a->copies[i], *y = &b->copies[i];
        if (strcmp(x->copy_id, y->copy_id) || x->state != y->state || x->condition != y->condition ||
            strcmp(x->current_holder_id, y->current_holder_id) || x->due_date != y->due_date ||
            x->renew_count != y->renew_count || x->overdue_flagged != y->overdue_flagged ||
            strcmp(x->reserved_for, y->reserved_for)) {
            diffs++;
            if (print && diffs <= 5) printf("  copy %s differs (%s vs %s)\n", x->copy_id,
                                            copy_state_name(x->state), copy_state_name(y->state));
        }
    }
    if (a->queue_count != b->queue_count) {
        diffs++;
        if (print) printf("  reservation queue length differs (%d vs %d)\n", a->queue_count, b->queue_count);
    } else {
        for (int i = 0; i < a->queue_count; i++) {
            if (strcmp(a->queue[i].book_id, b->queue[i].book_id) ||
                strcmp(a->queue[i].member_id, b->queue[i].member_id)) {
                diffs++;
                if (print) printf("  reservation queue entry %d differs\n", i + 1);
            }
        }
    }
    for (int i = 0; i < a->nonce_count; i++) {
        if (state_nonce(b, a->nonces[i].signer) != a->nonces[i].nonce) {
            diffs++;
            if (print) printf("  nonce of %s differs\n", a->nonces[i].signer);
        }
    }
    if (a->nonce_count != b->nonce_count) diffs++;
    for (int i = 0; i < a->fine_count; i++) {
        if (state_fines_owed(b, a->fines[i].member_id) != a->fines[i].owed) {
            diffs++;
            if (print) printf("  fines of %s differ\n", a->fines[i].member_id);
        }
    }
    return diffs;
}

int state_save(const LibraryState *st, const char *path) {
    return storage_save(path, STATE_MAGIC, st, sizeof(LibraryState), 1);
}

StorageResult state_load(LibraryState *st, const char *path) {
    void *records = NULL;
    int count = 0;
    StorageResult rc = storage_load(path, STATE_MAGIC, sizeof(LibraryState), 1, &records, &count);
    if (rc != STORAGE_OK) return rc;
    if (count != 1) {
        free(records);
        return STORAGE_ERROR;
    }
    memcpy(st, records, sizeof(LibraryState));
    free(records);

    /* Bound every count and terminate every string, so a corrupted file
     * cannot cause out-of-bounds reads. */
    if (st->copy_count < 0 || st->copy_count > MAX_COPIES) st->copy_count = 0;
    if (st->queue_count < 0 || st->queue_count > MAX_RESERVATIONS) st->queue_count = 0;
    if (st->nonce_count < 0 || st->nonce_count > MAX_SIGNERS) st->nonce_count = 0;
    if (st->fine_count < 0 || st->fine_count > MAX_MEMBERS) st->fine_count = 0;
    for (int i = 0; i < MAX_COPIES; i++) {
        Copy *c = &st->copies[i];
        c->copy_id[COPY_ID_MAX - 1] = c->book_id[ID_MAX - 1] = '\0';
        c->current_holder_id[ID_MAX - 1] = c->reserved_for[ID_MAX - 1] = '\0';
    }
    for (int i = 0; i < MAX_RESERVATIONS; i++) {
        st->queue[i].book_id[ID_MAX - 1] = st->queue[i].member_id[ID_MAX - 1] = '\0';
    }
    for (int i = 0; i < MAX_SIGNERS; i++) st->nonces[i].signer[SIGNER_MAX - 1] = '\0';
    for (int i = 0; i < MAX_MEMBERS; i++) st->fines[i].member_id[ID_MAX - 1] = '\0';
    return STORAGE_OK;
}
