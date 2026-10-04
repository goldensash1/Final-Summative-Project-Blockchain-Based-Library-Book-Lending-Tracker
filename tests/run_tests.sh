#!/bin/bash
# Automated end-to-end tests for the Formative 2 library tracker.
#
# Every test runs the real program in a throw-away folder, feeds it menu
# input through stdin and checks the output. Your own users.dat, keys/ and
# chain files are never touched.
#
# Usage:  make test      (or: bash tests/run_tests.sh from the project folder)

set -u
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$PROJECT/library_tracker"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/lt_tests.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

PASS=0
FAIL=0
USER_LOGIN=$'tester\nTestPass123\n'

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

cp "$PROJECT/books.txt" "$PROJECT/members.txt" "$WORK/"
cd "$WORK" || exit 1

echo "== 1. First run, lending events and token rewards (UTXO model)"
printf 'tester\nTestPass123\nTestPass123\n%s' "$USER_LOGIN" > in1
printf '1\nBK001\nALU001\n1\nBK002\nALU002\n3\nBK002\n2\nBK001\nALU001\n2\nBK002\nALU002\n' >> in1
printf '11\nALU001\nALU003\n2\n6\n10\n0\n' >> in1
run out1 --model utxo < in1
check "genesis block mined and chain valid"            "Genesis block mined at difficulty 2" out1
check "library treasury funded"                         "Library treasury funded with 10000.00 coins" out1
check "borrow goes to the pending pool"                 "borrowed by John Doe.*PENDING POOL" out1
check "overdue (not returned) creates no transaction"   "No token transaction created" out1
check "on-time return gives 10 coins (9 + 1 fee)"       "ON TIME" out1
check "on-time reward amount"                           "token reward 10 coins from LIBRARY = 9.00 to ALU001" out1
check "late return gives 5 coins (4 + 1 fee)"           "token reward 5 coins from LIBRARY = 4.00 to ALU002" out1
check "UTXO change output shown"                        "change back to sender" out1
check "unconfirmed reward cannot be spent"              "REJECTED: insufficient funds: ALU001 has 0.00 spendable" out1
check "pending pool lists 5 blocks"                     "Pending pool: 5 unconfirmed block" out1
check "balances unchanged before mining"                "ALU001 +John Doe +0\.00" out1

echo "== 2. Solo mining confirms the pending pool"
printf '%s7\n1\n10\n14\n15\n0\n' "$USER_LOGIN" | run out2 --model utxo
check "every block confirmed"                           "CONFIRMED as block #5" out2
check "hash attempts reported"                          "Proof of work found: nonce [0-9]+ after [0-9]+ hash attempts" out2
check "UTXO set printed after confirmed blocks"         "UTXO set: [0-9]+ confirmed unspent output" out2
check "solo miner paid 5 x 50 + 2 fees"                 "Paid to solo miner  : 252.00 coins" out2
check "ALU001 credited 9.00 after mining"               "ALU001 +John Doe +9\.00" out2
check "ALU002 credited 4.00 after mining"               "ALU002 +Jane Smith +4\.00" out2
check "records show token reward and signature"         "RETURNED +10 +[0-9a-f]{8} +VALID" out2
check "chain valid after mining"                        "Chain is VALID" out2

echo "== 3. UTXO transfer, change, locked inputs and pool mining"
printf '%s11\nALU001\nALU004\n3\n11\nALU001\nALU005\n5\n7\n2\n4\n0\n' "$USER_LOGIN" | run out3 --model utxo --seed 5
check "transfer accepted"                               "ACCEPTED: transfer [0-9a-f]+" out3
check "change returned to sender (9 = 3 + 5 + 1 fee)"   "ALU001 +5\.00 +\(change back to sender\)" out3
check "second transfer rejected (inputs locked)"        "REJECTED: insufficient funds: ALU001 has 5.00 spendable \(9.00 locked" out3
check "pool fee is 2%"                                  "Pool fee \(2%\) +: 1.02 coins" out3
check "pool reward table printed"                       "\| Miner ID +\| Hash rate \| +Attempts \| +Share % \|" out3
TXID=$(grep -Eo 'ACCEPTED: transfer [0-9a-f]+' out3 | head -1 | awk '{print $3}' | cut -c1-12)

echo "== 4. Double spend is rejected (UTXO replay)"
printf '%s13\n%s\n10\n0\n' "$USER_LOGIN" "$TXID" | run out4 --model utxo
check "replayed transfer rejected as double spend"      "REJECTED: double spend rejected: input [0-9a-f]+:[0-9] is already spent" out4
SUPPLY_UTXO=$(grep -Eo 'Total coins in circulation: [0-9.]+' out4 | tail -1)

