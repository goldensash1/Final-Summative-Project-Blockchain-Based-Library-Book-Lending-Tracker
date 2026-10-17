#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "lending.h"
#include "input.h"

/* ======================= Helpers ======================= */

/* The state including every pending request (caller frees). */
static LibraryState *projected(const App *app) {
    LibraryState *st = malloc(sizeof(LibraryState));
    if (st) app_projected_state(app, st);
    else printf("ERROR: Out of memory\n");
    return st;
}

/* Reads and validates a member ID argument (format, then existence). */
static Member *member_arg(App *app, int argc, char **argv, int i) {
    char id[ID_MAX + 8];
    if (!cli_id_arg(argc, argv, i, "Member ID: ", id, sizeof(id))) return NULL;
    if (!valid_entity_id(id)) {
        printf("ERROR: '%s' is not a valid member ID (3-15 letters/digits, e.g. ALU001)\n", id);
        return NULL;
    }
    Member *m = registry_find_member(&app->reg, id);
    if (!m) printf("ERROR: Member %s not found in the member registry\n", id);
    return m;
}

static Book *book_by_id(App *app, const char *id) {
    if (!valid_entity_id(id)) {
        printf("ERROR: '%s' is not a valid book ID (e.g. BK001)\n", id);
        return NULL;
    }
    Book *b = registry_find_book(&app->reg, id);
    if (!b) printf("ERROR: Book %s not found in the book registry\n", id);
    return b;
}

static Book *book_arg(App *app, int argc, char **argv, int i) {
    char id[ID_MAX + 8];
    if (!cli_id_arg(argc, argv, i, "Book ID: ", id, sizeof(id))) return NULL;
    return book_by_id(app, id);
}

/* Reads a copy ID and finds it in `st`. */
static const Copy *copy_arg(App *app, const LibraryState *st, int argc, char **argv, int i) {
    char id[COPY_ID_MAX + 8];
    if (!cli_id_arg(argc, argv, i, "Copy ID (e.g. BK001-C1): ", id, sizeof(id))) return NULL;
    (void)app;
    if (!valid_copy_id_format(id)) {
        printf("ERROR: '%s' is not a valid copy ID (format <BOOK_ID>-C<n>, e.g. BK001-C1)\n", id);
        return NULL;
    }
    const Copy *c = state_find_copy_const(st, id);
    if (!c) printf("ERROR: Copy %s is not in the copy inventory\n", id);
    return c;
}

static int valid_reason_code(const char *s) {
    size_t len = strlen(s);
    if (len < 3 || len >= REASON_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
    }
    return 1;
}

static void base_request(PendingRequest *req, int action, const Book *book, const char *copy_id,
                         const Member *m) {
    memset(req, 0, sizeof(*req));
    record_init(&req->rec, action);
    LendingRecord *r = &req->rec;
    if (book) {
        snprintf(r->book_id, sizeof(r->book_id), "%s", book->book_id);
        snprintf(r->book_title, sizeof(r->book_title), "%s", book->title);
    } else {
        snprintf(r->book_id, sizeof(r->book_id), "-");
    }
    snprintf(r->copy_id, sizeof(r->copy_id), "%s", copy_id ? copy_id : "-");
    snprintf(r->member_id, sizeof(r->member_id), "%s", m ? m->member_id : "-");
    snprintf(r->member_name, sizeof(r->member_name), "%s", m ? m->full_name : "-");
    req->priority = PRIORITY_NORMAL;
}

static void print_date_line(const char *label, time_t t) {
    char d[16];
    format_date(t, d, sizeof(d));
    printf("%s%s", label, d);
}

/* ======================= Membership ======================= */

void cmd_register_member(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    char id[ID_MAX + 8], name[PERSON_NAME_MAX + 8], course[16], err[160];
    if (!cli_id_arg(argc, argv, 1, "New member ID (e.g. ALU006): ", id, sizeof(id)) ||
        !cli_arg(argc, argv, 2, "Full name: ", name, sizeof(name)) ||
        !cli_arg(argc, argv, 3, "Course code: ", course, sizeof(course))) {
        printf("ERROR: register_member needs <member_id> \"<full name>\" <course_code>\n");
        return;
    }
    to_upper_trim(course);
    if (!registry_add_member(&app->reg, MEMBERS_FILE, id, name, course, err, sizeof(err))) {
        printf("ERROR: %s\n", err);
        return;
    }
    int created = 0;
    char pub[PUBKEY_HEX_LEN + 1] = "";
    if (!keystore_ensure_member(id, &created) || !keystore_registered_pubkey(id, pub)) {
        printf("ERROR: Member saved but the key pair could not be generated\n");
        return;
    }
    ledger_add_account(&app->ledger, id);
    printf("SUCCESS: Member %s (%s, %s) registered in %s with token balance 0.\n", id, name, course,
           MEMBERS_FILE);
    printf("  ECDSA P-256 key pair generated: %s/%s_private.pem (0600) and _public.pem\n",
           MEMBER_KEYS_DIR, id);
    printf("  On-chain identity (public key): %.32s...\n", pub);
}

