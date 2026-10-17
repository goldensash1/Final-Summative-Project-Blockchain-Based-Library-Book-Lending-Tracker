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
#include "fraud.h"
#include "merkle.h"

/* ======================= Command-line options ======================= */

typedef struct {
    int model;          /* MODEL_UTXO / MODEL_ACCOUNT, or -1 to ask at startup */
    int difficulty;     /* 0 = use the saved value */
    int batch;          /* 0 = use the saved value */
    int seed_set;
    unsigned int seed;
} Options;

static void print_usage(const char *prog) {
    printf("Usage: %s [--model utxo|account] [--difficulty 1-4] [--batch 1-%d] [--seed N]\n\n",
           prog, MAX_RECORDS_PER_BLOCK);
    printf("  -m, --model       token model for the whole session (asked at startup if omitted)\n");
    printf("  -d, --difficulty  proof-of-work difficulty = leading zero hex digits (default %d)\n",
           DEFAULT_DIFFICULTY);
    printf("  -b, --batch       lending records per block (default %d)\n", DEFAULT_BATCH_SIZE);
    printf("  -s, --seed        random seed for the pool miners' hash rates (default: time)\n");
    printf("  -h, --help        show this help\n");
    printf("Commands are read one per line from standard input; type 'help' once running.\n");
}

/* Matches "-x value", "--long value" or "--long=value". */
static const char *flag_value(int argc, char **argv, int *i, const char *short_name,
                              const char *long_name) {
    const char *arg = argv[*i];
    size_t long_len = strlen(long_name);
    if (strncmp(arg, long_name, long_len) == 0 && arg[long_len] == '=') return arg + long_len + 1;
    if (strcmp(arg, short_name) == 0 || strcmp(arg, long_name) == 0) {
        return *i + 1 < argc ? argv[++*i] : "";
    }
    return NULL;
}

/* Returns 1 to run, 0 to exit successfully (--help), -1 on a bad argument. */
static int parse_args(int argc, char **argv, Options *opt) {
    memset(opt, 0, sizeof(*opt));
    opt->model = -1;
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
                printf("ERROR: --difficulty must be a number from %d to %d\n", MIN_DIFFICULTY, MAX_DIFFICULTY);
                return -1;
            }
            opt->difficulty = n;
        } else if ((value = flag_value(argc, argv, &i, "-b", "--batch")) != NULL) {
            if (!parse_int(value, &n) || n < 1 || n > MAX_RECORDS_PER_BLOCK) {
                printf("ERROR: --batch must be a number from 1 to %d\n", MAX_RECORDS_PER_BLOCK);
                return -1;
            }
            opt->batch = n;
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

static int choose_model_interactively(void) {
    printf("\nSelect the token model for this session:\n");
    printf("  1. UTXO model (unspent transaction outputs, as in Bitcoin)\n");
    printf("  2. Account-balance model (balances and nonces, as in Ethereum)\n");
    int choice;
    if (!prompt_int_range("Choice [1]: ", 1, 2, 1, &choice)) return -1;
    return choice == 1 ? MODEL_UTXO : MODEL_ACCOUNT;
}

/* ======================= Chain commands ======================= */

static void cmd_chain_view(App *app, int argc, char **argv) {
    const char *arg = cli_positional(argc, argv, 1);
    int index;
    if (arg) {
        if (!parse_int(arg, &index)) {
            printf("ERROR: Block index must be a number\n");
            return;
        }
        chain_print_block(&app->chain, index);
        return;
    }
    chain_print_summary(&app->chain);
}

static void cmd_validate_chain(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("Validating %d block(s) and %d ledger transaction(s)...\n", app->chain.count,
           app->ledger.log_count);
    int was_locked = app->locked;
    app_validate(app, 1, NULL);
    if (was_locked && !app->locked) printf("Session unlocked.\n");
    if (app->locked) printf("Session locked: run chain_load to reload the saved chain from disk.\n");
}

static void cmd_chain_save(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!app_require_unlocked(app)) return;
    if (app_validate(app, 0, NULL) != 0) return;
    if (app_save_all(app)) {
        printf("Saved: %s (%d blocks), %s (%d requests), %s (%d txs), %s, %s, %s.\n", CHAIN_FILE,
               app->chain.count, PENDING_FILE, app->pending.count, LEDGER_FILE, app->ledger.log_count,
               STATE_FILE, BALANCES_FILE, META_FILE);
    }
}

static void cmd_chain_load(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!app_reload(app)) {
        printf("FATAL: Could not reload the saved state.\n");
        exit(1);
    }
    printf("Chain reloaded: %d block(s), %d pending request(s)%s.\n", app->chain.count,
           app->pending.count, app->locked ? " -- STILL INVALID, session locked" : "");
}

