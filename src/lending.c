#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lending.h"
#include "input.h"

/* ======================= Timeline queries ======================= */

static int timeline_length(const App *app) {
    return app->chain.count + app->pending.count;
}

static const Block *timeline_at(const App *app, int i) {
    return i < app->chain.count ? &app->chain.blocks[i]
                                : &app->pending.items[i - app->chain.count].block;
}

const Block *lending_active_loan(const App *app, const char *book_id, int *pending) {
    for (int i = timeline_length(app) - 1; i >= 0; i--) {
        const Block *b = timeline_at(app, i);
        if (strcmp(b->book_id, book_id) != 0) continue;
        if (strcmp(b->action, ACTION_BORROWED) == 0) {
            if (pending) *pending = i >= app->chain.count;
            return b;
        }
        if (strcmp(b->action, ACTION_RETURNED) == 0) return NULL;
        /* OVERDUE: the loan is still open, keep looking for its BORROWED block */
    }
    return NULL;
}

const Block *lending_latest(const App *app, const char *book_id, int *pending) {
    for (int i = timeline_length(app) - 1; i >= 0; i--) {
        const Block *b = timeline_at(app, i);
        if (strcmp(b->book_id, book_id) == 0) {
            if (pending) *pending = i >= app->chain.count;
            return b;
        }
    }
    return NULL;
}

/* ======================= Actions ======================= */

/* Reads a Book ID and (optionally) a Member ID and looks them up. */
static int prompt_book_and_member(App *app, Book **book, Member **member) {
    char book_id[20], member_id[20];
    if (!prompt_id("Enter Book ID: ", book_id, sizeof(book_id))) return 0;
    *book = registry_find_book(&app->reg, book_id);

    if (member) {
        if (!prompt_id("Enter Member ID: ", member_id, sizeof(member_id))) return 0;
        *member = registry_find_member(&app->reg, member_id);
    }

    if (!*book || (member && !*member)) {
        printf("ERROR: Book or Member not found\n");
        return 0;
    }
    return 1;
}

void lending_borrow(App *app) {
    Book *book;
    Member *member;
    if (!prompt_book_and_member(app, &book, &member)) return;

    int pending = 0;
    const Block *loan = lending_active_loan(app, book->book_id, &pending);
    if (loan) {
        printf("ERROR: Book '%s' is already on loan to %s%s\n", book->title, loan->member_name,
               pending ? " (that borrow is still waiting in the pending pool)" : "");
        return;
    }

    if (!app_ensure_intact(app)) return;
    if (app_submit_block(app, book->book_id, book->title, member->member_id, member->full_name,
                         ACTION_BORROWED, 0, NULL)) {
        printf("SUCCESS: '%s' borrowed by %s. Block signed by %s and added to the PENDING POOL "
               "(%d waiting).\n", book->title, member->full_name, app->session.username,
               app->pending.count);
        printf("         A borrow creates no token transaction. Mine the pending pool to confirm it.\n");
    }
}

void lending_return(App *app) {
    Book *book;
    Member *member;
    if (!prompt_book_and_member(app, &book, &member)) return;

    const Block *loan = lending_active_loan(app, book->book_id, NULL);
    if (!loan) {
        printf("ERROR: No active loan found for book '%s' (never borrowed, or already returned)\n",
               book->title);
        return;
    }
    if (strcmp(loan->member_id, member->member_id) != 0) {
        printf("ERROR: '%s' is on loan to %s (%s), not %s (%s)\n", book->title,
               loan->member_name, loan->member_id, member->full_name, member->member_id);
        return;
    }

    /* A return is late if the loan period has passed, or if a librarian
     * already marked the loan OVERDUE. */
    const Block *latest = lending_latest(app, book->book_id, NULL);
    int marked_overdue = latest && strcmp(latest->action, ACTION_OVERDUE) == 0;
    long days = (long)((time(NULL) - loan->timestamp) / 86400);
    int late = marked_overdue || days > LOAN_PERIOD_DAYS;
    int reward = late ? REWARD_LATE_COINS : REWARD_ON_TIME_COINS;

    if (!app_ensure_intact(app)) return;

    /* The return automatically creates the reward transaction: the library
     * treasury pays `reward` coins, of which the fixed fee goes to the miner
     * and the rest is credited to the member. */
    char memo[TX_MEMO_MAX];
    snprintf(memo, sizeof(memo), "%s return reward %s", late ? "late" : "on-time", book->book_id);
    Transaction tx;
    if (!tx_init(&tx, TX_REWARD, LIBRARY_ACCOUNT, member->member_id, (Amount)reward * COIN - TX_FEE,
                 TX_FEE, ledger_next_nonce(&app->ledger, LIBRARY_ACCOUNT), memo)) {
        printf("ERROR: Could not create the reward transaction\n");
        return;
    }

    char err[256];
    if (!ledger_admit(&app->ledger, &tx, err, sizeof(err))) {
        printf("ERROR: Reward transaction rejected: %s\n", err);
        return;
    }
    if (!app_submit_block(app, book->book_id, book->title, member->member_id, member->full_name,
                          ACTION_RETURNED, reward, &tx)) {
        ledger_release(&app->ledger, &tx);
        return;
    }

    printf("SUCCESS: '%s' returned by %s %s (%ld day(s) on loan, loan period %d days%s).\n",
           book->title, member->full_name, late ? "LATE" : "ON TIME", days, LOAN_PERIOD_DAYS,
           marked_overdue ? ", loan was marked OVERDUE" : "");
    printf("Reward transaction %.16s... created automatically:\n", tx.txid);
    printf("  token reward %d coins from %s = %s to %s + %s fee for the miner\n", reward,
           LIBRARY_ACCOUNT, fmt_amount(tx.body.amount), member->member_id, fmt_amount(tx.body.fee));
    ledger_describe_admitted(&app->ledger, &tx);
    printf("Block added to the PENDING POOL (%d waiting). %s's balance changes only after it is mined.\n",
           app->pending.count, member->full_name);
}