void cmd_view_member(App *app, int argc, char **argv) {
    Member *m = member_arg(app, argc, argv, 1);
    if (!m) return;
    LibraryState *st = projected(app);
    if (!st) return;

    char pub[PUBKEY_HEX_LEN + 1] = "(no key)";
    keystore_registered_pubkey(m->member_id, pub);
    time_t now = app_now(app);

    printf("\nMember %s\n", m->member_id);
    printf("  Name / course      : %s / %s\n", m->full_name, m->course_code);
    printf("  Public key (ID)    : %.40s...\n", pub);
    printf("  Request nonce      : %u confirmed, next request must use %u\n",
           state_nonce(app->state, m->member_id), state_nonce(st, m->member_id));
    printf("  Token balance      : %s coins (%s model)\n",
           fmt_amount(ledger_balance(&app->ledger, m->member_id)), ledger_model_name(app->ledger.model));
    printf("  Fines              : %d owed (confirmed), %d owed incl. pending, %d paid\n",
           state_fines_owed(app->state, m->member_id), state_fines_owed(st, m->member_id),
           state_fines_paid(st, m->member_id));
    printf("  Copies held (%d, max %d):\n", state_held_count(st, m->member_id), MAX_CONCURRENT_BORROWS);
    for (int i = 0; i < st->copy_count; i++) {
        const Copy *c = &st->copies[i];
        if (c->state != COPY_BORROWED || strcmp(c->current_holder_id, m->member_id) != 0) continue;
        int late = days_overdue(c->due_date, now);
        print_date_line("    ", c->due_date);
        printf("  %s  renewals %d/%d%s\n", c->copy_id, c->renew_count, MAX_RENEWALS,
               late ? "  OVERDUE" : "");
    }
    for (int i = 0; i < st->copy_count; i++) {
        const Copy *c = &st->copies[i];
        if (c->state == COPY_RESERVED && strcmp(c->reserved_for, m->member_id) == 0) {
            printf("  Copy %s is being HELD for this member (reservation): borrow_book %s %s\n",
                   c->copy_id, m->member_id, c->copy_id);
        }
    }
    for (int i = 0; i < st->queue_count; i++) {
        if (strcmp(st->queue[i].member_id, m->member_id) == 0) {
            printf("  In the reservation queue for %s (position %d)\n", st->queue[i].book_id,
                   state_queue_position(st, st->queue[i].book_id, m->member_id));
        }
    }
    free(st);
}

void cmd_member_nonce(App *app, int argc, char **argv) {
    Member *m = member_arg(app, argc, argv, 1);
    if (!m) return;
    LibraryState *st = projected(app);
    if (!st) return;
    unsigned int confirmed = state_nonce(app->state, m->member_id);
    unsigned int next = state_nonce(st, m->member_id);
    printf("Member %s request nonce: %u confirmed on chain; %u pending; next request must carry "
           "nonce %u.\n", m->member_id, confirmed, next - confirmed, next);
    printf("(The nonce increments only when a request is mined; a request with any other nonce is "
           "rejected as a replay.)\n");
    free(st);
}

void cmd_list_members(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    LibraryState *st = projected(app);
    if (!st) return;
    printf("\n%-9s %-22s %-7s %10s %5s %6s %5s\n", "Member", "Full Name", "Course", "Balance",
           "Held", "Fines", "Nonce");
    for (int i = 0; i < 70; i++) putchar('-');
    putchar('\n');
    for (int i = 0; i < app->reg.member_count; i++) {
        const Member *m = &app->reg.members[i];
        printf("%-9s %-22.22s %-7s %10s %5d %6d %5u\n", m->member_id, m->full_name, m->course_code,
               fmt_amount(ledger_balance(&app->ledger, m->member_id)), state_held_count(st, m->member_id),
               state_fines_owed(st, m->member_id), state_nonce(st, m->member_id));
    }
    printf("%d member(s). Balances are confirmed tokens; held/fines/nonce include pending requests.\n",
           app->reg.member_count);
    free(st);
}

/* ======================= Books and copies ======================= */

