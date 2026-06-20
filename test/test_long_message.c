/**
 * @file test_long_message.c
 * @brief End-to-end verification of LONG (concatenated) SMS handling.
 *
 * Unlike test_pdu_decoder.c (which mostly checks "does not crash") and
 * test_sms_assembly.c (which feeds synthetic pdu_sms_t structs), this file
 * drives the REAL pdu_decode() with REAL multi-part PDU hex strings -- the
 * exact bytes a SIM900A emits in an AT+CMGL response -- then reassembles the
 * fragments and asserts the final message is byte-exact and in the right
 * order.
 *
 * This is the path where "long message gets split / scrambled" bugs live.
 *
 * All source here is ASCII (hex strings + explicit UTF-8 byte arrays) so the
 * file is codepage-independent.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "unity.h"
#include "pdu_decoder.h"

/* ---- Minimal ordered reassembler -------------------------------------
 * Mirrors publish_assembled_sms()'s concatenation: place each fragment at
 * fragments[part_num] and join 1..total_parts in order. This isolates the
 * decode + ordering behavior (assembly-buffer slot management is already
 * covered by the 21 tests in test_sms_assembly.c).
 */
typedef struct {
    uint16_t ref_num;
    uint8_t  total_parts;
    uint8_t  received_parts;
    bool     active;
    char     fragments[11][PDU_MAX_MESSAGE_LEN]; /* 1-based, up to 10 parts */
    bool     part_received[11];
    char     sender[PDU_MAX_SENDER_LEN];
} reasm_t;

static void reasm_reset(reasm_t *r) { memset(r, 0, sizeof(*r)); }

/* Feed one decoded PDU. Returns true when the message is complete. */
static bool reasm_feed(reasm_t *r, const pdu_sms_t *sms) {
    if (!sms->is_multipart) {
        /* single -> "complete" with one fragment */
        strncpy(r->fragments[1], sms->message, PDU_MAX_MESSAGE_LEN - 1);
        r->part_received[1] = true;
        r->total_parts = 1;
        r->received_parts = 1;
        strncpy(r->sender, sms->sender, sizeof(r->sender) - 1);
        return true;
    }
    if (sms->part_num < 1 || sms->part_num > 10) return false;
    if (!r->active) { r->active = true; r->ref_num = sms->ref_num; r->total_parts = sms->total_parts; }
    strncpy(r->sender, sms->sender, sizeof(r->sender) - 1);
    if (!r->part_received[sms->part_num]) {
        r->part_received[sms->part_num] = true;
        strncpy(r->fragments[sms->part_num], sms->message, PDU_MAX_MESSAGE_LEN - 1);
        r->received_parts++;
    }
    return r->received_parts >= r->total_parts;
}

static void reasm_join(const reasm_t *r, char *out, size_t out_size) {
    out[0] = '\0';
    for (int i = 1; i <= r->total_parts && i <= 10; i++) {
        if (r->part_received[i]) {
            strncat(out, r->fragments[i], out_size - strlen(out) - 1);
        }
    }
}

/* ====================================================================== */
/* Case A: long UCS2 (Chinese) message, 3 parts, scrambled arrival order. */
/* ====================================================================== */
/*
 * Sender +886987654321, ref=0xAB, total=3. Reassembled text is:
 *   "<5 Chinese chars><1 Chinese char>123456<3 Chinese chars>"
 * i.e. a typical Taiwan verification SMS:  您的驗證碼為123456，請勿外洩
 *
 * Per-part PDU layout (UCS2, 8-bit concat ref):
 *   00            SMSC length = 0
 *   40            PDU type: SMS-DELIVER + UDHI
 *   0C 91 ...     OA: 12 digits, international (+886987654321)
 *   00            PID
 *   08            DCS = UCS2
 *   52308142214480  SCTS (skipped)
 *   <UDL>         total UD octets (UDH + data)
 *   05 00 03 AB TT PP   UDH: concat, ref=0xAB, total=TT, part=PP
 *   <UCS2 data>   UTF-16BE
 */
#define UCS2_PREFIX "00400C91889678563412000852308142214480"

static const char *PDU_UCS2_PART1 = UCS2_PREFIX "10" "050003AB0301" "60A876849A578B4978BC";
static const char *PDU_UCS2_PART2 = UCS2_PREFIX "14" "050003AB0302" "70BA003100320033003400350036";
static const char *PDU_UCS2_PART3 = UCS2_PREFIX "10" "050003AB0303" "FF0C8ACB52FF59166D29";

/* Expected reassembled message as explicit UTF-8 bytes:
 *   您 E68282A8? -> 您=E6 82 A8, 的=E7 9A 84, 驗=E9 A9 97, 證=E8 AD 89, 碼=E7 A2 BC,
 *   為=E7 82 BA, 1..6 = 31..36, ，=EF BC 8C, 請=E8 AB 8B, 勿=E5 8B BF, 外=E5 A4 96, 洩=E6 B4 A9
 */
