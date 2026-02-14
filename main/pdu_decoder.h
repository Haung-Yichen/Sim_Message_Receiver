/**
 * @file pdu_decoder.h
 * @brief PDU SMS Decoder for GSM 03.40 format
 * 
 * Supports:
 * - SMS-DELIVER PDU parsing
 * - UDH (User Data Header) for concatenated SMS
 * - GSM 7-bit and UCS2 encoding
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PDU_MAX_SENDER_LEN   32
#define PDU_MAX_MESSAGE_LEN  512

/**
 * @brief Decoded SMS message structure
 */
typedef struct {
    char sender[PDU_MAX_SENDER_LEN];    // Sender phone number
    char message[PDU_MAX_MESSAGE_LEN];  // Message content (UTF-8)
    
    // Concatenated SMS info (valid when is_multipart == true)
    bool is_multipart;                  // True if part of concatenated SMS
    uint16_t ref_num;                   // Concatenation reference number
    uint8_t total_parts;                // Total number of parts
    uint8_t part_num;                   // Current part number (1-based)
} pdu_sms_t;

/**
 * @brief Decode a PDU hex string into SMS structure
 * 
 * @param pdu_hex   Hex string from AT+CMGL response (without length prefix)
 * @param out       Output structure to fill
 * @return true     Decode successful
 * @return false    Decode failed (malformed PDU)
 */
bool pdu_decode(const char *pdu_hex, pdu_sms_t *out);