void cmd_register_book(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    char id[ID_MAX + 8], title[TITLE_MAX + 8], author[PERSON_NAME_MAX + 8], err[160];
    if (!cli_id_arg(argc, argv, 1, "New book ID (e.g. BK006): ", id, sizeof(id)) ||
        !cli_arg(argc, argv, 2, "Title: ", title, sizeof(title)) ||
        !cli_arg(argc, argv, 3, "Author: ", author, sizeof(author))) {
        printf("ERROR: register_book needs <book_id> \"<title>\" \"<author>\" [copies=N]\n");
        return;
    }
    int copies = 1;
    const char *opt = cli_option(argc, argv, "copies");
    if (opt && (!parse_int(opt, &copies) || copies < 0 || copies > 20)) {
        printf("ERROR: copies must be 0-20\n");
        return;
    }
    if (!registry_add_book(&app->reg, BOOKS_FILE, id, title, author, err, sizeof(err))) {
        printf("ERROR: %s\n", err);
        return;
    }
    printf("SUCCESS: Book %s '%s' by %s registered in %s.\n", id, title, author, BOOKS_FILE);
    for (int i = 0; i < copies; i++) {
        char copy_id[COPY_ID_MAX];
        state_next_copy_id(app->state, id, copy_id, sizeof(copy_id));
        if (state_add_copy(app->state, COPIES_FILE, copy_id, id, COND_NEW, app->chain.count)) {
            printf("  Copy %s added (NEW, AVAILABLE)\n", copy_id);
        }
    }
    app_save_all(app);
}

void cmd_add_copy(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    Book *book = book_arg(app, argc, argv, 1);
    if (!book) return;
    int condition = COND_NEW;
    const char *cond = cli_positional(argc, argv, 2);
    if (cond) {
        condition = condition_parse(cond);
        if (condition < COND_NEW || condition > COND_WORN) {
            printf("ERROR: Condition must be NEW, GOOD or WORN\n");
            return;
        }
    }
    char copy_id[COPY_ID_MAX];
    state_next_copy_id(app->state, book->book_id, copy_id, sizeof(copy_id));
    if (!state_add_copy(app->state, COPIES_FILE, copy_id, book->book_id, condition, app->chain.count)) {
        printf("ERROR: Could not add the copy (inventory full or file not writable)\n");
        return;
    }
    app_save_all(app);
    printf("SUCCESS: Copy %s of '%s' added (%s, AVAILABLE). '%s' now has %d copies.\n", copy_id,
           book->title, condition_name(condition), book->book_id,
           state_count_copies(app->state, book->book_id, -1));
}

static void print_copy_table_header(void) {
    printf("  %-10s %-6s %-10s %-8s %-10s %-8s %s\n", "Copy", "Cond.", "State", "Holder", "Due",
           "Renewed", "Held for");
}

static void print_copy_row(const Copy *c, time_t now) {
    char due[16];
    format_date(c->due_date, due, sizeof(due));
    printf("  %-10s %-6s %-10s %-8s %-10s %d/%-6d %s%s\n", c->copy_id, condition_name(c->condition),
           copy_state_name(c->state), c->current_holder_id[0] ? c->current_holder_id : "-", due,
           c->renew_count, MAX_RENEWALS, c->reserved_for[0] ? c->reserved_for : "-",
           c->state == COPY_BORROWED && days_overdue(c->due_date, now) ? "  OVERDUE" : "");
}

void cmd_view_book(App *app, int argc, char **argv) {
    Book *book = book_arg(app, argc, argv, 1);
    if (!book) return;
    LibraryState *st = projected(app);
    if (!st) return;
    time_t now = app_now(app);
    printf("\nBook %s: '%s' by %s\n", book->book_id, book->title, book->author);
    printf("  Copies: %d total, %d available, %d borrowed, %d reserved, %d lost\n",
           state_count_copies(st, book->book_id, -1), state_count_copies(st, book->book_id, COPY_AVAILABLE),
           state_count_copies(st, book->book_id, COPY_BORROWED),
           state_count_copies(st, book->book_id, COPY_RESERVED),
           state_count_copies(st, book->book_id, COPY_LOST));
    print_copy_table_header();
    for (int i = 0; i < st->copy_count; i++) {
        if (strcmp(st->copies[i].book_id, book->book_id) == 0) print_copy_row(&st->copies[i], now);
    }
    int q = state_queue_length(st, book->book_id);
    printf("  Reservation queue: %d member(s)", q);
    for (int i = 0, pos = 0; i < st->queue_count; i++) {
        if (strcmp(st->queue[i].book_id, book->book_id) == 0) {
            printf("%s%d. %s", pos ? ", " : ": ", pos + 1, st->queue[i].member_id);
            pos++;
        }
    }
    printf("\n  (includes pending requests; mined state may lag until the pool is mined)\n");
    free(st);
}

