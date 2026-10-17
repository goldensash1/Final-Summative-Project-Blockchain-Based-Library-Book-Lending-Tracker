#!/bin/bash
# Automated end-to-end tests for the summative library tracker.
#
# Every test runs the real program in a throw-away folder, feeds it CLI
# commands through stdin and checks the output. Your own users.dat, keys/
# and chain files are never touched.
#
# Usage:  make test      (or: bash tests/run_tests.sh from the project folder)

set -u
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$PROJECT/library_tracker"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/lt_tests.XXXXXX")"
if [ -n "${KEEP:-}" ]; then echo "(keeping $WORK)"; else trap 'rm -rf "$WORK"' EXIT; fi

PASS=0
FAIL=0
LOGIN=$'tester\nTestPass123\n'

if [ ! -x "$BIN" ]; then
    echo "Build the program first (make)." >&2
    exit 1
fi

# check "description" "pattern" file   (pattern is an extended regex)
check() {
    if grep -Eq -- "$2" "$3"; then
        PASS=$((PASS + 1)); printf '  PASS  %s\n' "$1"
    else
        FAIL=$((FAIL + 1)); printf '  FAIL  %s\n        (expected to find: %s)\n' "$1" "$2"
    fi
}

# check_not "description" "pattern" file
check_not() {
    if grep -Eq -- "$2" "$3"; then
        FAIL=$((FAIL + 1)); printf '  FAIL  %s\n        (did not expect: %s)\n' "$1" "$2"
    else
        PASS=$((PASS + 1)); printf '  PASS  %s\n' "$1"
    fi
}

# run OUTFILE ARGS... : runs the program in $WORK with stdin from the caller
run() {
    local out="$1"; shift
    (cd "$WORK" && "$BIN" "$@") > "$out" 2>&1
}

cp "$PROJECT/books.txt" "$PROJECT/members.txt" "$PROJECT/copies.txt" "$WORK/"
cd "$WORK" || exit 1

echo "== 1. First run: registries, keys, borrowing, reservations, multi-record blocks"
{
    printf 'tester\nTestPass123\nTestPass123\n%s' "$LOGIN"
    printf 'borrow_book ALU001 BK001\nborrow_book ALU999 BK001\nborrow_book ALU001 BK999\n'
    printf 'borrow_book ALU002 BK003\nborrow_book ALU003 BK003\nreserve_book ALU003 BK003\n'
    printf 'pool_view\nmine_solo\nchain_view 1\nexit\n'
} | run out1 --model utxo --seed 7
check "genesis block mined"                        "Genesis block mined at difficulty 2" out1
check "copies loaded from copies.txt"              "Loaded 9 physical copies from copies.txt" out1
check "member ECDSA key pairs generated"           "Generated 5 member ECDSA P-256 key pair" out1
check "unknown member rejected"                    "ERROR: Member ALU999 not found" out1
check "unknown book rejected"                      "ERROR: Book BK999 not found" out1
check "borrow signed by the member, nonce 0"       "REQ000001 \(BORROWED\) signed by ALU001 with nonce 0" out1
check "no copy available -> suggest reservation"   "No copy of 'The River Between' is available" out1
check "reservation accepted"                       "REQ000003 \(RESERVED\) signed by ALU003" out1
check "pool lists 3 pending requests"              "Pending pool: 3 request\(s\) \(3 PENDING, 0 SUSPICIOUS\)" out1
check "3 records batched into one block"           "Block body: 3 record\(s\)" out1
check "hash attempts displayed"                    "Proof of work found: nonce [0-9]+ after [0-9]+ hash attempts" out1
check "block confirmed, requests removed"          "CONFIRMED as block #1: 3 request" out1
check "UTXO set printed after the block"           "UTXO set: [0-9]+ confirmed unspent output" out1
check "Merkle tree printed with root"              "Level 2 \(root\)" out1