/* Tamper demo: changes the chain in memory only; validate_chain then shows
 * which check catches it, and the session locks until chain_load. */
static void cmd_tamper_block(App *app, int argc, char **argv) {
    if (!app_require_admin(app)) return;
    if (app->chain.count < 2) {
        printf("ERROR: Mine at least one block with records first.\n");
        return;
    }
    char buf[16], mode[16];
    int index;
    if (!cli_arg(argc, argv, 1, "Block index to tamper with: ", buf, sizeof(buf)) ||
        !parse_int(buf, &index) || index < 1 || index >= app->chain.count) {
        printf("ERROR: Block index must be 1-%d\n", app->chain.count - 1);
        return;
    }
    printf("Modes: record  = change a record's due date (hash untouched -> Merkle root & signature fail)\n"
           "       header  = change the block's token_reward (-> block hash fails)\n"
           "       remine  = change a record, recompute Merkle root and re-mine (-> signature & next link fail)\n");
    if (!cli_arg(argc, argv, 2, "Mode (record/header/remine): ", mode, sizeof(mode))) return;

    Block *b = &app->chain.blocks[index];
    if (strcmp(mode, "record") == 0) {
        b->records[0].due_date += 30 * SECONDS_PER_DAY;
        printf("Block #%d record %s: due_date silently moved 30 days later (nothing recomputed).\n",
               index, b->records[0].record_id);
    } else if (strcmp(mode, "header") == 0) {
        b->token_reward += 100;
        printf("Block #%d: token_reward changed to %d without recomputing the hash.\n", index, b->token_reward);
    } else if (strcmp(mode, "remine") == 0) {
        b->records[0].fine_amount = 0;
        b->records[0].due_date += 30 * SECONDS_PER_DAY;
        merkle_root(b->records, b->record_count, b->merkle_root);
        char prev[SHA256_HEX_LEN + 1], miner[MINER_NAME_MAX];
        snprintf(prev, sizeof(prev), "%s", b->previous_hash);
        snprintf(miner, sizeof(miner), "%s", b->sealed_by);
        unsigned long long attempts = pow_mine(b, b->index, prev, b->difficulty, miner);
        printf("Block #%d record %s changed, Merkle root recomputed and block re-mined in %llu attempts.\n",
               index, b->records[0].record_id, attempts);
    } else {
        printf("ERROR: Unknown mode '%s'\n", mode);
        return;
    }
    app->locked = 1;
    printf("The chain in memory is now tampered (nothing was saved). Run validate_chain to see it\n"
           "detected; the session is locked until chain_load restores the saved chain.\n");
}

static void cmd_add_user(App *app, int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!app_require_admin(app)) return;
    int role;
    if (!prompt_int_range("Role for new account (1 = LIBRARIAN, 2 = ADMIN): ", 1, 2, 0, &role)) return;
    auth_create_user_interactive(USERS_FILE, role == 2 ? ROLE_ADMIN : ROLE_LIBRARIAN);
}

/* ======================= Command table ======================= */

typedef void (*CommandFn)(App *app, int argc, char **argv);

#define CMD_LOCKED_OK 1   /* allowed while the session is locked */

typedef struct {
    const char *group;
    const char *name;
    const char *usage;
    const char *help;
    CommandFn fn;
    int flags;
} Command;

static void cmd_help(App *app, int argc, char **argv);