/* Prints every record about a copy / book / member, chain first then pool. */
static void print_history(const App *app, const char *member_id, const char *book_id,
                          const char *copy_id, int limit) {
    int shown = 0;
    printf("  %-5s ", "Block");
    record_print_header("");
    for (int b = 0; b < app->chain.count; b++) {
        const Block *blk = &app->chain.blocks[b];
        for (int r = 0; r < blk->record_count; r++) {
            const LendingRecord *rec = &blk->records[r];
            if (member_id && strcmp(rec->member_id, member_id) != 0) continue;
            if (book_id && strcmp(rec->book_id, book_id) != 0) continue;
            if (copy_id && strcmp(rec->copy_id, copy_id) != 0) continue;
            printf("  #%-4d ", b);
            record_print_row(rec, "");
            shown++;
        }
    }
    for (int i = 0; i < app->pending.count && (limit <= 0 || shown < limit + 20); i++) {
        const LendingRecord *rec = &app->pending.items[i].rec;
        if (member_id && strcmp(rec->member_id, member_id) != 0) continue;
        if (book_id && strcmp(rec->book_id, book_id) != 0) continue;
        if (copy_id && strcmp(rec->copy_id, copy_id) != 0) continue;
        printf("  %-5s ", "pend");
        record_print_row(rec, "");
        shown++;
    }
    if (shown == 0) printf("  (no records)\n");
}

void cmd_view_copy(App *app, int argc, char **argv) {
    LibraryState *st = projected(app);
    if (!st) return;
    const Copy *c = copy_arg(app, st, argc, argv, 1);
    if (c) {
        const Book *book = registry_find_book(&app->reg, c->book_id);
        printf("\nCopy %s of %s '%s'\n", c->copy_id, c->book_id, book ? book->title : "?");
        print_copy_table_header();
        print_copy_row(c, app_now(app));
        printf("  History:\n");
        print_history(app, NULL, NULL, c->copy_id, 0);
    }
    free(st);
}

void cmd_mark_lost(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    LibraryState *st = projected(app);
    if (!st) return;
    const Copy *c = copy_arg(app, st, argc, argv, 1);
    char reason[REASON_MAX + 8];
    if (!c) goto done;
    if (!cli_id_arg(argc, argv, 2, "Librarian reason code (e.g. MISSING_SHELF, DAMAGED_WATER): ",
                    reason, sizeof(reason))) {
        printf("ERROR: Marking a copy LOST requires a reason code\n");
        goto done;
    }
    if (!valid_reason_code(reason)) {
        printf("ERROR: Reason code must be 3-23 letters, digits or '_'\n");
        goto done;
    }

    Book *book = registry_find_book(&app->reg, c->book_id);
    Member *holder = c->state == COPY_BORROWED ? registry_find_member(&app->reg, c->current_holder_id) : NULL;
    PendingRequest req;
    base_request(&req, ACT_LOST, book, c->copy_id, holder);
    req.rec.condition = COND_LOST;
    snprintf(req.rec.reason_code, sizeof(req.rec.reason_code), "%s", reason);
    req.rec.fine_amount = holder ? LOST_REPLACEMENT_FINE : 0;
    req.rec.due_date = c->due_date;

    char signer[SIGNER_MAX];
    app_librarian_signer(app, signer, sizeof(signer));
    printf("Librarian override: copy %s (%s) -> LOST, reason %s%s.\n", c->copy_id,
           copy_state_name(c->state), reason, holder ? ", replacement fine charged to the holder" : "");
    if (app_submit(app, &req, signer, -1) && holder) {
        printf("  %s (%s) will owe %d coin(s) once this is mined.\n", holder->full_name,
               holder->member_id, LOST_REPLACEMENT_FINE);
    }
done:
    free(st);
}

void cmd_list_books(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    LibraryState *st = projected(app);
    if (!st) return;
    printf("\n%-7s %-28s %-24s %5s %5s %5s %5s %5s %5s\n", "Book", "Title", "Author", "Total",
           "Avail", "Out", "Held", "Lost", "Queue");
    for (int i = 0; i < 98; i++) putchar('-');
    putchar('\n');
    for (int i = 0; i < app->reg.book_count; i++) {
        const Book *b = &app->reg.books[i];
        printf("%-7s %-28.28s %-24.24s %5d %5d %5d %5d %5d %5d\n", b->book_id, b->title, b->author,
               state_count_copies(st, b->book_id, -1), state_count_copies(st, b->book_id, COPY_AVAILABLE),
               state_count_copies(st, b->book_id, COPY_BORROWED),
               state_count_copies(st, b->book_id, COPY_RESERVED),
               state_count_copies(st, b->book_id, COPY_LOST), state_queue_length(st, b->book_id));
    }
    printf("%d title(s), %d physical copies (including pending requests).\n", app->reg.book_count,
           st->copy_count);
    free(st);
}

/* ======================= Lending ======================= */

