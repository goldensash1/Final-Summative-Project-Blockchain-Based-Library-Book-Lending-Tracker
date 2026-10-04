# Blockchain-Based Library Book Lending Tracker: Formative 2

This project extends the Formative 1 lending tracker. Each borrow, return, overdue mark and token transfer still
creates a signed block, but now:

- **Returning a book creates a token reward transaction automatically.** An on-time return earns 10 coins and a
  late return earns 5. A book that is overdue and not returned creates no transaction. A fixed fee of 1 coin
  is taken from every reward and transfer and goes to the miner.
- **Blocks wait in a pending pool.** They are added to the chain only after a miner confirms them with
  proof of work. Balances change only at that point.
- **Balances use one of two transaction models,** chosen for the whole session: the **UTXO** model
  (like Bitcoin) or the **account-based** model (like Ethereum).
- **There are three ways to mine:** solo, pool (with a 2% pool fee and rewards shared by hash attempts) and
  cloud (rented hash power for 1 to 5 rounds, with rental and maintenance fees).

Everything from Formative 1 is still there: the book and member registries, login with PBKDF2 password
hashing, per-user ECDSA P-256 signing keys, chain validation and the tamper demo.

## How it works

**From a lending event to a token transaction.** Each lending action creates a block signed by the
logged-in librarian. A return also creates a reward transaction straight away: the `LIBRARY` treasury
pays 10 coins (on time) or 5 coins (late), the member receives the reward minus the 1-coin fee, and the
fee goes to the miner. The transaction ID is the SHA-256 hash of the transaction. It is stored in the
block's `tx_id` field and covered by the librarian's signature, so the block and its transaction can't be
separated or changed. Borrows and overdue marks create no transaction (token reward 0).

**How mining confirms blocks.** New blocks go into the pending pool (`pending.dat`) without an index,
a previous hash or a nonce. A miner takes them oldest first and, for each one:

1. checks the librarian's signature and that the transaction matches the block;
2. places the block after the current chain tip;
3. increases the nonce until the SHA-256 hash starts with `difficulty` zeros;
4. appends the block and confirms its transaction in the ledger.

Only at step 4 does the member's balance change. The miner is then paid 50 coins per block plus the
fees.

**UTXO model.** Balances are the sum of the unspent outputs a member owns. A transaction spends whole
outputs as inputs, pays the recipient (output 0) and returns any extra to the sender as change
(output 1). The fee is the inputs minus the outputs. Inputs are locked while the transaction is pending
and removed once it is confirmed, so spending them again is rejected as a double spend.

**Account model.** Each account stores a balance, a nonce and a linked-list history. A transaction must
use exactly the sender's next nonce, and the sender needs enough balance for the amount plus the fee
(minus whatever is already reserved by pending transactions). A reused or skipped nonce, or an
insufficient balance, is rejected.

**One ledger, two views.** All confirmed transactions are kept in order in `ledger.dat` and replayed
into the chosen model at startup, so both models always show the same balances.

## Dependencies

- A C compiler (`gcc` or `clang`) and `make`
- **OpenSSL 3.x** (headers and libcrypto/libssl). It provides SHA-256, ECDSA, PBKDF2, AES key encryption
  and secure random numbers.
  - macOS: `brew install openssl@3`
  - Debian/Ubuntu: `sudo apt-get install build-essential libssl-dev`

No other libraries are needed.

### Apple Silicon note

The `Makefile` looks for OpenSSL in `/opt/homebrew/opt/openssl@3` and `/usr/local/opt/openssl@3`. You can
override this with `make OPENSSL_PREFIX=/path/to/openssl`. If the only OpenSSL it finds is an Intel
(x86_64) build, it builds an x86_64 binary that runs under Rosetta 2. Install Rosetta once with
`softwareupdate --install-rosetta --agree-to-license`.

## Compile