static const Command COMMANDS[] = {
    {"Membership", "register_member", "<id> \"<name>\" <course>", "register a member (generates their ECDSA keys)", cmd_register_member, 0},
    {"Membership", "view_member", "<member_id>", "member details, loans, fines, nonce, balance", cmd_view_member, CMD_LOCKED_OK},
    {"Membership", "member_nonce", "<member_id>", "request nonce (replay protection)", cmd_member_nonce, CMD_LOCKED_OK},
    {"Membership", "list_members", "", "all members", cmd_list_members, CMD_LOCKED_OK},
    {"Books & copies", "register_book", "<id> \"<title>\" \"<author>\" [copies=N]", "register a title", cmd_register_book, 0},
    {"Books & copies", "view_book", "<book_id>", "title, copies and reservation queue", cmd_view_book, CMD_LOCKED_OK},
    {"Books & copies", "add_copy", "<book_id> [NEW|GOOD|WORN]", "add a physical copy", cmd_add_copy, 0},
    {"Books & copies", "view_copy", "<copy_id>", "copy state and history", cmd_view_copy, CMD_LOCKED_OK},
    {"Books & copies", "mark_lost", "<copy_id> <reason_code>", "librarian override: copy -> LOST", cmd_mark_lost, 0},
    {"Books & copies", "list_books", "", "all titles with copy counts", cmd_list_books, CMD_LOCKED_OK},
    {"Lending", "borrow_book", "<member_id> <book_id|copy_id> [due=YYYY-MM-DD] [nonce=N]", "borrow an available copy", cmd_borrow_book, 0},
    {"Lending", "return_book", "<copy_id> [NEW|GOOD|WORN] [reason=CODE]", "return a copy (reward 10 on time / 5 late)", cmd_return_book, 0},
    {"Lending", "renew_book", "<copy_id>", "extend the due date by 7 days (max 2)", cmd_renew_book, 0},
    {"Lending", "reserve_book", "<member_id> <book_id>", "join the queue when no copy is available", cmd_reserve_book, 0},
    {"Lending", "reservation_status", "[book_id]", "reservation queues and held copies", cmd_reservation_status, CMD_LOCKED_OK},
    {"Fines & overdue", "check_overdue", "", "flag overdue copies and list accrued fines", cmd_check_overdue, 0},
    {"Fines & overdue", "view_fines", "[member_id]", "fines owed, paid and accruing", cmd_view_fines, CMD_LOCKED_OK},
    {"Fines & overdue", "settle_fine", "<member_id> [amount] [method=tokens|cash]", "pay a fine", cmd_settle_fine, 0},
    {"Fines & overdue", "advance_days", "<n>", "move the simulated library clock forward [ADMIN]", cmd_advance_days, 0},
    {"Tokens & ledger", "token_transfer", "<from> <to> <amount>", "transfer tokens (session model)", cmd_token_transfer, 0},
    {"Tokens & ledger", "token_balance", "[account]", "balance of one account (or all)", cmd_token_balance, CMD_LOCKED_OK},
    {"Tokens & ledger", "utxo_view", "[owner]", "the UTXO set (UTXO model)", cmd_utxo_view, CMD_LOCKED_OK},
    {"Tokens & ledger", "account_balance", "[account]", "account balances (account model)", cmd_account_balance, CMD_LOCKED_OK},
    {"Tokens & ledger", "account_transfer", "<from> <to> <amount> [nonce]", "transfer with an explicit nonce (account model)", cmd_account_transfer, 0},
    {"Tokens & ledger", "account_nonce", "<account>", "account transaction nonce", cmd_account_nonce, CMD_LOCKED_OK},
    {"Tokens & ledger", "transaction_history", "<member_id>", "an account's transaction history", cmd_transaction_history, CMD_LOCKED_OK},
    {"Tokens & ledger", "replay_tx", "<txid-prefix>", "replay a confirmed tx (double-spend / nonce test)", cmd_replay_tx, 0},
    {"Chain & mining", "pool_view", "", "pending pool in priority order", cmd_pool_view, CMD_LOCKED_OK},
    {"Chain & mining", "mine_solo", "", "mine all PENDING requests alone", cmd_mine_solo, 0},
    {"Chain & mining", "mine_pool", "[miners 2-8]", "pool mining with reward-sharing table", cmd_mine_pool, 0},
    {"Chain & mining", "mine_cloud", "<rounds 1-5>", "rent hash power for 1-5 rounds", cmd_mine_cloud, 0},
    {"Chain & mining", "chain_view", "[index]", "all blocks, or one block with records + Merkle tree", cmd_chain_view, CMD_LOCKED_OK},
    {"Chain & mining", "validate_chain", "", "full chain verification and tamper detection", cmd_validate_chain, CMD_LOCKED_OK},
    {"Chain & mining", "chain_save", "", "save all state to disk now", cmd_chain_save, 0},
    {"Chain & mining", "chain_load", "", "reload all state from disk and re-validate", cmd_chain_load, CMD_LOCKED_OK},
    {"Chain & mining", "difficulty_status", "", "difficulty, target and expected attempts", cmd_difficulty_status, CMD_LOCKED_OK},
    {"Chain & mining", "set_difficulty", "<1-4>", "change the difficulty [ADMIN]", cmd_set_difficulty, 0},
    {"Chain & mining", "benchmark", "", "average attempts at difficulty 1-4", cmd_benchmark, CMD_LOCKED_OK},
    {"Fraud & audit", "fraud_review", "", "list SUSPICIOUS requests", cmd_fraud_review, CMD_LOCKED_OK},
    {"Fraud & audit", "approve_suspicious", "<id>", "approve a flagged request for mining", cmd_approve_suspicious, 0},
    {"Fraud & audit", "reject_suspicious", "<id>", "discard a flagged request", cmd_reject_suspicious, 0},
    {"Fraud & audit", "replay_request", "<id>", "replay a captured request (duplicate test)", cmd_replay_request, 0},
    {"Fraud & audit", "lending_history", "", "every lending record", cmd_lending_history, CMD_LOCKED_OK},
    {"Fraud & audit", "member_history", "<member_id>", "records of one member", cmd_member_history, CMD_LOCKED_OK},
    {"Fraud & audit", "book_history", "<book_id|copy_id>", "records of one title or copy", cmd_book_history, CMD_LOCKED_OK},
    {"Fraud & audit", "tamper_block", "<index> <record|header|remine>", "tamper demo, in memory [ADMIN]", cmd_tamper_block, CMD_LOCKED_OK},
    {"Session", "add_user", "", "create a librarian/admin account [ADMIN]", cmd_add_user, 0},
    {"Session", "help", "", "this list", cmd_help, CMD_LOCKED_OK},
    {"Session", "exit", "", "save and quit (also: quit)", NULL, CMD_LOCKED_OK},
};