void cmd_borrow_book(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    Member *m = member_arg(app, argc, argv, 1);
    if (!m) return;

    char target[COPY_ID_MAX + 8];
    if (!cli_id_arg(argc, argv, 2, "Book ID or copy ID: ", target, sizeof(target))) return;
    int is_copy = strstr(target, "-C") != NULL;
    char book_id[ID_MAX];
    snprintf(book_id, sizeof(book_id), "%.*s", is_copy ? (int)(strstr(target, "-C") - target) : (int)strlen(target), target);
    if (is_copy && !valid_copy_id_format(target)) {
        printf("ERROR: '%s' is not a valid copy ID\n", target);
        return;
    }
    Book *book = book_by_id(app, book_id);
    if (!book) return;

    LibraryState *st = projected(app);
    if (!st) return;
    const Copy *c = is_copy ? state_find_copy_const(st, target)
                            : state_borrowable_copy(st, book->book_id, m->member_id);
    if (is_copy && !c) {
        printf("ERROR: Copy %s is not in the inventory\n", target);
        free(st);
        return;
    }
    if (!c) {
        printf("ERROR: No copy of '%s' is available (%d copies: %d borrowed, %d held for "
               "reservations, %d lost).\n       Use 'reserve_book %s %s' to join the reservation queue.\n",
               book->title, state_count_copies(st, book->book_id, -1),
               state_count_copies(st, book->book_id, COPY_BORROWED),
               state_count_copies(st, book->book_id, COPY_RESERVED),
               state_count_copies(st, book->book_id, COPY_LOST), m->member_id, book->book_id);
        free(st);
        return;
    }

    time_t now = app_now(app);
    time_t due = now + LOAN_PERIOD_DAYS * SECONDS_PER_DAY;
    const char *due_opt = cli_option(argc, argv, "due");
    if (due_opt) {
        if (!parse_date(due_opt, &due)) {
            printf("ERROR: Invalid due date '%s' (use a real date, YYYY-MM-DD)\n", due_opt);
            free(st);
            return;
        }
        if (due <= now) {
            printf("ERROR: The due date %s is in the past\n", due_opt);
            free(st);
            return;
        }
        if (due > now + MAX_LOAN_DAYS * SECONDS_PER_DAY) {
            printf("ERROR: The due date may be at most %d days ahead\n", MAX_LOAN_DAYS);
            free(st);
            return;
        }
    }
    long nonce = -1;
    const char *nonce_opt = cli_option(argc, argv, "nonce");
    int n;
    if (nonce_opt) {
        if (!parse_int(nonce_opt, &n) || n < 0) {
            printf("ERROR: nonce must be a non-negative number\n");
            free(st);
            return;
        }
        nonce = n;
    }

    PendingRequest req;
    base_request(&req, ACT_BORROWED, book, c->copy_id, m);
    req.rec.due_date = due;
    if (c->state == COPY_RESERVED && strcmp(c->reserved_for, m->member_id) == 0) {
        req.waiting_since = c->reserved_since;   /* served in reservation order */
        printf("Copy %s was held for %s from the reservation queue.\n", c->copy_id, m->member_id);
    }
    char d[16];
    format_date(due, d, sizeof(d));
    printf("Borrow: %s (%s) takes copy %s of '%s', due %s.\n", m->full_name, m->member_id,
           c->copy_id, book->title, d);
    free(st);
    if (app_submit(app, &req, m->member_id, nonce) &&
        app->pending.items[app->pending.count - 1].status == REQ_PENDING) {
        printf("  The copy is BORROWED once this request is mined (pool_view / mine_solo).\n");
    }
}

/* The most recent BORROWED record of the copy on the chain (NULL if none,
 * or if the copy was returned after it). */
static const LendingRecord *chain_open_borrow(const App *app, const char *copy_id) {
    for (int b = app->chain.count - 1; b >= 0; b--) {
        const Block *blk = &app->chain.blocks[b];
        for (int r = blk->record_count - 1; r >= 0; r--) {
            const LendingRecord *rec = &blk->records[r];
            if (strcmp(rec->copy_id, copy_id) != 0) continue;
            if (rec->action == ACT_BORROWED) return rec;
            if (rec->action == ACT_RETURNED || rec->action == ACT_LOST) return NULL;
        }
    }
    return NULL;
}