echo "== 5. Account model: same balances, nonce and balance checks"
printf '%s10\n' "$USER_LOGIN" > in5
printf '11\nALU001\nALU004\n1\n0\n' >> in5                      # reused nonce
printf '11\nALU001\nALU004\n1\n7\n' >> in5                      # skipped nonce
printf '11\nALU004\nALU001\n50\n0\n' >> in5                     # insufficient balance
printf '11\nALU001\nALU004\n1\n1\n' >> in5                      # correct
printf '13\n%s\n12\nALU001\n0\n' "$TXID" >> in5
run out5 --model account < in5
SUPPLY_ACC=$(grep -Eo 'Total coins in circulation: [0-9.]+' out5 | head -1)
if [ -n "$SUPPLY_UTXO" ] && [ "$SUPPLY_UTXO" = "$SUPPLY_ACC" ]; then
    PASS=$((PASS + 1)); echo "  PASS  both models give the same total supply ($SUPPLY_ACC)"
else
    FAIL=$((FAIL + 1)); echo "  FAIL  models disagree: '$SUPPLY_UTXO' vs '$SUPPLY_ACC'"
fi
check "reused nonce rejected"                           "REJECTED: invalid nonce: nonce 0 was already used" out5
check "skipped nonce rejected"                          "REJECTED: invalid nonce: 7 skips ahead" out5
check "insufficient balance rejected"                   "REJECTED: insufficient balance: ALU004 has 3.00 available" out5
check "correct nonce accepted"                          "nonce 1 accepted for ALU001" out5
check "replay rejected by nonce"                        "Account model: the sender's nonce moved past 0" out5
check "history printed from the linked list"            "Transaction history for ALU001 \(linked list, 2 entries" out5

echo "== 6. Cloud mining: profitable and unprofitable rentals"
printf '%s1\nBK003\nALU003\n7\n3\n1\n0\n' "$USER_LOGIN" | run out6 --model account --difficulty 1
check "cloud summary printed"                           "Cloud mining: multi-round earnings summary" out6
check "short rental is profitable"                      "The rental was profitable in every round" out6
printf '%s1\nBK004\nALU004\n7\n3\n5\n0\n' "$USER_LOGIN" | run out7 --model utxo --difficulty 1
check "5 rounds for 1 block triggers the warning"       "WARNING: after round [0-9] the cumulative fees" out7
check "overall loss reported"                           "WARNING: This rental was UNPROFITABLE overall" out7
check "gross, fees and net printed"                     "Net profit +: -[0-9]" out7

echo "== 7. Difficulty"
printf '%s9\n8\n4\n1\nBK005\nALU005\n7\n1\n0\n' "$USER_LOGIN" | run out8 --model utxo
check "benchmark expected attempts 16^4"                "^4 +0000\.\.\. +65536" out8
check "difficulty changed to 4 from the menu"           "Difficulty set to 4" out8
check "block mined with 4 leading zeros"                "Block hash: 0000[0-9a-f]{60}" out8
printf '' | run out9 --difficulty 7; RC=$?
check "--difficulty 7 rejected"                         "--difficulty must be a number from 1 to 4" out9
if [ "$RC" -ne 0 ]; then PASS=$((PASS + 1)); echo "  PASS  invalid argument exits with an error code"
else FAIL=$((FAIL + 1)); echo "  FAIL  invalid argument should exit with an error code"; fi

echo "== 8. Tamper detection"
printf '%s16\n2\n5\n10\n15\n1\nBK001\nALU001\n0\n' "$USER_LOGIN" | run out10 --model utxo
check "re-mined block: signature fails"                 "Block #5: librarian's digital signature does not verify" out10
check "re-mined block: reward mismatches its tx"        "Block #5: token reward 10 does not match its reward transaction" out10
check "re-mined block: next link broken"                "Block #6: previous_hash does not match" out10
check "new blocks refused on an invalid chain"          "Refusing to add or mine blocks on an invalid chain" out10
printf '%s15\n0\n' "$USER_LOGIN" | run out11 --model utxo
check "restart reloads the genuine chain"               "Chain is VALID" out11
cp ledger.dat ledger.bak
python3 - <<'EOF'
d = bytearray(open('ledger.dat', 'rb').read())
i = d.find(b'late return reward')
d[i:i + 4] = b'LATE'
open('ledger.dat', 'wb').write(d)
EOF
printf '%s' "$USER_LOGIN" | run out12 --model utxo
check "edited ledger.dat detected at startup"           "does not match its SHA-256 ID. The ledger file was modified" out12
mv ledger.bak ledger.dat

echo "== 9. Access control"
printf '%s17\n1\nlibrarian1\nLibPass1234\nLibPass1234\n0\n' "$USER_LOGIN" | run out13 --model utxo
printf 'librarian1\nLibPass1234\n8\n16\n0\n' | run out14 --model utxo
check "librarian created"                               "LIBRARIAN account 'librarian1' created" out13
check "librarian cannot change difficulty or tamper"    "Access denied. This action requires the ADMIN role" out14
check_not "librarian session has no crash"              "Segmentation|Abort" out14

echo
echo "Result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
