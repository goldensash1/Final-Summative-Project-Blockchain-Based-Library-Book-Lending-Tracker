#ifndef RECORD_H
#define RECORD_H

#include <stddef.h>
#include <time.h>
#include "crypto.h"

/* Field sizes shared by the registries, the copy inventory and the chain. */
#define ID_MAX 20               /* member / book IDs */
#define COPY_ID_MAX 24          /* e.g. BK001-C2 */
#define TITLE_MAX 80
#define PERSON_NAME_MAX 50
#define RECORD_ID_MAX 16        /* e.g. REQ000012 */
#define REASON_MAX 24           /* librarian override reason code */
#define SIGNER_MAX 40           /* member ID or LIB:<username> */

/* What a lending record (transaction) says happened. The first five are the
 * actions named in the brief; LOST, FINE_PAID and TRANSFER cover the
 * librarian override, fine settlement and member token transfers, which must
 * also pass through the pending pool and the chain. */
typedef enum {
    ACT_BORROWED = 0,
    ACT_RETURNED,
    ACT_RESERVED,
    ACT_RENEWED,
    ACT_OVERDUE,
    ACT_LOST,
    ACT_FINE_PAID,
    ACT_TRANSFER,
    ACT_COUNT
} RecordAction;

/* Physical condition of a copy, best to worst. A "level" is one step. */
typedef enum {
    COND_NONE = -1,
    COND_NEW = 0,
    COND_GOOD = 1,
    COND_WORN = 2,
    COND_LOST = 3
} CopyCondition;

/* One lending record. Everything except `signature`/`sig_len` is covered by
 * the ECDSA signature, so no field can change after signing. */
typedef struct {
    char record_id[RECORD_ID_MAX];      /* = the request_id it was submitted with */
    char book_id[ID_MAX];               /* must exist in the book registry */
    char copy_id[COPY_ID_MAX];          /* physical copy ("-" for a title reservation) */
    char book_title[TITLE_MAX];         /* copied from the registry when created */
    char member_id[ID_MAX];             /* must exist in the member registry */
    char member_name[PERSON_NAME_MAX];  /* copied from the registry when created */
    int action;                         /* RecordAction */
    time_t timestamp;                   /* when the request was made */
    time_t due_date;                    /* loan due date (0 when not applicable) */
    int fine_amount;                    /* coins; 0 unless late RETURNED / OVERDUE / LOST / FINE_PAID */
    int condition;                      /* CopyCondition reported (RETURNED / LOST), else COND_NONE */
    char reason_code[REASON_MAX];       /* librarian override reason ("" if none) */
    int token_reward;                   /* RETURNED: 10 on time, 5 late; 0 otherwise */
    char tx_id[SHA256_HEX_LEN + 1];     /* token transaction carried by the request
                                           (TRANSFER / FINE_PAID), ZERO_HASH if none */
    unsigned int nonce;                 /* signer's request counter (replay protection) */
    char signer[SIGNER_MAX];            /* member ID, or LIB:<username> for librarian actions */
    char signer_pubkey[PUBKEY_HEX_LEN + 1]; /* the signer's public key: its on-chain identity */
    unsigned char signature[MAX_SIGNATURE_LEN];
    unsigned int sig_len;
} LendingRecord;

#define ZERO_HASH "0000000000000000000000000000000000000000000000000000000000000000"

const char *record_action_name(int action);
const char *condition_name(int condition);

/* Parses "NEW", "good", ... Returns COND_NONE if unknown. */
int condition_parse(const char *s);

/* Clears a record and fills the defaults (no copy, no fine, no tx). */
void record_init(LendingRecord *r, int action);

/* SHA-256 hex of the signed content (every field but the signature). This is
 * what the signer signs. */
void record_signing_hash(const LendingRecord *r, char out[SHA256_HEX_LEN + 1]);

/* SHA-256 hex of the complete record including its signature: the Merkle
 * tree leaf. */
void record_hash(const LendingRecord *r, char out[SHA256_HEX_LEN + 1]);

/* Stores the key's public half in signer_pubkey and signs the record.
 * Returns 1 on success. */
int record_sign(LendingRecord *r, const char *signer, EVP_PKEY *key);

/* 1 if the signature verifies against the public key embedded in the record. */
int record_signature_valid(const LendingRecord *r);

/* 1 if the embedded public key is the one registered for the signer (so a
 * record cannot be signed by an attacker's own key). */
int record_signer_registered(const LendingRecord *r);

void record_sanitize(LendingRecord *r);

/* "2026-10-17" / "2026-10-17 14:05" in local time ("-" for 0). */
void format_date(time_t t, char *buf, size_t size);
void format_datetime(time_t t, char *buf, size_t size);

/* One-line summary for tables and logs. */
void record_print_row(const LendingRecord *r, const char *prefix);
void record_print_header(const char *prefix);

#endif