void cmd_return_book(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    LibraryState *st = projected(app);
    if (!st) return;
    const Copy *c = copy_arg(app, st, argc, argv, 1);
    if (!c) goto done;

    const LendingRecord *borrow = chain_open_borrow(app, c->copy_id);
    if (!borrow) {
        if (c->state == COPY_BORROWED) {
            printf("ERROR: The BORROWED record of copy %s is still in the pending pool, not on the "
                   "chain. Mine it first.\n", c->copy_id);
        } else {
            printf("ERROR: No BORROWED record for copy %s on the chain (it is %s).\n", c->copy_id,
                   copy_state_name(c->state));
        }
        goto done;
    }
    if (c->state != COPY_BORROWED || strcmp(c->current_holder_id, borrow->member_id) != 0) {
        printf("ERROR: Copy %s is not currently on loan (a return may already be pending)\n", c->copy_id);
        goto done;
    }

    Member *m = registry_find_member(&app->reg, borrow->member_id);
    Book *book = registry_find_book(&app->reg, c->book_id);
    if (!m || !book) {
        printf("ERROR: The loan refers to an unregistered member or book\n");
        goto done;
    }

    int condition = c->condition;
    const char *cond = cli_positional(argc, argv, 2);
    if (cond && (condition = condition_parse(cond)) == COND_NONE) {
        printf("ERROR: Condition must be NEW, GOOD or WORN\n");
        goto done;
    }
    const char *reason = cli_option(argc, argv, "reason");
    char reason_code[REASON_MAX] = "";
    if (reason) {
        snprintf(reason_code, sizeof(reason_code), "%s", reason);
        to_upper_trim(reason_code);
        if (!valid_reason_code(reason_code)) {
            printf("ERROR: Reason code must be 3-23 letters, digits or '_'\n");
            goto done;
        }
    }

    time_t now = app_now(app);
    int late_days = days_overdue(c->due_date, now);
    int late = late_days > 0;
    PendingRequest req;
    base_request(&req, ACT_RETURNED, book, c->copy_id, m);
    req.rec.due_date = c->due_date;
    req.rec.condition = condition;
    snprintf(req.rec.reason_code, sizeof(req.rec.reason_code), "%s", reason_code);
    req.rec.fine_amount = late_days * FINE_PER_DAY;
    req.rec.token_reward = late ? REWARD_LATE_COINS : REWARD_ON_TIME_COINS;
    req.priority = late || c->overdue_flagged ? PRIORITY_OVERDUE : PRIORITY_NORMAL;

    char due[16];
    format_date(c->due_date, due, sizeof(due));
    printf("Return: copy %s ('%s') by %s (%s), due %s: %s", c->copy_id, book->title, m->full_name,
           m->member_id, due, late ? "LATE" : "ON TIME");
    if (late) printf(" by %d day(s), fine %d coin(s) at %d/day", late_days, req.rec.fine_amount, FINE_PER_DAY);
    printf(". Condition %s -> %s.\n", condition_name(c->condition), condition_name(condition));

    int queue = state_queue_length(st, c->book_id);
    if (app_submit(app, &req, m->member_id, -1)) {
        printf("  Token reward when mined: %d coins (%s) -> %s to %s after the %s-coin fee.\n",
               req.rec.token_reward, late ? "late return" : "on-time return",
               fmt_amount((Amount)req.rec.token_reward * COIN - TX_FEE), m->member_id, fmt_amount(TX_FEE));
        if (queue > 0) {
            printf("  %d member(s) are waiting for '%s': this copy will be offered (RESERVED) to the "
                   "first in the queue.\n", queue, book->title);
        }
    }
done:
    free(st);
}

void cmd_renew_book(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    LibraryState *st = projected(app);
    if (!st) return;
    const Copy *c = copy_arg(app, st, argc, argv, 1);
    if (!c) goto done;
    if (c->state != COPY_BORROWED) {
        printf("ERROR: Copy %s is %s, not on loan\n", c->copy_id, copy_state_name(c->state));
        goto done;
    }
    Member *m = registry_find_member(&app->reg, c->current_holder_id);
    Book *book = registry_find_book(&app->reg, c->book_id);
    if (!m || !book) goto done;

    PendingRequest req;
    base_request(&req, ACT_RENEWED, book, c->copy_id, m);
    req.rec.due_date = c->due_date + RENEWAL_DAYS * SECONDS_PER_DAY;
    char old_due[16], new_due[16];
    format_date(c->due_date, old_due, sizeof(old_due));
    format_date(req.rec.due_date, new_due, sizeof(new_due));
    printf("Renew: copy %s held by %s, due %s -> %s (+%d days, renewal %d of %d).\n", c->copy_id,
           m->member_id, old_due, new_due, RENEWAL_DAYS, c->renew_count + 1, MAX_RENEWALS);
    app_submit(app, &req, m->member_id, -1);
done:
    free(st);
}