#define COMMAND_COUNT ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

static void cmd_help(App *app, int argc, char **argv) {
    (void)app;
    (void)argc;
    (void)argv;
    const char *group = "";
    for (int i = 0; i < COMMAND_COUNT; i++) {
        const Command *c = &COMMANDS[i];
        if (strcmp(group, c->group) != 0) {
            group = c->group;
            printf("\n -- %s --\n", group);
        }
        char left[96];
        snprintf(left, sizeof(left), "%s %s", c->name, c->usage);
        printf("  %-62s %s\n", left, c->help);
    }
    printf("\nArguments left out are asked for. Quote names with spaces: register_member ALU006 \"Jane Doe\" BLK101\n");
}

static void print_status(const App *app) {
    char now[24];
    format_datetime(app_now(app), now, sizeof(now));
    int suspicious = 0;
    for (int i = 0; i < app->pending.count; i++) suspicious += app->pending.items[i].status == REQ_SUSPICIOUS;
    printf("\n[%s | %s | %s | difficulty %d | %d block(s) | pool %d (%d suspicious)%s]\n",
           app->session.username, ledger_model_name(app->ledger.model), now, app->meta.difficulty,
           app->chain.count, app->pending.count, suspicious, app->locked ? " | LOCKED" : "");
}

/* ======================= Main loop ======================= */

int main(int argc, char **argv) {
    Options opt;
    int parsed = parse_args(argc, argv, &opt);
    if (parsed <= 0) return parsed == 0 ? 0 : 1;
    srand(opt.seed_set ? opt.seed : (unsigned int)time(NULL));

    static App app;   /* large (registry arrays): keep it off the stack */
    memset(&app, 0, sizeof(app));

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

    if (!app_open(&app, (LedgerModel)model, opt.difficulty, opt.batch)) {
        printf("FATAL: Could not load the chain, ledger or copy inventory. Exiting.\n");
        app_close(&app);
        auth_logout(&app.session);
        crypto_cleanup();
        return 1;
    }
    printf("Token model for this session: %s. Difficulty: %d. Block size: %d record(s).\n",
           ledger_model_name(app.ledger.model), app.meta.difficulty, app.meta.batch_size);
    if (app.locked) {
        printf("SESSION LOCKED: the saved chain failed validate_chain. Only viewing, validate_chain and\n"
               "chain_load are allowed until valid files are restored.\n");
    }
    printf("Type 'help' for the list of commands.\n");

    for (;;) {
        /* Overdue detection runs on every CLI invocation. */
        app_overdue_scan(&app, 0);
        print_status(&app);

        char line[512];
        int rc = read_line("> ", line, sizeof(line));
        if (rc == INPUT_EOF) break;
        if (rc != INPUT_OK) continue;

        char *words[CLI_MAX_ARGS];
        int count = tokenize(line, words, CLI_MAX_ARGS);
        if (count == 0) continue;
        if (strcmp(words[0], "exit") == 0 || strcmp(words[0], "quit") == 0) break;

        const Command *cmd = NULL;
        for (int i = 0; i < COMMAND_COUNT && !cmd; i++) {
            if (strcmp(words[0], COMMANDS[i].name) == 0) cmd = &COMMANDS[i];
        }
        if (!cmd || !cmd->fn) {
            printf("Unknown command '%s'. Type 'help' for the list.\n", words[0]);
            continue;
        }
        if (app.locked && !(cmd->flags & CMD_LOCKED_OK)) {
            app_require_unlocked(&app);
            continue;
        }
        cmd->fn(&app, count, words);
        if (input_eof()) break;
    }

    if (!app.locked && app_save_all(&app)) printf("State saved. Goodbye.\n");
    else printf("Nothing saved (session locked). Goodbye.\n");
    app_close(&app);
    auth_logout(&app.session);
    crypto_cleanup();
    return 0;
}