```bash
make            # builds ./library_tracker from every file in src/
make test       # runs the automated end-to-end tests (51 checks, in a temporary folder)
make dist       # creates Formative2_Golden_Munyankindi.zip for submission
make clean      # removes the binary, chain.dat, pending.dat and ledger.dat (keeps accounts and keys)
make distclean  # full reset: also removes users.dat and keys/
```

`make test` runs the real program through every scenario in the brief: rewards, the pending pool, all
three mining methods, both models, double spends, nonces, the difficulty setting and tampering. It works
in a temporary folder, so your own accounts and chain are never touched. It should end with
`Result: 51 passed, 0 failed`.

To compile by hand (macOS example):

```bash
gcc -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L \
    -I$(brew --prefix openssl@3)/include -o library_tracker src/*.c \
    -L$(brew --prefix openssl@3)/lib -lssl -lcrypto
```

The code compiles with no warnings under `-Wall -Wextra`. Run the program from the project folder so it can
find `books.txt` and `members.txt`.

> Chain files from Formative 1 have a different block layout. The program detects this and asks you to run
> `make clean` first.

## Run, switch transaction model and set the difficulty

```
./library_tracker [--model utxo|account] [--difficulty 1-4] [--seed N] [--help]
```

| Option | Meaning |
|---|---|
| `-m`, `--model utxo` or `account` | Transaction model for the whole session. If you leave it out, a menu asks at startup. |
| `-d`, `--difficulty 1-4` | Proof-of-work difficulty, which is the number of leading zero hex digits the block hash needs. The default is **2**. Values outside 1 to 4 are rejected. |
| `-s`, `--seed N` | Seed for the random hash rates of the pool miners, so a pool run can be repeated. |

Make shortcuts:

```bash
make run                                   # asks for the model at startup
make run-utxo                              # --model utxo
make run-account ARGS="--difficulty 3"     # --model account --difficulty 3
```

**Switching models:** quit the program and start it again with the other `--model`. Both models are built
from the same confirmed transaction log (`ledger.dat`), so balances are identical in both. Only the way
they are stored and checked changes.

**Changing the difficulty during a session:** menu option **8** (ADMIN only) accepts a value from 1 to 4.
Each block stores the difficulty it was mined at, so older blocks still validate.

On the first run you create the admin account and log in. The program then mines the genesis block, which
also funds the `LIBRARY` reward treasury with 10,000 coins.

## Menu

```
 -- Lending --                            -- Tokens --
  1. Borrow Book                          10. View Balances & UTXO Set / Account Balances
  2. Return Book (creates the reward)     11. Transfer Tokens
  3. Mark Book Overdue                    12. Transaction History for an Account
  4. List Books                           13. Replay a Confirmed Transaction (double-spend / nonce test)
  5. List Members (with balances)        -- Chain --
 -- Pending pool & mining --              14. View Confirmed Lending Records
  6. View Pending Pool                    15. Validate Chain
  7. Mine Pending Pool (solo/pool/cloud)  16. Tamper With a Block (demo) [ADMIN]
  8. Set Mining Difficulty [ADMIN]        17. Add User Account [ADMIN]
  9. Difficulty Benchmark                  0. Exit
```

## How to test the mining simulations

1. Create some pending blocks. For example, borrow `BK001` for `ALU001` (1), borrow `BK002` for `ALU002`
   (1), mark `BK002` overdue (3), then return both books (2). The first return is on time (10 coins). The
   second is late (5 coins) because the loan was marked overdue. Option 6 shows the pending pool.
2. **Solo mining:** choose option 7, then 1. For every block the program shows the nonce, the number of
   hash attempts, the time and the hash. In the UTXO model it also shows the full UTXO set; in the account
   model it shows the updated accounts. At the end you see the reward: 50 coins per block plus the fees.
3. **Pool mining:** choose option 7, then 2, then enter the number of miners (2 to 8). Each miner gets a
   random hash rate. A table shows each miner's ID, attempts, share percentage and reward, after the 2%
   pool fee.