void cmd_reserve_book(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    Member *m = member_arg(app, argc, argv, 1);
    if (!m) return;
    Book *book = book_arg(app, argc, argv, 2);
    if (!book) return;

    LibraryState *st = projected(app);
    if (!st) return;
    int position = state_queue_length(st, book->book_id) + 1;
    free(st);

    PendingRequest req;
    base_request(&req, ACT_RESERVED, book, NULL, m);
    printf("Reserve: %s (%s) joins the queue for '%s' at position %d.\n", m->full_name,
           m->member_id, book->title, position);
    if (app_submit(app, &req, m->member_id, -1)) {
        printf("  When a copy is returned it is offered to the queue in order before anyone else can "
               "borrow it.\n");
    }
}

void cmd_reservation_status(App *app, int argc, char **argv) {
    LibraryState *st = projected(app);
    if (!st) return;
    const char *only = cli_positional(argc, argv, 1);
    char filter[ID_MAX] = "";
    if (only) {
        snprintf(filter, sizeof(filter), "%s", only);
        to_upper_trim(filter);
        if (!book_by_id(app, filter)) {
            free(st);
            return;
        }
    }

    printf("\nReservation status%s%s (FIFO queues, including pending requests)\n", filter[0] ? " for " : "", filter);
    int any = 0;
    for (int b = 0; b < app->reg.book_count; b++) {
        const Book *book = &app->reg.books[b];
        if (filter[0] && strcmp(filter, book->book_id) != 0) continue;
        int q = state_queue_length(st, book->book_id);
        int held = state_count_copies(st, book->book_id, COPY_RESERVED);
        if (!q && !held && !filter[0]) continue;
        any = 1;
        printf("  %s '%s': %d available, %d waiting in queue\n", book->book_id, book->title,
               state_count_copies(st, book->book_id, COPY_AVAILABLE), q);
        for (int i = 0, pos = 0; i < st->queue_count; i++) {
            if (strcmp(st->queue[i].book_id, book->book_id) != 0) continue;
            char since[24];
            format_datetime(st->queue[i].reservation_date, since, sizeof(since));
            printf("     %d. %s since %s\n", ++pos, st->queue[i].member_id, since);
        }
        for (int i = 0; i < st->copy_count; i++) {
            const Copy *c = &st->copies[i];
            if (c->state == COPY_RESERVED && strcmp(c->book_id, book->book_id) == 0) {
                printf("     Copy %s is RESERVED (held) for %s: offered automatically on return\n",
                       c->copy_id, c->reserved_for);
            }
        }
    }
    if (!any) printf("  (no reservations)\n");
    free(st);
}

/* ======================= Fines and overdue ======================= */

void cmd_check_overdue(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!app_require_unlocked(app)) return;
    app_overdue_scan(app, 1);

    LibraryState *st = projected(app);
    if (!st) return;
    time_t now = app_now(app);
    char today[16];
    format_date(now, today, sizeof(today));
    printf("\nOverdue copies on %s (fine %d coin(s) per day):\n", today, FINE_PER_DAY);
    int any = 0;
    for (int i = 0; i < st->copy_count; i++) {
        const Copy *c = &st->copies[i];
        int days = c->state == COPY_BORROWED ? days_overdue(c->due_date, now) : 0;
        if (!days) continue;
        char due[16];
        format_date(c->due_date, due, sizeof(due));
        printf("  %-10s held by %-7s due %s: %d day(s) overdue, fine accrued %d coin(s)%s\n",
               c->copy_id, c->current_holder_id, due, days, days * FINE_PER_DAY,
               c->overdue_flagged ? " [OVERDUE record]" : "");
        any = 1;
    }
    if (!any) printf("  (none)\n");
    free(st);
}

void cmd_view_fines(App *app, int argc, char **argv) {
    LibraryState *st = projected(app);
    if (!st) return;
    const char *only = cli_positional(argc, argv, 1);
    char filter[ID_MAX] = "";
    if (only) {
        snprintf(filter, sizeof(filter), "%s", only);
        to_upper_trim(filter);
        if (!registry_find_member(&app->reg, filter)) {
            printf("ERROR: Member %s not found\n", filter);
            free(st);
            return;
        }
    }
    time_t now = app_now(app);
    printf("\n%-8s %-20s %8s %10s %6s %9s\n", "Member", "Name", "Owed", "Owed+pend", "Paid", "Accruing");
    for (int i = 0; i < 66; i++) putchar('-');
    putchar('\n');
    for (int i = 0; i < app->reg.member_count; i++) {
        const Member *m = &app->reg.members[i];
        if (filter[0] && strcmp(filter, m->member_id) != 0) continue;
        int accruing = 0;
        for (int k = 0; k < st->copy_count; k++) {
            const Copy *c = &st->copies[k];
            if (c->state == COPY_BORROWED && strcmp(c->current_holder_id, m->member_id) == 0) {
                accruing += days_overdue(c->due_date, now) * FINE_PER_DAY;
            }
        }
        int owed = state_fines_owed(app->state, m->member_id);
        int owed_pending = state_fines_owed(st, m->member_id);
        if (!filter[0] && !owed && !owed_pending && !accruing && !state_fines_paid(st, m->member_id)) continue;
        printf("%-8s %-20.20s %8d %10d %6d %9d\n", m->member_id, m->full_name, owed, owed_pending,
               state_fines_paid(st, m->member_id), accruing);
    }
    printf("Owed = confirmed on chain (late returns, lost copies); Accruing = still-overdue copies,\n"
           "charged when they are returned. Pay with: settle_fine <member_id> [amount] [method=tokens|cash]\n");
    free(st);
}

