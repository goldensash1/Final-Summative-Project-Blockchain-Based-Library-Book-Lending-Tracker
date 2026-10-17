#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "record.h"

static const char *ACTION_NAMES[ACT_COUNT] = {
    "BORROWED", "RETURNED", "RESERVED", "RENEWED", "OVERDUE", "LOST", "FINE_PAID", "TRANSFER"
};

static const char *CONDITION_NAMES[] = {"NEW", "GOOD", "WORN", "LOST"};

const char *record_action_name(int action) {
    return action >= 0 && action < ACT_COUNT ? ACTION_NAMES[action] : "INVALID";
}

const char *condition_name(int condition) {
    return condition >= COND_NEW && condition <= COND_LOST ? CONDITION_NAMES[condition] : "-";
}

int condition_parse(const char *s) {
    for (int c = COND_NEW; c <= COND_LOST; c++) {
        if (strcasecmp(s, CONDITION_NAMES[c]) == 0) return c;
    }
    return COND_NONE;
}

void record_init(LendingRecord *r, int action) {
    memset(r, 0, sizeof(*r));
    r->action = action;
    r->condition = COND_NONE;
    snprintf(r->copy_id, sizeof(r->copy_id), "-");
    snprintf(r->tx_id, sizeof(r->tx_id), "%s", ZERO_HASH);
}

/* The signed content, as one delimited string. The signer and its public key
 * are part of it, so a signature cannot be moved to another identity. */
static int record_payload(const LendingRecord *r, char *buf, size_t size) {
    int n = snprintf(buf, size, "%s|%s|%s|%s|%s|%s|%s|%ld|%ld|%d|%d|%s|%d|%s|%u|%s|%s",
                     r->record_id, r->book_id, r->copy_id, r->book_title, r->member_id,
                     r->member_name, record_action_name(r->action), (long)r->timestamp,
                     (long)r->due_date, r->fine_amount, r->condition, r->reason_code,
                     r->token_reward, r->tx_id, r->nonce, r->signer, r->signer_pubkey);
    return (n < 0 || (size_t)n >= size) ? -1 : n;
}

void record_signing_hash(const LendingRecord *r, char out[SHA256_HEX_LEN + 1]) {
    char buf[1024];
    int n = record_payload(r, buf, sizeof(buf));
    if (n < 0) {
        out[0] = '\0';
        return;
    }
    sha256_hex((const unsigned char *)buf, (size_t)n, out);
}

void record_hash(const LendingRecord *r, char out[SHA256_HEX_LEN + 1]) {
    char buf[1024 + 2 * MAX_SIGNATURE_LEN + 2];
    int n = record_payload(r, buf, 1024);
    if (n < 0 || r->sig_len > MAX_SIGNATURE_LEN) {
        out[0] = '\0';
        return;
    }
    buf[n++] = '|';
    bytes_to_hex(r->signature, r->sig_len, buf + n);
    n += (int)(2 * r->sig_len);
    sha256_hex((const unsigned char *)buf, (size_t)n, out);
}

int record_sign(LendingRecord *r, const char *signer, EVP_PKEY *key) {
    if (!key) return 0;
    snprintf(r->signer, sizeof(r->signer), "%s", signer);
    if (!crypto_pubkey_hex(key, r->signer_pubkey)) return 0;

    char h[SHA256_HEX_LEN + 1];
    record_signing_hash(r, h);
    return h[0] != '\0' && crypto_sign_hash(key, h, r->signature, &r->sig_len);
}

int record_signature_valid(const LendingRecord *r) {
    if (r->sig_len == 0) return 0;   /* missing signature */
    char h[SHA256_HEX_LEN + 1];
    record_signing_hash(r, h);
    return h[0] != '\0' &&
           crypto_verify_hash_pubhex(r->signer_pubkey, h, r->signature, r->sig_len);
}

int record_signer_registered(const LendingRecord *r) {
    char registered[PUBKEY_HEX_LEN + 1];
    return keystore_registered_pubkey(r->signer, registered) &&
           strcmp(registered, r->signer_pubkey) == 0;
}

void record_sanitize(LendingRecord *r) {
    r->record_id[sizeof(r->record_id) - 1] = '\0';
    r->book_id[sizeof(r->book_id) - 1] = '\0';
    r->copy_id[sizeof(r->copy_id) - 1] = '\0';
    r->book_title[sizeof(r->book_title) - 1] = '\0';
    r->member_id[sizeof(r->member_id) - 1] = '\0';
    r->member_name[sizeof(r->member_name) - 1] = '\0';
    r->reason_code[sizeof(r->reason_code) - 1] = '\0';
    r->tx_id[sizeof(r->tx_id) - 1] = '\0';
    r->signer[sizeof(r->signer) - 1] = '\0';
    r->signer_pubkey[sizeof(r->signer_pubkey) - 1] = '\0';
    if (r->sig_len > MAX_SIGNATURE_LEN) r->sig_len = 0;
}

void format_date(time_t t, char *buf, size_t size) {
    struct tm *tm_info = t ? localtime(&t) : NULL;
    if (!tm_info || strftime(buf, size, "%Y-%m-%d", tm_info) == 0) snprintf(buf, size, "-");
}

void format_datetime(time_t t, char *buf, size_t size) {
    struct tm *tm_info = t ? localtime(&t) : NULL;
    if (!tm_info || strftime(buf, size, "%Y-%m-%d %H:%M", tm_info) == 0) snprintf(buf, size, "-");
}

void record_print_header(const char *prefix) {
    printf("%s%-10s %-9s %-9s %-20s %-7s %-14s %-10s %4s %5s %5s %-12s\n", prefix, "Record", "Action",
           "Copy", "Title", "Member", "Name", "Due", "Fine", "Rewrd", "Nonce", "Signer");
}

void record_print_row(const LendingRecord *r, const char *prefix) {
    char due[16];
    format_date(r->due_date, due, sizeof(due));
    printf("%s%-10s %-9s %-9.9s %-20.20s %-7.7s %-14.14s %-10s %4d %5d %5u %-12.12s\n", prefix,
           r->record_id, record_action_name(r->action), r->copy_id, r->book_title, r->member_id,
           r->member_name, due, r->fine_amount, r->token_reward, r->nonce, r->signer);
}
