# Blockchain-Based Library Book Lending Tracker: Summative

A command-line program in C that keeps an ALU library's lending records on a small proof-of-work
blockchain. It combines Formative 1 (signed lending records, book and member registries, SHA-256 hash
chain, validation) and Formative 2 (token rewards for returns, a pending pool, mining, UTXO and account
models, solo/pool/cloud mining), and adds the summative features:

- **Multi-record blocks with a Merkle root.** A block holds up to 3 lending records (configurable 1-8).
  The Merkle tree is built from scratch and checked on every validation.
- **Copy-level inventory** loaded from `copies.txt`, with an availability state machine
  (AVAILABLE, BORROWED, RESERVED, LOST).
- **Reservations, renewals, overdue detection and fines.** Each title has a FIFO reservation queue, and a
  returned copy is offered to the first member in the queue. Renewals add 7 days (at most 2 per loan).
  Overdue copies are flagged automatically and fined 1 coin per day.
- **Real ECDSA signatures for every member.** Each member has their own P-256 key pair and signs their own
  requests. The public key is stored in every record as the signer's on-chain identity.
- **Nonce replay protection.** Every signer has a request counter. A request with any nonce other than the
  next one is rejected.
- **Prioritised pending pool.** Overdue-related requests come first, then whoever has waited longest.
- **Fraud detection** with a manual review workflow (`fraud_review`, `approve_suspicious`, `reject_suspicious`).
- **Disk persistence** of the chain, pool, ledger, inventory, queues, nonces, fines, balances and
  settings. A saved chain must pass `validate_chain` before any other operation is allowed.
- **A named-command CLI** (`borrow_book`, `mine_solo`, `validate_chain`, ...).

## Dependencies

- A C compiler (`gcc` or `clang`) and `make`
- **OpenSSL 3.x** (headers and libcrypto). It provides SHA-256, ECDSA (P-256), PBKDF2, AES key encryption
  and secure random numbers.
  - macOS: `brew install openssl@3`
  - Debian/Ubuntu: `sudo apt-get install build-essential libssl-dev`
- `python3` is only used by `make test` (to edit a file for the tamper test).

The `Makefile` looks for OpenSSL in `/opt/homebrew/opt/openssl@3` and `/usr/local/opt/openssl@3`; override
with `make OPENSSL_PREFIX=/path/to/openssl`. If the only OpenSSL found is an Intel build on an Apple
Silicon Mac, it builds an x86_64 binary that runs under Rosetta 2.

## Compile

```bash
make            # builds ./library_tracker from every file in src/ with -Wall -Wextra
make test       # 78 automated end-to-end checks in a temporary folder
make clean      # removes the binary and all saved chain state (keeps accounts and keys)
make distclean  # full reset: also removes users.dat and keys/
make dist       # creates Summative_Golden_Munyankindi.zip
```

By hand (macOS example):

```bash
gcc -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -I$(brew --prefix openssl@3)/include \
    -o library_tracker src/*.c -L$(brew --prefix openssl@3)/lib -lssl -lcrypto
```

The code compiles with no warnings. Run the program from the project folder so it finds `books.txt`,
`members.txt` and `copies.txt`. Chain files from older versions of the program have a different format and are refused:
run `make clean` first.

## Run

```
./library_tracker [--model utxo|account] [--difficulty 1-4] [--batch 1-8] [--seed N]
```

On the first run you create the admin account. The program then generates a key pair for every member
and mines the genesis block, which funds the `LIBRARY` reward treasury with 10,000 coins. Commands are
typed one per line. Type `help` for the full list. Missing arguments are asked for, and names with spaces
go in quotes: `register_member ALU006 "Jane Doe" BLK103`.

### Switching between the UTXO and account-based models

The model is chosen for the whole session with `--model utxo` or `--model account` (or from a menu at
startup if the option is left out). To switch, type `exit` and start again with the other option. Both
models are rebuilt from the same confirmed transaction log (`ledger.dat`), so the balances are the same in
both; only how they are stored and checked changes. `make run-utxo` and `make run-account` are shortcuts.

### Setting the mining difficulty

- At startup: `--difficulty N` with N from 1 to 4 (hash must start with N zeros). Other values are refused.
- During a session (ADMIN): `set_difficulty N`. `difficulty_status` shows the target and expected attempts.
- The difficulty is saved in `meta.dat`. Each block stores the difficulty it was mined at, so old blocks
  still validate after a change.

### How to test the mining simulations

1. Create some pending requests, for example `borrow_book ALU001 BK001`, `borrow_book ALU002 BK002`,
   `borrow_book ALU003 BK004`, `borrow_book ALU004 BK005`. `pool_view` shows them in priority order.
2. **Solo:** `mine_solo`. For each block it prints the records, the nonce, the number of hash attempts, the
   hash and the UTXO set (or updated accounts). The miner gets 50 coins per block plus fees.
3. **Pool:** `mine_pool 4`. Four miners get random hash rates. The table shows miner ID, attempts, share
   and reward after the 2% pool fee. Reward share = miner_attempts / total_attempts x (block rewards + fees - pool fee).
