/**
 * @file argus_browser_command.c
 * @brief Strict, allocation-free Phase 4B.4 browser command decoder.
 */

#include "argus_browser_command.h"

#include <limits.h>
#include <string.h>

typedef struct {
    const uint8_t *body;
    size_t len;
    size_t pos;
} argus_browser_json_parser_t;

typedef enum {
    FIELD_COMMAND = 0,
    FIELD_TARGET_RPM_MILLI,
    FIELD_FORWARD,
    FIELD_UNKNOWN,
} argus_browser_field_t;

enum {
    SEEN_COMMAND = 1U << 0,
    SEEN_TARGET = 1U << 1,
    SEEN_FORWARD = 1U << 2,
};

static void invalidate_request(argus_browser_command_request_t *request)
{
    if (request == NULL) {
        return;
    }

    memset(request, 0, sizeof(*request));
    request->command_type = (argus_cmd_type_t)-1;
}

static bool is_json_whitespace(uint8_t ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void skip_whitespace(argus_browser_json_parser_t *parser)
{
    while (parser->pos < parser->len &&
           is_json_whitespace(parser->body[parser->pos])) {
        parser->pos++;
    }
}

static bool is_value_delimiter(const argus_browser_json_parser_t *parser)
{
    if (parser->pos >= parser->len) {
        return true;
    }

    uint8_t ch = parser->body[parser->pos];
    return is_json_whitespace(ch) || ch == ',' || ch == '}';
}

static int hex_value(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool parse_hex_quad(argus_browser_json_parser_t *parser,
                           uint32_t *out_value)
{
    if (parser->len - parser->pos < 4U) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < 4U; i++) {
        int digit = hex_value(parser->body[parser->pos++]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (uint32_t)digit;
    }

    *out_value = value;
    return true;
}

static bool append_byte(char *out, size_t out_size, size_t *out_len,
                        uint8_t value)
{
    if (value == 0 || *out_len + 1U >= out_size) {
        return false;
    }
    out[(*out_len)++] = (char)value;
    return true;
}

static bool append_codepoint(char *out, size_t out_size, size_t *out_len,
                             uint32_t codepoint)
{
    if (codepoint == 0 || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return false;
    }

    if (codepoint <= 0x7FU) {
        return append_byte(out, out_size, out_len, (uint8_t)codepoint);
    }
    if (codepoint <= 0x7FFU) {
        return append_byte(out, out_size, out_len,
                           (uint8_t)(0xC0U | (codepoint >> 6))) &&
               append_byte(out, out_size, out_len,
                           (uint8_t)(0x80U | (codepoint & 0x3FU)));
    }
    if (codepoint <= 0xFFFFU) {
        return append_byte(out, out_size, out_len,
                           (uint8_t)(0xE0U | (codepoint >> 12))) &&
               append_byte(out, out_size, out_len,
                           (uint8_t)(0x80U | ((codepoint >> 6) & 0x3FU))) &&
               append_byte(out, out_size, out_len,
                           (uint8_t)(0x80U | (codepoint & 0x3FU)));
    }

    return append_byte(out, out_size, out_len,
                       (uint8_t)(0xF0U | (codepoint >> 18))) &&
           append_byte(out, out_size, out_len,
                       (uint8_t)(0x80U | ((codepoint >> 12) & 0x3FU))) &&
           append_byte(out, out_size, out_len,
                       (uint8_t)(0x80U | ((codepoint >> 6) & 0x3FU))) &&
           append_byte(out, out_size, out_len,
                       (uint8_t)(0x80U | (codepoint & 0x3FU)));
}

static bool parse_json_string(argus_browser_json_parser_t *parser,
                              char *out, size_t out_size)
{
    if (parser->pos >= parser->len || parser->body[parser->pos] != '"' ||
        out == NULL || out_size == 0U) {
        return false;
    }

    parser->pos++;
    size_t out_len = 0;

    while (parser->pos < parser->len) {
        uint8_t ch = parser->body[parser->pos++];
        if (ch == '"') {
            out[out_len] = '\0';
            return true;
        }
        if (ch < 0x20U) {
            return false;
        }
        if (ch != '\\') {
            if (!append_byte(out, out_size, &out_len, ch)) {
                return false;
            }
            continue;
        }

        if (parser->pos >= parser->len) {
            return false;
        }

        uint8_t escape = parser->body[parser->pos++];
        switch (escape) {
            case '"': case '\\': case '/':
                if (!append_byte(out, out_size, &out_len, escape)) return false;
                break;
            case 'b':
                if (!append_byte(out, out_size, &out_len, '\b')) return false;
                break;
            case 'f':
                if (!append_byte(out, out_size, &out_len, '\f')) return false;
                break;
            case 'n':
                if (!append_byte(out, out_size, &out_len, '\n')) return false;
                break;
            case 'r':
                if (!append_byte(out, out_size, &out_len, '\r')) return false;
                break;
            case 't':
                if (!append_byte(out, out_size, &out_len, '\t')) return false;
                break;
            case 'u': {
                uint32_t codepoint = 0;
                if (!parse_hex_quad(parser, &codepoint)) return false;
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (parser->len - parser->pos < 6U ||
                        parser->body[parser->pos] != '\\' ||
                        parser->body[parser->pos + 1U] != 'u') {
                        return false;
                    }
                    parser->pos += 2U;
                    uint32_t low = 0;
                    if (!parse_hex_quad(parser, &low) ||
                        low < 0xDC00U || low > 0xDFFFU) {
                        return false;
                    }
                    codepoint = 0x10000U +
                                ((codepoint - 0xD800U) << 10) +
                                (low - 0xDC00U);
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    return false;
                }
                if (!append_codepoint(out, out_size, &out_len, codepoint)) {
                    return false;
                }
                break;
            }
            default:
                return false;
        }
    }

    return false;
}

static argus_browser_field_t identify_field(const char *key)
{
    if (strcmp(key, "command") == 0) return FIELD_COMMAND;
    if (strcmp(key, "target_rpm_milli") == 0) return FIELD_TARGET_RPM_MILLI;
    if (strcmp(key, "forward") == 0) return FIELD_FORWARD;
    return FIELD_UNKNOWN;
}

static bool map_command(const char *command, argus_cmd_type_t *out_type)
{
    static const struct {
        const char *name;
        argus_cmd_type_t type;
    } mappings[] = {
        {"set_target", ARGUS_CMD_TYPE_SET_TARGET},
        {"start", ARGUS_CMD_TYPE_START},
        {"stop", ARGUS_CMD_TYPE_STOP_NORMAL},
        {"unlock", ARGUS_CMD_TYPE_UNLOCK},
        {"estop", ARGUS_CMD_TYPE_ESTOP},
        {"reset_estop", ARGUS_CMD_TYPE_RESET_ESTOP},
        {"recover", ARGUS_CMD_TYPE_RECOVER},
    };

    for (size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); i++) {
        if (strcmp(command, mappings[i].name) == 0) {
            *out_type = mappings[i].type;
            return true;
        }
    }
    return false;
}

