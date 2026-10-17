#ifndef LENDING_H
#define LENDING_H

#include "app.h"

/* Library commands. Each one that changes something creates a signed request
 * and submits it to the pending pool (app_submit); nothing reaches the chain
 * or changes a balance until it is mined. Member actions are signed with the
 * member's own private key, librarian overrides with the librarian's. */

/* Membership */
void cmd_register_member(App *app, int argc, char **argv);
void cmd_view_member(App *app, int argc, char **argv);
void cmd_member_nonce(App *app, int argc, char **argv);
void cmd_list_members(App *app, int argc, char **argv);

/* Books and copies */
void cmd_register_book(App *app, int argc, char **argv);
void cmd_view_book(App *app, int argc, char **argv);
void cmd_add_copy(App *app, int argc, char **argv);
void cmd_view_copy(App *app, int argc, char **argv);
void cmd_mark_lost(App *app, int argc, char **argv);
void cmd_list_books(App *app, int argc, char **argv);

/* Lending */
void cmd_borrow_book(App *app, int argc, char **argv);
void cmd_return_book(App *app, int argc, char **argv);
void cmd_renew_book(App *app, int argc, char **argv);
void cmd_reserve_book(App *app, int argc, char **argv);
void cmd_reservation_status(App *app, int argc, char **argv);

/* Fines and overdue */
void cmd_check_overdue(App *app, int argc, char **argv);
void cmd_view_fines(App *app, int argc, char **argv);
void cmd_settle_fine(App *app, int argc, char **argv);
void cmd_advance_days(App *app, int argc, char **argv);

/* Audit */
void cmd_lending_history(App *app, int argc, char **argv);
void cmd_member_history(App *app, int argc, char **argv);
void cmd_book_history(App *app, int argc, char **argv);

#endif
