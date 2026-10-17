#ifndef STATE_H
#define STATE_H

#include <time.h>
#include "record.h"
#include "registry.h"
#include "storage.h"

/* Library rules (the configurable values of the lending lifecycle). */
#define LOAN_PERIOD_DAYS 14        /* default due date = borrow date + 14 days */
#define MAX_LOAN_DAYS 60           /* a requested due date may be at most 60 days ahead */
#define RENEWAL_DAYS 7             /* each renewal extends the due date by 7 days */
#define MAX_RENEWALS 2             /* per loan */
#define FINE_PER_DAY 1             /* coins per day overdue */
#define LOST_REPLACEMENT_FINE 20   /* coins charged when a borrowed copy is marked LOST */
#define SECONDS_PER_DAY 86400L

#define MAX_COPIES 1000
#define MAX_RESERVATIONS 500
#define MAX_SIGNERS 600

typedef enum {
    COPY_AVAILABLE = 0,
    COPY_BORROWED,
    COPY_RESERVED,   /* returned and held for the next member in the queue */
    COPY_LOST
} CopyState;

typedef struct {
    char copy_id[COPY_ID_MAX];
    char book_id[ID_MAX];
    int condition;                    /* CopyCondition */
    int state;                        /* CopyState */
    char current_holder_id[ID_MAX];   /* member holding it, "" if none */
    time_t due_date;                  /* 0 unless BORROWED */
    time_t borrowed_at;
    int renew_count;
    int overdue_flagged;              /* an OVERDUE record exists for this loan */
    char reserved_for[ID_MAX];        /* member it is held for while RESERVED */
    time_t reserved_since;            /* when that member joined the queue */
} Copy;

/* One entry of a title's FIFO reservation queue. All titles share one array;
 * entries for a book_id keep their arrival order. */
typedef struct {
    char book_id[ID_MAX];
    char member_id[ID_MAX];
    time_t reservation_date;
} Reservation;

/* Replay protection: the next nonce each signer must use. */
typedef struct {
    char signer[SIGNER_MAX];
    unsigned int nonce;
} SignerNonce;

typedef struct {
    char member_id[ID_MAX];
    int owed;    /* coins */
    int paid;
} MemberFines;

/* Everything the lending records change: the copy inventory, the reservation
 * queues, the signers' nonces and the members' fines. The confirmed state is
 * the result of applying every record on the chain, in order, to the copies
 * loaded from copies.txt. */
typedef struct {
    Copy copies[MAX_COPIES];
    int copy_count;
    Reservation queue[MAX_RESERVATIONS];
    int queue_count;
    SignerNonce nonces[MAX_SIGNERS];
    int nonce_count;
    MemberFines fines[MAX_MEMBERS];
    int fine_count;
} LibraryState;

const char *copy_state_name(int state);

/* Empty state, then the copies of copies.txt
 * (copy_id,book_id,condition[,added_at_block]) whose added_at_block is at
 * most max_height (initial copies have none = 0). Every book_id must exist in
 * the registry. Returns 0 if the file is missing, empty or has no valid copy. */
int state_load_copies(LibraryState *st, const char *path, const Registry *reg, int max_height);

/* Adds the copies of copies.txt added at a block index in [min, max]
 * (used while replaying the chain). Returns the number added, -1 on error. */
int state_load_copies_added(LibraryState *st, const char *path, const Registry *reg,
                            int min_height, int max_height);

/* Adds a new copy (AVAILABLE) and appends it to copies.txt, recording the
 * index of the next block so a replay creates it at the same point. */
int state_add_copy(LibraryState *st, const char *path, const char *copy_id,
                   const char *book_id, int condition, int added_at_block);

/* Next free copy ID for a title: <book_id>-C<n>. */
void state_next_copy_id(const LibraryState *st, const char *book_id, char *out, size_t size);

/* 1 if `id` looks like <book_id>-C<digits>. */
int valid_copy_id_format(const char *id);

Copy *state_find_copy(LibraryState *st, const char *copy_id);
const Copy *state_find_copy_const(const LibraryState *st, const char *copy_id);

/* Number of copies of the title (state < 0: any state). */
int state_count_copies(const LibraryState *st, const char *book_id, int state);

/* The copy the member can borrow: one RESERVED for them first, otherwise the
 * first AVAILABLE copy. NULL if none. */
const Copy *state_borrowable_copy(const LibraryState *st, const char *book_id, const char *member_id);

/* 1-based position of the member in the title's queue, 0 if not queued. */
int state_queue_position(const LibraryState *st, const char *book_id, const char *member_id);
int state_queue_length(const LibraryState *st, const char *book_id);

/* Copies the member currently holds (BORROWED). */
int state_held_count(const LibraryState *st, const char *member_id);

unsigned int state_nonce(const LibraryState *st, const char *signer);
int state_fines_owed(const LibraryState *st, const char *member_id);
int state_fines_paid(const LibraryState *st, const char *member_id);

/* The availability state machine. Checks that the record is a valid
 * transition from the current state (and carries the signer's expected
 * nonce); only then applies it. On failure nothing changes and the reason is
 * written to err. Valid transitions:
 *   AVAILABLE -> BORROWED            borrow
 *   RESERVED  -> BORROWED            borrow by the member it is held for
 *   BORROWED  -> AVAILABLE/RESERVED  return (RESERVED when the queue is not empty:
 *                                    the copy is offered to the queue head)
 *   AVAILABLE/BORROWED -> LOST       librarian override with a reason code
 * plus RESERVED (join queue), RENEWED, OVERDUE, FINE_PAID and TRANSFER, which
 * do not change the copy's availability. */
int state_apply(LibraryState *st, const LendingRecord *rec, char *err, size_t err_size);

/* Compares two states; prints the first differences when `print`. Returns
 * the number of differences (0 = identical). */
int state_compare(const LibraryState *a, const LibraryState *b, int print);

int state_save(const LibraryState *st, const char *path);
StorageResult state_load(LibraryState *st, const char *path);

#endif