static const unsigned char EXPECTED_UCS2_UTF8[] = {
    0xE6,0x82,0xA8, 0xE7,0x9A,0x84, 0xE9,0xA9,0x97, 0xE8,0xAD,0x89, 0xE7,0xA2,0xBC, /* 您的驗證碼 */
    0xE7,0x82,0xBA, 0x31,0x32,0x33,0x34,0x35,0x36,                                  /* 為123456   */
    0xEF,0xBC,0x8C, 0xE8,0xAB,0x8B, 0xE5,0x8B,0xBF, 0xE5,0xA4,0x96, 0xE6,0xB4,0xA9, /* ，請勿外洩 */
    0x00
};

void test_long_ucs2_each_part_detected_as_multipart(void) {
    const char *pdus[3] = { PDU_UCS2_PART1, PDU_UCS2_PART2, PDU_UCS2_PART3 };
    for (int i = 0; i < 3; i++) {
        pdu_sms_t sms;
        bool ok = pdu_decode(pdus[i], &sms);
        TEST_ASSERT_TRUE(ok); /* UCS2 part failed to decode */
        TEST_ASSERT_TRUE(sms.is_multipart); /* not flagged multipart -> would be split! */
        TEST_ASSERT_EQUAL_UINT16(0xAB, sms.ref_num);
        TEST_ASSERT_EQUAL_UINT8(3, sms.total_parts);
        TEST_ASSERT_EQUAL_UINT8(i + 1, sms.part_num);
        TEST_ASSERT_TRUE(sms.sender[0] == '+');
        TEST_ASSERT_EQUAL_STRING("+886987654321", sms.sender);
    }
}

void test_long_ucs2_reassembled_in_order(void) {
    reasm_t r; reasm_reset(&r);
    pdu_sms_t s1, s2, s3;
    TEST_ASSERT_TRUE(pdu_decode(PDU_UCS2_PART1, &s1));
    TEST_ASSERT_TRUE(pdu_decode(PDU_UCS2_PART2, &s2));
    TEST_ASSERT_TRUE(pdu_decode(PDU_UCS2_PART3, &s3));

    /* in-order feed */
    TEST_ASSERT_FALSE(reasm_feed(&r, &s1));
    TEST_ASSERT_FALSE(reasm_feed(&r, &s2));
    TEST_ASSERT_TRUE(reasm_feed(&r, &s3));

    char joined[1024];
    reasm_join(&r, joined, sizeof(joined));
    TEST_ASSERT_EQUAL_STRING((const char *)EXPECTED_UCS2_UTF8, joined);
}

void test_long_ucs2_reassembled_scrambled_order(void) {
    reasm_t r; reasm_reset(&r);
    pdu_sms_t s1, s2, s3;
    TEST_ASSERT_TRUE(pdu_decode(PDU_UCS2_PART1, &s1));
    TEST_ASSERT_TRUE(pdu_decode(PDU_UCS2_PART2, &s2));
    TEST_ASSERT_TRUE(pdu_decode(PDU_UCS2_PART3, &s3));

    /* arrive 3, 1, 2 -- final text must STILL be in logical order */
    TEST_ASSERT_FALSE(reasm_feed(&r, &s3));
    TEST_ASSERT_FALSE(reasm_feed(&r, &s1));
    TEST_ASSERT_TRUE(reasm_feed(&r, &s2));

    char joined[1024];
    reasm_join(&r, joined, sizeof(joined));
    TEST_ASSERT_EQUAL_STRING((const char *)EXPECTED_UCS2_UTF8, joined);
}

/* ====================================================================== */
/* Case B: long GSM 7-bit message, 2 parts (16-bit ref, 0 fill bits).      */
/* ====================================================================== */
/*
 * 16-bit concat UDH is 7 octets = 56 bits, an exact multiple of 7, so the
 * packed 7-bit data needs ZERO fill bits -- standard packing. Each part is
 * the known-good "hello" packing (E8329BFD06, verified by the existing
 * single-part GSM7 test). Reassembled => "hellohello".
 *
 *   00                SMSC len 0
 *   40                SMS-DELIVER + UDHI
 *   04 81 2143        OA: 4 digits national "1234"
 *   00                PID
 *   00                DCS = GSM 7-bit
 *   99309251619580    SCTS
 *   0D                UDL = 13 septets (8 for UDH + 5 for "hello")
 *   06 08 04 1234 TT PP   UDH: 16-bit concat ref=0x1234, total=TT, part=PP
 *   E8329BFD06        "hello" packed
 */
#define GSM7_PREFIX "004004812143000099309251619580" "0D"

