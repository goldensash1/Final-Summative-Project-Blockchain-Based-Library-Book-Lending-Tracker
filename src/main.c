#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "app.h"
#include "input.h"
#include "lending.h"
#include "tokens.h"
#include "mining.h"

/* Command-line options. */
typedef struct {
    int model;          /* MODEL_UTXO / MODEL_ACCOUNT, or -1 to ask at startup */
    int difficulty;
    int seed_set;
    unsigned int seed;
} Options;

static void print_usage(const char *prog) {
    printf("Usage: %s [--model utxo|account] [--difficulty 1-4] [--seed N]\n\n", prog);
    printf("  -m, --model       transaction model for the whole session (asked at startup if omitted)\n");
    printf("  -d, --difficulty  proof-of-work difficulty = leading zero hex digits (default %d)\n",
           DEFAULT_DIFFICULTY);
    printf("  -s, --seed        random seed for the pool miners' hash rates (default: time)\n");
    printf("  -h, --help        show this help\n");
}

/* Matches "-x value", "--long value" or "--long=value". Returns the value,
 * "" if the value is missing, or NULL if `argv[*i]` is a different flag. */
static const char *flag_value(int argc, char **argv, int *i, const char *short_name,
                              const char *long_name) {
    const char *arg = argv[*i];
    if (!arg) return NULL;
    size_t long_len = strlen(long_name);
    if (strncmp(arg, long_name, long_len) == 0 && arg[long_len] == '=') return arg + long_len + 1;
    if (strcmp(arg, short_name) == 0 || strcmp(arg, long_name) == 0) {
        return *i + 1 < argc ? argv[++*i] : "";
    }
    return NULL;
}