static argus_browser_command_decode_result_t parse_command_value(
    argus_browser_json_parser_t *parser,
    argus_cmd_type_t *out_type)
{
    if (parser->pos >= parser->len) {
        return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
    }
    if (parser->body[parser->pos] != '"') {
        return ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE;
    }

    char command[20];
    if (!parse_json_string(parser, command, sizeof(command))) {
        return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
    }
    if (!map_command(command, out_type)) {
        return ARGUS_BROWSER_CMD_DECODE_UNSUPPORTED_COMMAND;
    }
    return ARGUS_BROWSER_CMD_DECODE_OK;
}

static argus_browser_command_decode_result_t parse_target_value(
    argus_browser_json_parser_t *parser,
    int32_t *out_target)
{
    if (parser->pos >= parser->len) {
        return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
    }

    bool negative = false;
    if (parser->body[parser->pos] == '-') {
        negative = true;
        parser->pos++;
    }

    if (parser->pos >= parser->len) {
        return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
    }
    if (parser->body[parser->pos] < '0' || parser->body[parser->pos] > '9') {
        return ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE;
    }

    uint64_t value = 0;
    if (parser->body[parser->pos] == '0') {
        parser->pos++;
        if (parser->pos < parser->len &&
            parser->body[parser->pos] >= '0' && parser->body[parser->pos] <= '9') {
            return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
        }
    } else {
        while (parser->pos < parser->len &&
               parser->body[parser->pos] >= '0' && parser->body[parser->pos] <= '9') {
            uint8_t digit = (uint8_t)(parser->body[parser->pos++] - '0');
            if (value > (UINT64_MAX - digit) / 10U) {
                return ARGUS_BROWSER_CMD_DECODE_VALUE_OUT_OF_RANGE;
            }
            value = value * 10U + digit;
        }
    }

    if (parser->pos < parser->len &&
        (parser->body[parser->pos] == '.' ||
         parser->body[parser->pos] == 'e' ||
         parser->body[parser->pos] == 'E')) {
        return ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE;
    }
    if (!is_value_delimiter(parser)) {
        return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
    }
    if (negative || value > ARGUS_BROWSER_COMMAND_MAX_TARGET_RPM_MILLI) {
        return ARGUS_BROWSER_CMD_DECODE_VALUE_OUT_OF_RANGE;
    }

    *out_target = (int32_t)value;
    return ARGUS_BROWSER_CMD_DECODE_OK;
}