static const char *PDU_GSM7_PART1 = GSM7_PREFIX "06080412340201" "E8329BFD06";
static const char *PDU_GSM7_PART2 = GSM7_PREFIX "06080412340202" "E8329BFD06";

void test_long_gsm7_multipart_header_decoded(void) {
    pdu_sms_t sms;
    TEST_ASSERT_TRUE(pdu_decode(PDU_GSM7_PART1, &sms));
    TEST_ASSERT_TRUE(sms.is_multipart); /* not flagged multipart -> would be split! */
    TEST_ASSERT_EQUAL_UINT16(0x1234, sms.ref_num);
    TEST_ASSERT_EQUAL_UINT8(2, sms.total_parts);
    TEST_ASSERT_EQUAL_UINT8(1, sms.part_num);
    /* UDH bit-alignment correctness: content must be exactly "hello",
       not shifted/garbled by the 7-octet header. */
    TEST_ASSERT_EQUAL_STRING("hello", sms.message);
}

void test_long_gsm7_reassembled(void) {
    reasm_t r; reasm_reset(&r);
    pdu_sms_t s1, s2;
    TEST_ASSERT_TRUE(pdu_decode(PDU_GSM7_PART1, &s1));
    TEST_ASSERT_TRUE(pdu_decode(PDU_GSM7_PART2, &s2));

    /* out of order: part 2 first */
    TEST_ASSERT_FALSE(reasm_feed(&r, &s2));
    TEST_ASSERT_TRUE(reasm_feed(&r, &s1));

    char joined[1024];
    reasm_join(&r, joined, sizeof(joined));
    TEST_ASSERT_EQUAL_STRING("hellohello", joined);
}

/* ====================================================================== */
/* Case C: a SINGLE long-ish message must NOT be flagged multipart.        */
/*   (guards the opposite failure: a normal SMS wrongly treated as a part) */
/* ====================================================================== */
void test_single_sms_not_multipart(void) {
    /* "hello" from "1234", no UDH (type 00) -- the verified single PDU */
    const char *pdu = "00000481214300009930925161958005E8329BFD06";
    pdu_sms_t sms;
    TEST_ASSERT_TRUE(pdu_decode(pdu, &sms));
    TEST_ASSERT_FALSE(sms.is_multipart); /* single SMS wrongly flagged multipart */
    TEST_ASSERT_EQUAL_STRING("hello", sms.message);
}

/* ====================================================================== */
/* Case D: UCS2 with emoji (UTF-16 surrogate pairs > U+FFFF).             */
/*   Regression for the "0xed invalid UTF-8 -> whole SMS dropped" bug.    */
/* ====================================================================== */
/*
 * Single-part UCS2 SMS body "<emoji>A":
 *   emoji U+1F4E9 (📩) = surrogate pair D83D DCE9  (4 octets)
 *   'A'              = 0041                       (2 octets)
 *   UDL = 06 octets, no UDH, DCS=08.
 * Expected UTF-8: F0 9F 93 A9 (📩) + 41 (A).
 */
void test_ucs2_emoji_surrogate_pair(void) {
    const char *pdu = "0000048121430008" "99309251619580" "06" "D83DDCE90041";
    pdu_sms_t sms;
    TEST_ASSERT_TRUE(pdu_decode(pdu, &sms));
    TEST_ASSERT_FALSE(sms.is_multipart);
    static const unsigned char expected[] = { 0xF0,0x9F,0x93,0xA9, 0x41, 0x00 };
    TEST_ASSERT_EQUAL_STRING((const char *)expected, sms.message);
}

/* An unpaired high surrogate must become U+FFFD (EF BF BD), never raw 0xED. */
void test_ucs2_unpaired_surrogate_becomes_replacement(void) {
    /* body = lone high surrogate D83D + 'A' (no low surrogate follows) */
    const char *pdu = "0000048121430008" "99309251619580" "04" "D83D0041";
    pdu_sms_t sms;
    TEST_ASSERT_TRUE(pdu_decode(pdu, &sms));
    static const unsigned char expected[] = { 0xEF,0xBF,0xBD, 0x41, 0x00 };
    TEST_ASSERT_EQUAL_STRING((const char *)expected, sms.message);
}

/* ===== Test Runner ===== */
void run_long_message_tests(void) {
    printf("\n=== Long / Concatenated Message Tests ===\n");
    RUN_TEST(test_long_ucs2_each_part_detected_as_multipart);
    RUN_TEST(test_long_ucs2_reassembled_in_order);
    RUN_TEST(test_long_ucs2_reassembled_scrambled_order);
    RUN_TEST(test_long_gsm7_multipart_header_decoded);
    RUN_TEST(test_long_gsm7_reassembled);
    RUN_TEST(test_single_sms_not_multipart);
    RUN_TEST(test_ucs2_emoji_surrogate_pair);
    RUN_TEST(test_ucs2_unpaired_surrogate_becomes_replacement);
}