4. **Cloud:** `mine_cloud 3` rents 400 attempts per round for 3 rounds at 20 coins per round. The summary
   shows gross earnings, fees and net profit. A warning is printed in any round where the total fees are
   higher than the total rewards (try `mine_cloud 5` with only one pending request).
5. **Difficulty:** `benchmark` mines sample blocks at difficulties 1-4 (expect about 16, 256, 4096 and 65536
   attempts). Then `set_difficulty 4` and `mine_solo` to see a hash starting with `0000`.

To see overdue detection without waiting two weeks, use `advance_days 20` (ADMIN). It moves the library's
simulated clock forward, and the overdue scan then flags every copy past its due date.

## Commands

| Group | Commands |
|---|---|
| Membership | `register_member <id> "<name>" <course>`, `view_member <id>`, `member_nonce <id>`, `list_members` |
| Books and copies | `register_book <id> "<title>" "<author>" [copies=N]`, `view_book <id>`, `add_copy <book_id> [NEW\|GOOD\|WORN]`, `view_copy <copy_id>`, `mark_lost <copy_id> <reason_code>`, `list_books` |
| Lending | `borrow_book <member_id> <book_id\|copy_id> [due=YYYY-MM-DD] [nonce=N]`, `return_book <copy_id> [condition] [reason=CODE]`, `renew_book <copy_id>`, `reserve_book <member_id> <book_id>`, `reservation_status [book_id]` |
| Fines and overdue | `check_overdue`, `view_fines [member_id]`, `settle_fine <member_id> [amount] [method=tokens\|cash]`, `advance_days <n>` |
| Tokens and ledger | `token_transfer <from> <to> <amount>`, `token_balance [id]`, `utxo_view [owner]`, `account_balance [id]`, `account_transfer <from> <to> <amount> [nonce]`, `account_nonce <id>`, `transaction_history <member_id>`, `replay_tx <txid-prefix>` |
| Chain and mining | `pool_view`, `mine_solo`, `mine_pool [miners]`, `mine_cloud <rounds>`, `chain_view [index]`, `validate_chain`, `chain_save`, `chain_load`, `difficulty_status`, `set_difficulty <1-4>`, `benchmark` |
| Fraud and audit | `fraud_review`, `approve_suspicious <id>`, `reject_suspicious <id>`, `replay_request <id>`, `lending_history`, `member_history <id>`, `book_history <book_id\|copy_id>`, `tamper_block <index> <record\|header\|remine>` |
| Session | `add_user`, `help`, `exit` |

## Library rules (configurable in `src/state.h` and `src/app.h`)

| Rule | Value |
|---|---|
| Loan period / renewal | 14 days / +7 days, at most 2 renewals; not allowed if another member reserved the title or the copy is overdue |
| Fine | 1 coin per day overdue, charged when the late return is mined; 20 coins for a borrowed copy marked LOST |
| Token reward | 10 coins on time, 5 late (minus the 1-coin fee, which goes to the miner); none while a book stays overdue |
| Block reward | 50 coins per block plus fees |
| Fraud limits | more than 3 copies at once; re-borrowing a title within 24 hours of returning it; a duplicate request_id; a return that lowers the condition by more than one level without a reason code |

## Files

| File | Contents |
|---|---|
| `books.txt`, `members.txt` | registries (`id,title,author` and `id,full_name,course_code`) |
| `copies.txt` | `copy_id,book_id,condition[,added_at_block]` |
| `chain.dat` | every block with its records (binary) |
| `pending.dat` | the pending pool |
| `ledger.dat` | every confirmed token transaction |
| `library_state.dat` | copy inventory, reservation queues, request nonces, fines |
| `balances.dat`, `utxo.dat` | balances and account nonces; the UTXO set |
| `meta.dat` | difficulty, block reward, batch size, simulated clock, next request ID, chain tip |
| `users.dat`, `keys/` | librarian accounts (PBKDF2), librarian and member ECDSA keys |

Every file is written to a temporary file and renamed, so a crash never leaves half a file. At startup the
saved inventory and balances are compared with the state rebuilt from the chain; the chain wins if they
differ.

## Project layout

```
src/
  main.c          options, command table and CLI loop, chain/tamper commands
  app.c/.h        startup, persistence, validate_chain, request submission, overdue scan
  record.c/.h     lending record, signing and verification
  merkle.c/.h     Merkle tree
  blockchain.c/.h block, proof of work, chain checks
  state.c/.h      copy inventory, state machine, reservation queues, nonces, fines
  pending.c/.h    pending pool and priority order
  fraud.c/.h      fraud heuristics and review commands
  lending.c/.h    membership, book, copy, lending, fine and history commands
  tokens.c/.h     token commands
  mining.c/.h     block building, solo / pool / cloud mining, difficulty
  ledger, utxo, account, transaction   the two token models
  crypto, auth, registry, storage, input  SHA-256/ECDSA/keystore, login, registries, files, input
tests/run_tests.sh    automated end-to-end tests (make test)
docs/                 Technical Report - Summative (.docx/.pdf), System Design - Summative.pdf, diagrams/
```