static argus_browser_command_decode_result_t parse_forward_value(
    argus_browser_json_parser_t *parser,
    bool *out_forward)
{
    static const uint8_t true_value[] = {'t', 'r', 'u', 'e'};
    static const uint8_t false_value[] = {'f', 'a', 'l', 's', 'e'};

    if (parser->pos >= parser->len) {
        return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
    }

    if (parser->len - parser->pos >= sizeof(true_value) &&
        memcmp(parser->body + parser->pos, true_value, sizeof(true_value)) == 0) {
        parser->pos += sizeof(true_value);
        if (!is_value_delimiter(parser)) return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
        *out_forward = true;
        return ARGUS_BROWSER_CMD_DECODE_OK;
    }
    if (parser->len - parser->pos >= sizeof(false_value) &&
        memcmp(parser->body + parser->pos, false_value, sizeof(false_value)) == 0) {
        parser->pos += sizeof(false_value);
        if (!is_value_delimiter(parser)) return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
        *out_forward = false;
        return ARGUS_BROWSER_CMD_DECODE_OK;
    }
    return ARGUS_BROWSER_CMD_DECODE_INVALID_TYPE;
}

argus_browser_command_decode_result_t argus_browser_command_decode(
    const uint8_t *body,
    size_t body_len,
    argus_browser_command_request_t *out_request)
{
    if (out_request == NULL) {
        return ARGUS_BROWSER_CMD_DECODE_INVALID_ARGUMENT;
    }
    invalidate_request(out_request);

    if (body == NULL && body_len != 0U) {
        return ARGUS_BROWSER_CMD_DECODE_INVALID_ARGUMENT;
    }
    if (body_len > ARGUS_BROWSER_COMMAND_MAX_BODY_LEN) {
        return ARGUS_BROWSER_CMD_DECODE_BODY_TOO_LARGE;
    }

    argus_browser_json_parser_t parser = {
        .body = body,
        .len = body_len,
        .pos = 0,
    };
    skip_whitespace(&parser);
    if (parser.pos == parser.len) {
        return ARGUS_BROWSER_CMD_DECODE_EMPTY_BODY;
    }
    if (parser.body[parser.pos] != '{') {
        return ARGUS_BROWSER_CMD_DECODE_TOP_LEVEL_NOT_OBJECT;
    }
    parser.pos++;

    argus_browser_command_request_t candidate;
    invalidate_request(&candidate);
    uint8_t seen_fields = 0;

    skip_whitespace(&parser);
    if (parser.pos < parser.len && parser.body[parser.pos] == '}') {
        parser.pos++;
    } else {
        while (true) {
            char key[24];
            if (!parse_json_string(&parser, key, sizeof(key))) {
                return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
            }
            skip_whitespace(&parser);
            if (parser.pos >= parser.len || parser.body[parser.pos++] != ':') {
                return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
            }
            skip_whitespace(&parser);

            argus_browser_field_t field = identify_field(key);
            if (field == FIELD_UNKNOWN) {
                return ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD;
            }

            uint8_t field_bit = (uint8_t)(1U << (unsigned)field);
            if ((seen_fields & field_bit) != 0U) {
                return ARGUS_BROWSER_CMD_DECODE_DUPLICATE_FIELD;
            }
            seen_fields |= field_bit;

            argus_browser_command_decode_result_t result;
            switch (field) {
                case FIELD_COMMAND:
                    result = parse_command_value(&parser, &candidate.command_type);
                    break;
                case FIELD_TARGET_RPM_MILLI:
                    result = parse_target_value(&parser, &candidate.target_rpm_milli);
                    break;
                case FIELD_FORWARD:
                    result = parse_forward_value(&parser, &candidate.forward);
                    break;
                default:
                    result = ARGUS_BROWSER_CMD_DECODE_UNKNOWN_FIELD;
                    break;
            }
            if (result != ARGUS_BROWSER_CMD_DECODE_OK) {
                return result;
            }

            skip_whitespace(&parser);
            if (parser.pos >= parser.len) {
                return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
            }
            if (parser.body[parser.pos] == '}') {
                parser.pos++;
                break;
            }
            if (parser.body[parser.pos] != ',') {
                return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
            }
            parser.pos++;
            skip_whitespace(&parser);
        }
    }

    skip_whitespace(&parser);
    if (parser.pos != parser.len) {
        return ARGUS_BROWSER_CMD_DECODE_MALFORMED_JSON;
    }
    if ((seen_fields & SEEN_COMMAND) == 0U) {
        return ARGUS_BROWSER_CMD_DECODE_MISSING_FIELD;
    }

    if (candidate.command_type == ARGUS_CMD_TYPE_SET_TARGET) {
        if ((seen_fields & (SEEN_TARGET | SEEN_FORWARD)) !=
            (SEEN_TARGET | SEEN_FORWARD)) {
            return ARGUS_BROWSER_CMD_DECODE_MISSING_FIELD;
        }
    } else if ((seen_fields & (SEEN_TARGET | SEEN_FORWARD)) != 0U) {
        return ARGUS_BROWSER_CMD_DECODE_UNEXPECTED_FIELD;
    }

    candidate.is_valid = true;
    *out_request = candidate;
    return ARGUS_BROWSER_CMD_DECODE_OK;
}