echo "== 2. Restart: validation from disk, on-time return, reservation offer, renewal"
{
    printf '%s' "$LOGIN"
    printf 'renew_book BK001-C1\nreturn_book BK003-C1\nborrow_book ALU001 BK003\nmine_solo\n'
    printf 'borrow_book ALU003 BK003\nmine_solo\nexit\n'
} | run out2 --model utxo
check "saved chain re-validated at startup"         "validate_chain: Chain is VALID. 2 block" out2
check "renewal extends due date by 7 days"          "renewal 1 of 2" out2
check "on-time return earns 10 coins"               "reward tx [0-9a-f]+: 10 coins \(9.00 to member \+ 1.00 fee\)" out2
check "returned copy held for queue head"           "RESERVATION OFFER: returned copy BK003-C1 is now RESERVED for ALU003" out2
check "others cannot borrow the held copy"          "1 held for reservations" out2
check "queue member borrows the held copy"          "Copy BK003-C1 was held for ALU003" out2

echo "== 3. Overdue detection, fines, priority, settlement"
{
    printf '%s' "$LOGIN"
    printf 'advance_days 20\npool_view\nreturn_book BK003-C1 GOOD\npool_view\nmine_solo\n'
    printf 'view_fines ALU003\nsettle_fine ALU003\nsettle_fine ALU003 2 method=cash\nmine_solo\n'
    printf 'view_fines ALU003\nexit\n'
} | run out3 --model utxo
check "overdue copy detected automatically"        "OVERDUE DETECTED: copy BK003-C1" out3
check "OVERDUE record signed by the librarian"     "\(OVERDUE\) signed by LIB:tester" out3
check "overdue-related request has priority"       "1   REQ[0-9]+  OVERDUE   BK003-C1" out3
check "late return detected with a fine"           "LATE by [0-9]+ day\(s\), fine [0-9]+ coin" out3
check "late return earns 5 coins"                  "5 coins \(4.00 to member \+ 1.00 fee\)" out3
check "fine owed after mining"                     "ALU003 +Emmanuel +[1-9][0-9]* " out3
check "token payment refused without funds"        "REJECTED: token transaction: insufficient" out3
check "cash settlement accepted"                   "\(FINE_PAID\) signed by ALU003" out3

echo "== 4. Replay protection and fraud detection"
{
    printf '%s' "$LOGIN"
    printf 'borrow_book ALU004 BK002 nonce=5\n'
    printf 'borrow_book ALU002 BK001\nborrow_book ALU002 BK002\nborrow_book ALU002 BK004\nborrow_book ALU002 BK005\n'
    printf 'return_book BK001-C1 WORN\n'
    printf 'replay_request REQ000001\nfraud_review\nmine_solo\npool_view\n'
    printf 'approve_suspicious REQ000001\nreject_suspicious REQ000001\nexit\n'
} | run out4 --model utxo
check "wrong nonce rejected"                        "REJECTED: nonce 5 is not ALU004's expected next nonce 0" out4
check "excessive concurrent borrows flagged"        "SUSPICIOUS: excessive concurrent borrows: ALU002 already holds 3" out4
check "condition manipulation flagged"              "SUSPICIOUS: condition manipulation: BK001-C1 downgraded NEW -> WORN" out4
check "duplicate request flagged"                   "FLAGGED: duplicate request: request_id REQ000001" out4
check "replay keeps a valid signature"              "the ECDSA signature is VALID" out4
check "suspicious requests withheld from mining"    "Withheld REQ[0-9]+ \(BORROWED\): SUSPICIOUS" out4
check "duplicate cannot be approved"                "a replay can only be rejected" out4
check "duplicate rejected in review"                "REJECTED: REQ000001 discarded" out4

echo "== 5. Review workflow and re-borrow heuristic"
RET_ID=$(grep -Eo "FLAGGED: request REQ[0-9]+ \(RETURNED\)" out4 | grep -Eo "REQ[0-9]+")
{
    printf '%s' "$LOGIN"
    printf 'fraud_review\napprove_suspicious %s\nmine_solo\n' "$RET_ID"
    printf 'borrow_book ALU001 BK001\nexit\n'
} | run out5 --model utxo
check "approved request becomes PENDING"            "APPROVED: $RET_ID" out5
check "approved return is mined"                    "$RET_ID +RETURNED" out5
check "rapid re-borrow flagged"                     "SUSPICIOUS: rapid re-borrow: ALU001 returned BK001" out5