4. **Cloud mining:** choose option 7, then 3, then enter the number of rounds (1 to 5). The rental gives
   400 attempts per round for a 20-coin fee per round, plus a maintenance fee of 10% of the round's reward.
   - To see a **profitable** rental: start with `--difficulty 1`, create 2 or 3 pending blocks and rent 2
     rounds.
   - To see an **unprofitable** rental (with a warning): start with `--difficulty 3` and rent 2 rounds for
     one block, or rent 5 rounds when only one block is pending.
5. **Difficulty:** option 9 mines 20 sample blocks at each difficulty from 1 to 4. Expect about 16, 256,
   4096 and 65536 attempts on average. Set the difficulty to 4 with option 8 and solo-mine one block to
   watch it take longer.

## How to test the edge cases

| Test | Steps | Expected result |
|---|---|---|
| Overdue book that is never returned | Option 3 on a borrowed book | OVERDUE block with token reward 0 and no transaction |
| Insufficient balance | Option 11, amount larger than the member's balance | `REJECTED: insufficient ...` |
| Wrong or reused nonce (account model) | Option 11 with a nonce lower or higher than the one suggested | `REJECTED: invalid nonce ...` |
| Double spend (UTXO model) | Option 13, enter a confirmed transfer's ID | `REJECTED: double spend rejected: input ... is already spent` |
| Unconfirmed tokens | Transfer from a member whose reward is still pending | Rejected: tokens can be spent only after mining |
| Unprofitable cloud rental | See step 4 above | `WARNING: ... UNPROFITABLE` |
| Tampering | Option 16 then 15, or edit `chain.dat` / `ledger.dat` | Detected and reported |

## Project layout

```
src/
  main.c         command-line options, model selection, menu
  app.c/.h       shared state, startup, genesis, full validation, saving
  lending.c/.h   borrow / return (creates the reward tx) / overdue, book and member lists
  tokens.c/.h    balances, transfers, history, replay test
  mining.c/.h    solo / pool / cloud mining, difficulty setting and benchmark
  pending.c/.h   pending pool (FIFO, saved to pending.dat)
  blockchain.c/.h  Block struct, signing, SHA-256 block hash, proof of work, chain validation
  transaction.c/.h Transaction struct, SHA-256 transaction ID, amounts
  ledger.c/.h    confirmed transaction log, sends each transaction to the chosen model
  utxo.c/.h      UTXO set: coin selection, change, fees, double-spend checks
  account.c/.h   accounts: balance, nonce, linked-list history
  storage.c/.h   atomic binary file save/load with format checks
  crypto.c/.h    SHA-256 (with a midstate copy for fast mining), ECDSA keys, sign/verify
  auth.c/.h      accounts, PBKDF2 passwords, roles
  registry.c/.h  books.txt / members.txt
  input.c/.h     safe input helpers
tests/
  run_tests.sh   automated end-to-end tests (make test)
docs/
  Technical Report - Formative 2.pdf / .docx
  demo_video_script.md     word-for-word video recording script + keystroke cue card
  viva_questions.md        likely questions and answers about the design
  submission_checklist.md  requirement-by-requirement evidence and upload steps
  system_design.svg/.png, screenshots/
```

The program creates these files at runtime (they are not committed): `chain.dat` (confirmed blocks),
`pending.dat` (pending pool), `ledger.dat` (confirmed transactions), `users.dat` and `keys/`.

## Assumptions

- A loan is 14 days. A return is late if more than 14 days have passed **or** the loan was marked OVERDUE.
  The OVERDUE mark is what lets you demonstrate a late return without waiting two weeks.
- Rewards are paid from a `LIBRARY` treasury funded in the genesis block. A member receives the reward
  minus the 1-coin fee (9 or 4 coins), and the fee goes to the miner.
- Each confirmed block earns its miner 50 coins plus the fees of the transactions it contains.
- Members have no keys of their own. The logged-in librarian records and signs every action, including
  transfers, as in Formative 1.
- Cloud rental fees are paid to the provider outside the chain. The ledger receives the mining reward
  minus the maintenance fee.
