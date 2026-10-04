#ifndef LENDING_H
#define LENDING_H

#include "app.h"

/* Lending actions. Each one signs a new block and puts it in the pending
 * pool; nothing reaches the chain (or changes a balance) until it is mined.
 * A return also creates the member's token reward transaction:
 *   on time -> 10 coins, late -> 5 coins (each minus the 1-coin fee). */
void lending_borrow(App *app);
void lending_return(App *app);
void lending_overdue(App *app);   /* not returned: no transaction is created */

void lending_list_books(App *app);
void lending_list_members(App *app);

/* The confirmed chain followed by the pending pool is one timeline: a book
 * borrowed in a pending block is already unavailable.
 * Returns the BORROWED block of the book's open loan (NULL if none), and
 * sets *pending to 1 if that block is not mined yet. */
const Block *lending_active_loan(const App *app, const char *book_id, int *pending);

/* Most recent block about the book (confirmed or pending), or NULL. */
const Block *lending_latest(const App *app, const char *book_id, int *pending);

#endif