echo "== 6. Tokens: UTXO change and double spend; account nonces and history"
EXC_ID=$(grep -Eo "FLAGGED: request REQ[0-9]+ \(BORROWED\)" out4 | grep -Eo "REQ[0-9]+" | head -1)
REB_ID=$(grep -Eo "FLAGGED: request REQ[0-9]+ \(BORROWED\)" out5 | grep -Eo "REQ[0-9]+" | head -1)
{
    printf '%s' "$LOGIN"
    printf 'reject_suspicious %s\nreject_suspicious %s\n' "$EXC_ID" "$REB_ID"
    printf 'token_transfer ALU002 ALU005 3\nmine_solo\nutxo_view ALU002\nexit\n'
} | run out6 --model utxo
TXID=$(grep -Eo "ACCEPTED: transfer [0-9a-f]{16}" out6 | head -1 | awk '{print $3}')
printf '%sreplay_tx %s\nexit\n' "$LOGIN" "${TXID:0:12}" | run out6b --model utxo
check "transfer change returned to sender"          "change back to sender" out6
check "UTXO double spend rejected"                  "REJECTED: double spend rejected" out6b
check "rejected flags free the member's later requests" "REQ[0-9]+  TRANSFER" out6
{
    printf '%s' "$LOGIN"
    printf 'account_transfer ALU005 ALU001 1 7\naccount_transfer ALU005 ALU001 1 0\nmine_solo\n'
    printf 'account_balance ALU005\naccount_nonce ALU005\ntransaction_history ALU005\nexit\n'
} | run out7 --model account
check "account model: skipped nonce rejected"       "invalid nonce: 7 skips ahead" out7
check "account model: correct nonce accepted"       "ACCEPTED: transfer .* account nonce 0" out7
check "account nonce incremented on confirmation"   "Account ALU005: nonce 1" out7
check "history printed from the linked list"        "Transaction history for ALU005 \(linked list" out7

echo "== 7. Pool and cloud mining, difficulty"
{
    printf '%s' "$LOGIN"
    printf 'token_transfer ALU001 ALU005 0.5\nmine_pool 4\n'
    printf 'token_transfer ALU001 ALU002 0.5\nmine_cloud 5\n'
    printf 'difficulty_status\nset_difficulty 4\ntoken_transfer ALU001 ALU003 0.5\nmine_solo\nexit\n'
} | run out8 --model utxo --seed 3
check "pool fee is 2%"                              "Pool fee \(2%\)" out8
check "pool table shows shares and rewards"         "\| Miner ID  \| Hash rate \|   Attempts \|  Share % \| Blocks \|      Reward \|" out8
check "cloud mining multi-round summary"            "Cloud mining: multi-round earnings summary" out8
check "cloud gross, fees and net printed"           "Net profit       :" out8
check "cloud warning when fees exceed rewards"      "WARNING: after round [0-9] the cumulative fees" out8
check "difficulty status"                           "Expected effort : ~256 hash attempts" out8
check "difficulty 4 block has 4 leading zeros"      "Block hash: 0000" out8
printf '%s' "$LOGIN" | run out9 --model utxo --difficulty 7
check "--difficulty 7 rejected"                     "ERROR: --difficulty must be a number from 1 to 4" out9