void cmd_settle_fine(App *app, int argc, char **argv) {
    if (!app_require_unlocked(app)) return;
    Member *m = member_arg(app, argc, argv, 1);
    if (!m) return;
    LibraryState *st = projected(app);
    if (!st) return;
    int owed = state_fines_owed(st, m->member_id);
    free(st);
    if (owed <= 0) {
        printf("ERROR: %s has no outstanding fines (fines are charged when a late return is mined)\n",
               m->member_id);
        return;
    }

    int amount = owed;
    const char *amount_arg = cli_positional(argc, argv, 2);
    if (amount_arg && (!parse_int(amount_arg, &amount) || amount <= 0 || amount > owed)) {
        printf("ERROR: Amount must be a whole number of coins from 1 to %d\n", owed);
        return;
    }
    const char *method = cli_option(argc, argv, "method");
    int by_tokens = !method || strcmp(method, "tokens") == 0;
    if (method && !by_tokens && strcmp(method, "cash") != 0) {
        printf("ERROR: method must be 'tokens' or 'cash'\n");
        return;
    }

    PendingRequest req;
    base_request(&req, ACT_FINE_PAID, NULL, NULL, m);
    snprintf(req.rec.book_title, sizeof(req.rec.book_title), "Fine settlement");
    req.rec.fine_amount = amount;
    req.priority = PRIORITY_OVERDUE;   /* fine settlements are overdue-related */
    if (by_tokens) {
        char memo[TX_MEMO_MAX];
        snprintf(memo, sizeof(memo), "fine payment %d coins", amount);
        if (!tx_init(&req.tx, TX_TRANSFER, m->member_id, LIBRARY_ACCOUNT, (Amount)amount * COIN,
                     TX_FEE, ledger_next_nonce(&app->ledger, m->member_id), memo)) {
            printf("ERROR: Could not create the payment transaction\n");
            return;
        }
        req.has_tx = 1;
        snprintf(req.rec.tx_id, sizeof(req.rec.tx_id), "%s", req.tx.txid);
    }
    printf("Settle fine: %s pays %d of %d coin(s) owed %s.\n", m->member_id, amount, owed,
           by_tokens ? "in tokens to the LIBRARY account (+1 coin fee)" : "in cash (recorded on-chain)");
    app_submit(app, &req, m->member_id, -1);
}

void cmd_advance_days(App *app, int argc, char **argv) {
    if (!app_require_admin(app) || !app_require_unlocked(app)) return;
    char buf[16];
    int days;
    if (!cli_arg(argc, argv, 1, "Days to advance the library clock (1-365): ", buf, sizeof(buf)) ||
        !parse_int(buf, &days) || days < 1 || days > 365) {
        printf("ERROR: Days must be 1-365\n");
        return;
    }
    app->meta.clock_offset += (long)days * SECONDS_PER_DAY;
    app_save_pending(app);
    char today[24];
    format_datetime(app_now(app), today, sizeof(today));
    printf("Library clock advanced by %d day(s): it is now %s (simulated; offset saved in %s).\n",
           days, today, META_FILE);
    app_overdue_scan(app, 1);
}

/* ======================= Audit ======================= */

void cmd_lending_history(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\nLending history: every record on the chain (then pending requests)\n");
    print_history(app, NULL, NULL, NULL, 0);
}

void cmd_member_history(App *app, int argc, char **argv) {
    Member *m = member_arg(app, argc, argv, 1);
    if (!m) return;
    printf("\nHistory of member %s (%s)\n", m->member_id, m->full_name);
    print_history(app, m->member_id, NULL, NULL, 0);
}

void cmd_book_history(App *app, int argc, char **argv) {
    char id[COPY_ID_MAX + 8];
    if (!cli_id_arg(argc, argv, 1, "Book ID or copy ID: ", id, sizeof(id))) return;
    if (strstr(id, "-C")) {
        printf("\nHistory of copy %s\n", id);
        print_history(app, NULL, NULL, id, 0);
        return;
    }
    Book *book = book_by_id(app, id);
    if (!book) return;
    printf("\nHistory of %s '%s' (all copies)\n", book->book_id, book->title);
    print_history(app, NULL, book->book_id, NULL, 0);
}