void lending_overdue(App *app) {
    Book *book;
    if (!prompt_book_and_member(app, &book, NULL)) return;

    const Block *loan = lending_active_loan(app, book->book_id, NULL);
    if (!loan) {
        printf("ERROR: Book '%s' is not currently on loan\n", book->title);
        return;
    }
    const Block *latest = lending_latest(app, book->book_id, NULL);
    if (latest && strcmp(latest->action, ACTION_OVERDUE) == 0) {
        printf("ERROR: Book '%s' is already marked OVERDUE\n", book->title);
        return;
    }

    /* Copy what we need: `loan` may point into the pending pool, which can
     * move in memory when a block is added. */
    char member_id[20], member_name[50];
    snprintf(member_id, sizeof(member_id), "%s", loan->member_id);
    snprintf(member_name, sizeof(member_name), "%s", loan->member_name);
    long days = (long)((time(NULL) - loan->timestamp) / 86400);
    printf("'%s' has been on loan to %s for %ld day(s) (loan period: %d days).\n",
           book->title, member_name, days, LOAN_PERIOD_DAYS);

    if (!app_ensure_intact(app)) return;
    if (app_submit_block(app, book->book_id, book->title, member_id, member_name,
                         ACTION_OVERDUE, 0, NULL)) {
        printf("SUCCESS: '%s' marked OVERDUE for %s. Block added to the PENDING POOL (%d waiting).\n",
               book->title, member_name, app->pending.count);
        printf("         No token transaction created: the book has not been returned (token reward 0).\n");
    }
}

/* ======================= Listings ======================= */

static void describe_book_status(const App *app, const Book *book, char *out, size_t size,
                                 int *available) {
    int latest_pending = 0;
    const Block *latest = lending_latest(app, book->book_id, &latest_pending);
    const Block *loan = lending_active_loan(app, book->book_id, NULL);
    const char *suffix = latest_pending ? " [pending]" : "";

    *available = loan == NULL;
    if (!loan) {
        snprintf(out, size, "Available%s", suffix);
        return;
    }
    int overdue = latest && strcmp(latest->action, ACTION_OVERDUE) == 0;
    snprintf(out, size, "%s: %s (%s)%s", overdue ? "OVERDUE" : "On loan",
             loan->member_name, loan->member_id, suffix);
}

void lending_list_books(App *app) {
    int available_count = 0;

    printf("\n%-8s %-28s %-26s %s\n", "Book ID", "Title", "Author", "Status");
    for (int i = 0; i < 100; i++) putchar('-');
    putchar('\n');

    for (int i = 0; i < app->reg.book_count; i++) {
        const Book *b = &app->reg.books[i];
        char status[112];
        int available;
        describe_book_status(app, b, status, sizeof(status), &available);
        available_count += available;
        printf("%-8s %-28.28s %-26.26s %s\n", b->book_id, b->title, b->author, status);
    }
    printf("\n%d book(s) in total, %d available, %d on loan. [pending] = latest action not "
           "mined yet.\n", app->reg.book_count, available_count,
           app->reg.book_count - available_count);
}

void lending_list_members(App *app) {
    printf("\n%-9s %-22s %-7s %10s  %s\n", "Member ID", "Full Name", "Course", "Balance",
           "Books on loan");
    for (int i = 0; i < 100; i++) putchar('-');
    putchar('\n');

    for (int i = 0; i < app->reg.member_count; i++) {
        const Member *m = &app->reg.members[i];
        char loans[256] = "";
        size_t used = 0;

        for (int j = 0; j < app->reg.book_count; j++) {
            const Block *loan = lending_active_loan(app, app->reg.books[j].book_id, NULL);
            if (!loan || strcmp(loan->member_id, m->member_id) != 0) continue;

            const Block *latest = lending_latest(app, app->reg.books[j].book_id, NULL);
            int overdue = latest && strcmp(latest->action, ACTION_OVERDUE) == 0;
            int n = snprintf(loans + used, sizeof(loans) - used, "%s%s%s", used ? ", " : "",
                             app->reg.books[j].book_id, overdue ? " (OVERDUE)" : "");
            if (n < 0 || (size_t)n >= sizeof(loans) - used) break;
            used += (size_t)n;
        }

        printf("%-9s %-22.22s %-7s %10s  %s\n", m->member_id, m->full_name, m->course_code,
               fmt_amount(ledger_balance(&app->ledger, m->member_id)), used ? loans : "-");
    }
    printf("\n%d member(s) in total. Balances are confirmed tokens (%s model).\n",
           app->reg.member_count, ledger_model_name(app->ledger.model));
}