echo "== 8. Registration and persistence of runtime additions"
{
    printf '%s' "$LOGIN"
    printf 'register_member ALU006 "Jane Doe" BLK103\nregister_member 12X "Bad" BLK1\n'
    printf 'register_book BK006 "Arrow of God" "Chinua Achebe" copies=2\nadd_copy BK003 GOOD\n'
    printf 'borrow_book ALU006 BK006 due=2026-02-30\nborrow_book ALU006 BK006 due=2020-01-01\n'
    printf 'borrow_book ALU006 BK006\nmark_lost BK006-C2 SHELF_MISSING\nmine_solo\nexit\n'
} | run out10 --model utxo
check "member registered with key pair"             "SUCCESS: Member ALU006 .* registered" out10
check "invalid member ID format rejected"           "ERROR: invalid member ID '12X'" out10
check "book registered with copies"                 "Copy BK006-C2 added" out10
check "copy added"                                  "SUCCESS: Copy BK003-C2" out10
check "impossible date rejected"                    "Invalid due date '2026-02-30'" out10
check "past date rejected"                          "The due date 2020-01-01 is in the past" out10
check "mark_lost with reason code"                  "\(LOST\) signed by LIB:tester" out10
printf '%sexit\n' "$LOGIN" | run out11 --model account
check "chain with every record type re-validates"   "validate_chain: Chain is VALID" out11
check "model switch replays same ledger"            "into the Account-based model" out11

echo "== 9. Tamper detection"
printf '%stamper_block 1 record\nvalidate_chain\nborrow_book ALU004 BK002\nchain_load\nexit\n' "$LOGIN" | run out12 --model utxo
check "edited record: Merkle root mismatch"         "Block #1: Merkle root recomputed .* does not match" out12
check "edited record: signature fails"              "Block #1: record REQ[0-9]+ .*ECDSA signature .* does not verify" out12
check "tampered session refuses new requests"       "ERROR: Session locked" out12
check "chain_load restores the genuine chain"       "Chain reloaded: .* pending request" out12
printf '%stamper_block 1 header\nvalidate_chain\nexit\n' "$LOGIN" | run out13 --model utxo
check "edited header: block hash mismatch"          "Block #1: stored hash does not match the recomputed hash" out13
printf '%stamper_block 1 remine\nvalidate_chain\nexit\n' "$LOGIN" | run out14 --model utxo
check "re-mined block: next link broken"            "Block #2: previous_hash does not match block #1" out14
cp chain.dat chain.bak
python3 -I -c "
d=bytearray(open('chain.dat','rb').read()); i=d.find(b'Americanah'); d[i+1]=ord('X'); open('chain.dat','wb').write(d)"
printf '%sborrow_book ALU004 BK002\nexit\n' "$LOGIN" | run out15 --model utxo
check "edited chain.dat detected at startup"        "validate_chain: Chain is INVALID" out15
check "session locked until chain is restored"      "SESSION LOCKED" out15
cp chain.bak chain.dat
cp keys/members/ALU002_public.pem k.bak && cp keys/members/ALU004_public.pem keys/members/ALU002_public.pem
printf '%sexit\n' "$LOGIN" | run out16 --model utxo
check "swapped member key detected"                 "is signed by a key that is not ALU002's registered public key" out16
cp k.bak keys/members/ALU002_public.pem
printf '%sexit\n' "$LOGIN" | run out17 --model utxo
check "restored files validate again"               "validate_chain: Chain is VALID" out17

echo "== 10. Access control and missing files"
printf '%sadd_user\n1\nlibrarian1\nLibPass1234\nLibPass1234\nexit\n' "$LOGIN" | run out18 --model utxo
printf 'librarian1\nLibPass1234\nset_difficulty 3\ntamper_block 1 header\nexit\n' | run out19 --model utxo
check "librarian created"                           "SUCCESS: LIBRARIAN account 'librarian1' created" out18
check "librarian cannot change difficulty"          "requires the ADMIN role" out19
mv copies.txt copies.bak
printf '%sexit\n' "$LOGIN" | run out20 --model utxo
check "missing copies.txt reported"                 "Could not open copy inventory file 'copies.txt'" out20
mv copies.bak copies.txt
cp books.txt books.bak && cp members.txt members.bak
printf '12,Bad ID,Nobody\n' >> books.txt
printf 'ALU009,Too Long Course,ABCDEFGHIJKLMNOP\n' >> members.txt
printf '%sexit\n' "$LOGIN" | run out21 --model utxo
check "invalid book ID in books.txt skipped"        "Invalid book on line [0-9]+ skipped" out21
check "invalid member in members.txt skipped"       "Invalid member on line [0-9]+ skipped" out21
mv books.bak books.txt && mv members.bak members.txt

echo
echo "Result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
