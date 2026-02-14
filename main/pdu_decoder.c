/**
 * @file pdu_decoder.c
 * @brief PDU SMS Decoder implementation
 */

#include <string.h>
#include <stdlib.h>
#include "pdu_decoder.h"
#include "esp_log.h"

static const char *TAG = "PDU_DECODER";

// --- Helper Functions ---

/**
 * @brief Convert hex character to nibble value
 */
static int hex_to_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/**
 * @brief Convert two hex characters to byte
 */
static int hex_to_byte(const char *hex) {
    int hi = hex_to_nibble(hex[0]);
    int lo = hex_to_nibble(hex[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

/**
 * @brief Decode semi-octet encoded phone number (swapped nibbles)
 * @param hex       Input hex string
 * @param num_digits Number of digits to decode
 * @param out       Output buffer
 * @param out_size  Output buffer size
 */
static void decode_phone_number(const char *hex, int num_digits, char *out, size_t out_size) {
    size_t j = 0;
    for (int i = 0; i < num_digits && j < out_size - 1; i++) {
        // Semi-octet: nibbles are swapped
        // hex[0] hex[1] -> digit 2, digit 1
        int hex_idx = (i / 2) * 2;
        char c;
        if (i % 2 == 0) {
            c = hex[hex_idx + 1]; // Low nibble first
        } else {
            c = hex[hex_idx];     // High nibble second
        }
        if (c == 'F' || c == 'f') break; // Padding
        out[j++] = c;
    }
    out[j] = '\0';
}

/**
 * @brief Decode GSM 7-bit packed data to UTF-8
 * @param hex           Input hex string (packed 7-bit data)
 * @param num_septets   Number of septets (characters)
 * @param udh_bits      Number of bits used by UDH (for alignment)
 * @param out           Output buffer
 * @param out_size      Output buffer size
 */
static void decode_gsm7bit(const char *hex, int num_septets, int udh_bits, char *out, size_t out_size) {
    // GSM 7-bit default alphabet (basic ASCII subset)
    static const char gsm7bit_basic[] = 
        "@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\x1bÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?"
        "¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà";
    
    // Convert hex to bytes
    size_t hex_len = strlen(hex);
    size_t byte_count = hex_len / 2;
    uint8_t *bytes = malloc(byte_count);
    if (!bytes) return;
    
    for (size_t i = 0; i < byte_count; i++) {
        int b = hex_to_byte(hex + i * 2);
        bytes[i] = (b >= 0) ? b : 0;
    }
    
    // Skip UDH fill bits
    int bit_offset = udh_bits % 7;
    if (bit_offset > 0) bit_offset = 7 - bit_offset;
    
    size_t out_idx = 0;
    int bit_pos = bit_offset;
    
    for (int sept = 0; sept < num_septets && out_idx < out_size - 1; sept++) {
        int byte_idx = bit_pos / 8;
        int bit_in_byte = bit_pos % 8;
        
        uint8_t septet;
        if (bit_in_byte <= 1) {
            // Septet fits in one byte
            septet = (bytes[byte_idx] >> bit_in_byte) & 0x7F;
        } else {
            // Septet spans two bytes
            septet = (bytes[byte_idx] >> bit_in_byte);
            if ((size_t)(byte_idx + 1) < byte_count) {
                septet |= (bytes[byte_idx + 1] << (8 - bit_in_byte));
            }
            septet &= 0x7F;
        }
        
        // Map to character (simplified - ASCII range only for now)
        if (septet < 128) {
            // Basic ASCII mapping for common characters
            if (septet >= 32 && septet < 127) {
                out[out_idx++] = (char)septet;
            } else if (septet == 0x0A) {
                out[out_idx++] = '\n';
            } else if (septet == 0x0D) {
                out[out_idx++] = '\r';
            } else {
                out[out_idx++] = '?';
            }
        }
        
        bit_pos += 7;
    }
    
    out[out_idx] = '\0';
    free(bytes);
}

/**
 * @brief Decode UCS2 (UTF-16BE) hex to UTF-8
 */
static void decode_ucs2(const char *hex, int num_octets, int udh_octets, char *out, size_t out_size) {
    const char *data_start = hex + (udh_octets * 2);
    int data_octets = num_octets - udh_octets;
    
    size_t j = 0;
    for (int i = 0; i + 3 < data_octets * 2 && j + 4 < out_size; i += 4) {
        int b1 = hex_to_byte(data_start + i);
        int b2 = hex_to_byte(data_start + i + 2);
        if (b1 < 0 || b2 < 0) continue;
        
        uint16_t wc = (b1 << 8) | b2;
        
        if (wc < 0x80) {
            out[j++] = (char)wc;
        } else if (wc < 0x800) {
            out[j++] = 0xC0 | (wc >> 6);
            out[j++] = 0x80 | (wc & 0x3F);
        } else {
            out[j++] = 0xE0 | (wc >> 12);
            out[j++] = 0x80 | ((wc >> 6) & 0x3F);
            out[j++] = 0x80 | (wc & 0x3F);
        }
    }
    out[j] = '\0';
}

// --- Main Decode Function ---

bool pdu_decode(const char *pdu_hex, pdu_sms_t *out) {
    if (!pdu_hex || !out) return false;
    
    memset(out, 0, sizeof(pdu_sms_t));
    
    size_t len = strlen(pdu_hex);
    if (len < 20) {
        ESP_LOGW(TAG, "PDU too short: %d chars", (int)len);
        return false;
    }
    
    int pos = 0;
    
    // 1. SMSC Length (skip SMSC info)
    int smsc_len = hex_to_byte(pdu_hex + pos);
    if (smsc_len < 0) return false;
    pos += 2 + (smsc_len * 2); // Skip SMSC length + SMSC data
    
    if ((size_t)pos >= len) return false;
    
    // 2. PDU Type (first octet)
    int pdu_type = hex_to_byte(pdu_hex + pos);
    if (pdu_type < 0) return false;
    pos += 2;
    
    // Check TP-MTI (bits 0-1): should be 00 for SMS-DELIVER
    if ((pdu_type & 0x03) != 0x00) {
        ESP_LOGW(TAG, "Not SMS-DELIVER: type=0x%02X", pdu_type);
        return false;
    }
    
    // Check TP-UDHI (bit 6): User Data Header present?
    bool has_udh = (pdu_type & 0x40) != 0;
    
    // 3. Originating Address (Sender)
    if ((size_t)pos + 2 > len) return false;
    int oa_len = hex_to_byte(pdu_hex + pos); // Number of digits
    pos += 2;
    
    if ((size_t)pos + 2 > len) return false;
    int oa_type = hex_to_byte(pdu_hex + pos);
    pos += 2;
    
    // Calculate octets for address (round up)
    int oa_octets = (oa_len + 1) / 2;
    if ((size_t)pos + oa_octets * 2 > len) return false;
    
    // Add '+' for international format
    int sender_offset = 0;
    if ((oa_type & 0x70) == 0x10) { // International
        out->sender[0] = '+';
        sender_offset = 1;
    }
    decode_phone_number(pdu_hex + pos, oa_len, out->sender + sender_offset, 
                        sizeof(out->sender) - sender_offset);
    pos += oa_octets * 2;
    
    // 4. Protocol Identifier (skip)
    pos += 2;
    
    // 5. Data Coding Scheme
    if ((size_t)pos + 2 > len) return false;
    int dcs = hex_to_byte(pdu_hex + pos);
    if (dcs < 0) return false;
    pos += 2;
    
    // 6. Timestamp (7 octets, skip)
    pos += 14;
    
    // 7. User Data Length
    if ((size_t)pos + 2 > len) return false;
    int udl = hex_to_byte(pdu_hex + pos);
    if (udl < 0) return false;
    pos += 2;
    
    // User Data starts here
    const char *ud_hex = pdu_hex + pos;
    int udh_octets = 0;
    
    // 8. Parse UDH if present
    if (has_udh) {
        int udhl = hex_to_byte(ud_hex);
        if (udhl < 0) return false;
        
        udh_octets = 1 + udhl; // UDHL + UDH content
        
        // Parse UDH Information Elements
        int ie_pos = 2; // After UDHL
        while (ie_pos < udh_octets * 2) {
            int iei = hex_to_byte(ud_hex + ie_pos);
            if (iei < 0) break;
            ie_pos += 2;
            
            int iel = hex_to_byte(ud_hex + ie_pos);
            if (iel < 0) break;
            ie_pos += 2;
            
            if (iei == 0x00 && iel == 3) {
                // Concatenated SMS, 8-bit reference
                int ref = hex_to_byte(ud_hex + ie_pos);
                int total = hex_to_byte(ud_hex + ie_pos + 2);
                int part = hex_to_byte(ud_hex + ie_pos + 4);
                
                if (ref >= 0 && total > 0 && part > 0) {
                    out->is_multipart = true;
                    out->ref_num = (uint16_t)ref;
                    out->total_parts = (uint8_t)total;
                    out->part_num = (uint8_t)part;
                    ESP_LOGI(TAG, "Multipart SMS: ref=%d, part %d/%d", ref, part, total);
                }
            } else if (iei == 0x08 && iel == 4) {
                // Concatenated SMS, 16-bit reference
                int ref_hi = hex_to_byte(ud_hex + ie_pos);
                int ref_lo = hex_to_byte(ud_hex + ie_pos + 2);
                int total = hex_to_byte(ud_hex + ie_pos + 4);
                int part = hex_to_byte(ud_hex + ie_pos + 6);
                
                if (ref_hi >= 0 && ref_lo >= 0 && total > 0 && part > 0) {
                    out->is_multipart = true;
                    out->ref_num = (uint16_t)((ref_hi << 8) | ref_lo);
                    out->total_parts = (uint8_t)total;
                    out->part_num = (uint8_t)part;
                    ESP_LOGI(TAG, "Multipart SMS (16-bit): ref=%d, part %d/%d", 
                             out->ref_num, part, total);
                }
            }
            
            ie_pos += iel * 2;
        }
    }
    
    // 9. Decode message content based on DCS
    // DCS coding groups (simplified):
    // 0x00-0x03: GSM 7-bit
    // 0x04-0x07: 8-bit data
    // 0x08-0x0F: UCS2
    
    if ((dcs & 0x0C) == 0x08) {
        // UCS2 encoding
        // UDL is in octets for UCS2
        decode_ucs2(ud_hex, udl, udh_octets, out->message, sizeof(out->message));
    } else {
        // GSM 7-bit encoding (default)
        // UDL is in septets (characters)
        int udh_bits = udh_octets * 8;
        int septets_for_udh = has_udh ? ((udh_bits + 6) / 7) : 0;
        int msg_septets = udl - septets_for_udh;
        
        decode_gsm7bit(ud_hex + (udh_octets * 2), msg_septets, udh_bits, 
                       out->message, sizeof(out->message));
    }
    
    ESP_LOGI(TAG, "Decoded: from=%s, msg=%s", out->sender, out->message);
    return true;
}