/* Returns 1 to run, 0 to exit successfully (--help), -1 on a bad argument. */
static int parse_args(int argc, char **argv, Options *opt) {
    opt->model = -1;
    opt->difficulty = DEFAULT_DIFFICULTY;
    opt->seed_set = 0;
    opt->seed = 0;

    for (int i = 1; i < argc; i++) {
        const char *value;
        int n;

        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if ((value = flag_value(argc, argv, &i, "-m", "--model")) != NULL) {
            if (strcasecmp(value, "utxo") == 0) opt->model = MODEL_UTXO;
            else if (strcasecmp(value, "account") == 0) opt->model = MODEL_ACCOUNT;
            else {
                printf("ERROR: --model must be 'utxo' or 'account'\n");
                return -1;
            }
        } else if ((value = flag_value(argc, argv, &i, "-d", "--difficulty")) != NULL) {
            if (!parse_int(value, &n) || n < MIN_DIFFICULTY || n > MAX_DIFFICULTY) {
                printf("ERROR: --difficulty must be a number from %d to %d\n",
                       MIN_DIFFICULTY, MAX_DIFFICULTY);
                return -1;
            }
            opt->difficulty = n;
        } else if ((value = flag_value(argc, argv, &i, "-s", "--seed")) != NULL) {
            if (!parse_int(value, &n) || n < 0) {
                printf("ERROR: --seed must be a non-negative number\n");
                return -1;
            }
            opt->seed = (unsigned int)n;
            opt->seed_set = 1;
        } else {
            printf("ERROR: Unknown argument '%s'\n\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }
    return 1;
}

/* Returns the chosen model, or -1 if input ended. */
static int choose_model_interactively(void) {
    printf("\nSelect the transaction model for this session:\n");
    printf("  1. UTXO model (unspent transaction outputs, as in Bitcoin)\n");
    printf("  2. Account-balance model (balances and nonces, as in Ethereum)\n");
    int choice;
    if (!prompt_int_range("Choice [1]: ", 1, 2, 1, &choice)) return -1;
    return choice == 1 ? MODEL_UTXO : MODEL_ACCOUNT;
}

static void print_menu(const App *app) {
    const char *balances = app->ledger.model == MODEL_UTXO ? "View Balances & UTXO Set"
                                                           : "View Account Balances";
    printf("\n===== Blockchain Library Lending Tracker =====\n");
    printf("User: %s (%s) | Model: %s | Difficulty: %d | Chain: %d block(s) | Pending: %d\n",
           app->session.username, auth_role_name(app->session.role),
           ledger_model_name(app->ledger.model), app->difficulty, app->chain.count,
           app->pending.count);
    printf(" -- Lending --\n");
    printf("  1. Borrow Book\n");
    printf("  2. Return Book (creates the token reward)\n");
    printf("  3. Mark Book Overdue\n");
    printf("  4. List Books\n");
    printf("  5. List Members\n");
    printf(" -- Pending pool & mining --\n");
    printf("  6. View Pending Pool\n");
    printf("  7. Mine Pending Pool (solo / pool / cloud)\n");
    printf("  8. Set Mining Difficulty [ADMIN]\n");
    printf("  9. Difficulty Benchmark\n");
    printf(" -- Tokens --\n");
    printf(" 10. %s\n", balances);
    printf(" 11. Transfer Tokens\n");
    printf(" 12. Transaction History for an Account\n");
    printf(" 13. Replay a Confirmed Transaction (double-spend / nonce test)\n");
    printf(" -- Chain --\n");
    printf(" 14. View Confirmed Lending Records\n");
    printf(" 15. Validate Chain\n");
    printf(" 16. Tamper With a Block (demo) [ADMIN]\n");
    printf(" 17. Add User Account [ADMIN]\n");
    printf("  0. Exit\n");
}

static void do_tamper(App *app) {
    if (!app_require_admin(app)) return;
    if (app->chain.count < 2) {
        printf("ERROR: Mine at least one lending block first.\n");
        return;
    }

    printf("Tamper demo (changes are made in memory only):\n");
    printf("  1. Change a block's action WITHOUT recomputing its hash\n");
    printf("  2. Change a block's token reward and RE-MINE its hash (attacker with hash power)\n");
    int mode;
    if (!prompt_int_range("Choice: ", 1, 2, 0, &mode)) return;

    char prompt[64];
    snprintf(prompt, sizeof(prompt), "Block index to tamper with (0-%d): ", app->chain.count - 1);
    int index;
    if (!prompt_int_range(prompt, 0, app->chain.count - 1, -1, &index)) return;

    if (mode == 1) {
        char new_action[10];
        if (read_line("New action value to inject (e.g. RETURNED): ", new_action,
                      sizeof(new_action)) != INPUT_OK) return;
        chain_tamper_action(&app->chain, index, new_action);
        printf("Block #%d's action was silently changed to '%s' (hash NOT recomputed).\n",
               index, new_action);
    } else {
        int reward;
        if (!prompt_int_range("New token reward to inject (e.g. 10): ", 0, 1000, -1, &reward)) return;
        unsigned long long attempts = chain_tamper_reward(&app->chain, index, reward);
        printf("Block #%d's token reward changed to %d and its hash RE-MINED after %llu attempts.\n",
               index, reward, attempts);
        printf("Its own hash and proof of work are valid again, but the librarian's signature,\n"
               "its reward transaction and the next block's previous_hash no longer match.\n");
    }
    printf("Run 'Validate Chain' (15) to see the tampering detected. New blocks and mining are\n"
           "refused while the chain is invalid, so it is never saved; restart the program to\n"
           "reload the genuine chain.\n");
}

static void do_add_user(const App *app) {
    if (!app_require_admin(app)) return;

    int role;
    if (!prompt_int_range("Role for new account (1 = LIBRARIAN, 2 = ADMIN): ", 1, 2, 0, &role)) return;
    auth_create_user_interactive(USERS_FILE, role == 2 ? ROLE_ADMIN : ROLE_LIBRARIAN);
}

static void do_validate(App *app) {
    printf("Validating %d block(s) and %d ledger transaction(s)...\n", app->chain.count,
           app->ledger.log_count);
    app_validate(app, 1);
}

int main(int argc, char **argv) {
    Options opt;
    int parsed = parse_args(argc, argv, &opt);
    if (parsed <= 0) return parsed == 0 ? 0 : 1;
    srand(opt.seed_set ? opt.seed : (unsigned int)time(NULL));

    static App app;   /* large (registry arrays): keep it off the stack */
    memset(&app, 0, sizeof(app));
    app.difficulty = opt.difficulty;

    printf("Loading registries...\n");
    if (!registry_load(&app.reg, BOOKS_FILE, MEMBERS_FILE)) {
        printf("FATAL: Could not load book/member registries. Exiting.\n");
        return 1;
    }
    printf("Loaded %d books and %d members.\n", app.reg.book_count, app.reg.member_count);

    if (!auth_has_users(USERS_FILE) && !auth_first_run_setup(USERS_FILE)) {
        printf("FATAL: Administrator account was not created. Exiting.\n");
        return 1;
    }
    if (!auth_login(USERS_FILE, &app.session)) {
        printf("FATAL: Authentication failed. Exiting.\n");
        return 1;
    }

    int model = opt.model >= 0 ? opt.model : choose_model_interactively();
    if (model < 0) {
        auth_logout(&app.session);
        return 1;
    }
    printf("Transaction model for this session: %s. Mining difficulty: %d.\n",
           ledger_model_name((LedgerModel)model), app.difficulty);

    if (!app_open(&app, (LedgerModel)model)) {
        printf("FATAL: Could not load the chain and ledger. Exiting.\n");
        app_close(&app);
        auth_logout(&app.session);
        crypto_cleanup();
        return 1;
    }

    printf("Verifying chain integrity on startup...\n");
    app_validate(&app, 1);

    int running = 1;
    while (running) {
        print_menu(&app);
        char choice[16];
        int rc = read_line("Choose an option: ", choice, sizeof(choice));
        if (rc == INPUT_EOF) break;

        int opt_num = -1;
        if (rc == INPUT_OK && !parse_int(choice, &opt_num)) opt_num = -1;

        switch (opt_num) {
            case 1: lending_borrow(&app); break;
            case 2: lending_return(&app); break;
            case 3: lending_overdue(&app); break;
            case 4: lending_list_books(&app); break;
            case 5: lending_list_members(&app); break;
            case 6: pending_print(&app.pending, app.difficulty); break;
            case 7: mining_menu(&app); break;
            case 8: mining_set_difficulty(&app); break;
            case 9: mining_benchmark(); break;
            case 10: tokens_view_balances(&app); break;
            case 11: tokens_transfer(&app); break;
            case 12: tokens_history(&app); break;
            case 13: tokens_replay_test(&app); break;
            case 14: chain_print_records(&app.chain); break;
            case 15: do_validate(&app); break;
            case 16: do_tamper(&app); break;
            case 17: do_add_user(&app); break;
            case 0: running = 0; break;
            default: printf("Invalid option, try again.\n"); break;
        }
        if (input_eof()) break;
    }

    printf("Goodbye.\n");
    app_close(&app);
    auth_logout(&app.session);
    crypto_cleanup();
    return 0;
}
