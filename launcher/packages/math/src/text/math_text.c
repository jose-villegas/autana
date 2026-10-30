/* math_text: scalar decimal conversion without allocation. */
#include "math/text/math_text.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static bool
text_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool
text_integer(const char* s, const char** end, int32_t* out, int32_t low, int32_t high) {
    const char* p = s;
    if (*p == '-' || *p == '+') {
        ++p;
    }
    if (!text_digit(*p)) {
        return false;
    }
    while (text_digit(*p)) {
        ++p;
    }
    if (end == NULL && *p != '\0') {
        return false;
    }
    errno = 0;
    char* stop;
    intmax_t value = strtoimax(s, &stop, MATH_TEXT_RADIX);
    if (errno == ERANGE || value < low || value > high) {
        return false;
    }
    *out = (int32_t)value;
    if (end != NULL) {
        *end = p;
    }
    return true;
}

int
mathi_format(char* buf, size_t cap, int32_t v) {
    return snprintf(buf, cap, "%" PRId32, v);
}

bool
mathi_parse(const char* s, const char** end, int32_t* out) {
    return text_integer(s, end, out, INT32_MIN, INT32_MAX);
}

static int
text_digit_value(char c, int radix) {
    int d = text_digit(c)               ? c - '0'
            : isalpha((unsigned char)c) ? tolower((unsigned char)c) - 'a' + MATH_TEXT_RADIX
                                        : radix;
    return d < radix ? d : -1;
}

static bool
text_unsigned(const char* s, const char** end, uint32_t* out, int radix) {
    const char* p = s;
    if (radix == MATH_TEXT_HEX_RADIX) {
        if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) {
            return false;
        }
        p += 2;
    }
    const char* digits = p;
    uint64_t value = 0;
    for (int d; (d = text_digit_value(*p, radix)) >= 0; ++p) {
        value = (value * (unsigned)radix) + (unsigned)d;
        if (value > UINT32_MAX) {
            return false;
        }
    }
    if (p == digits || (end == NULL && *p != '\0')) {
        return false;
    }
    *out = (uint32_t)value;
    if (end != NULL) {
        *end = p;
    }
    return true;
}

int
mathu_format(char* buf, size_t cap, uint32_t v) {
    return snprintf(buf, cap, "%" PRIu32, v);
}

bool
mathu_parse(const char* s, const char** end, uint32_t* out) {
    return text_unsigned(s, end, out, MATH_TEXT_RADIX);
}

bool
mathu_parse_hex(const char* s, const char** end, uint32_t* out) {
    return text_unsigned(s, end, out, MATH_TEXT_HEX_RADIX);
}

int
maths_format(char* buf, size_t cap, int16_t v) {
    return mathi_format(buf, cap, v);
}

bool
maths_parse(const char* s, const char** end, int16_t* out) {
    int32_t value;
    if (!text_integer(s, end, &value, INT16_MIN, INT16_MAX)) {
        return false;
    }
    *out = (int16_t)value;
    return true;
}

static const char*
text_exponent_end(const char* p) {
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '-' || *p == '+') {
            ++p;
        }
        if (!text_digit(*p)) {
            return NULL;
        }
        while (text_digit(*p)) {
            ++p;
        }
    }
    return p;
}

static const char*
text_decimal_end(const char* s) {
    const char* p = s;
    if (*p == '-' || *p == '+') {
        ++p;
    }
    bool digits = text_digit(*p);
    while (text_digit(*p)) {
        ++p;
    }
    if (*p == '.') {
        ++p;
        digits |= text_digit(*p);
        while (text_digit(*p)) {
            ++p;
        }
    }
    if (!digits) {
        return NULL;
    }
    return text_exponent_end(p);
}

static float
text_float_value(const char* s, const char* p, char** stop) {
    float value;
    const char* number = (*s == '+' || *s == '-') ? s + 1 : s;
    if (number[0] == '0' && p == number + 1 && (*p == 'x' || *p == 'X')) {
        value = *s == '-' ? -0.0F : 0.0F;
        *stop = (char*)p;
    } else {
        value = strtof(s, stop);
    }
    return value;
}

bool
mathf_parse(const char* s, const char** end, float* out) {
    const char* p = s;
    if (*p == '-' || *p == '+') {
        ++p;
    }
    bool special = strncmp(p, "inf", sizeof "inf" - 1) == 0 || strncmp(p, "nan", sizeof "nan" - 1) == 0;
    p = special ? p + sizeof "inf" - 1 : text_decimal_end(s);
    if (p == NULL || (end == NULL && *p != '\0')) {
        return false;
    }
    char* stop;
    errno = 0;
    float value = text_float_value(s, p, &stop);
    if (stop != p || (!special && (isinf(value) || (errno == ERANGE && value == 0.0F)))) {
        return false;
    }
    *out = value;
    if (end != NULL) {
        *end = p;
    }
    return true;
}

int
mathf_format(char* buf, size_t cap, float v) {
    char text[MATHF_TEXT_MAX];
    if (isnan(v)) {
        return math_text_copy(buf, cap, "nan", sizeof "nan" - 1);
    }
    int length = 0;
    for (int precision = 1; precision <= MATHF_TEXT_PRECISION; ++precision) {
        length = snprintf(text, sizeof text, "%.*g", precision, (double)v);
        float parsed = strtof(text, NULL);
        if (memcmp(&v, &parsed, sizeof v) == 0) {
            break;
        }
    }
    return math_text_copy(buf, cap, text, length);
}

static bool
text_fixed_whole(const char** cursor, uint64_t limit, uint64_t* out) {
    const char* p = *cursor;
    uint64_t whole = 0;
    while (text_digit(*p)) {
        whole = (whole * MATH_TEXT_RADIX) + (unsigned)(*p++ - '0');
        if (whole > limit / MATHX_ONE) {
            return false;
        }
    }
    *cursor = p;
    *out = whole;
    return true;
}

static uint32_t
text_fixed_fraction(const char** cursor, unsigned* remainder, bool* residual) {
    const char* first = *cursor;
    const char* p = first;
    while (text_digit(*p)) {
        ++p;
    }
    *cursor = p;
    uint32_t fraction = 0;
    while (p != first) {
        uint32_t scaled = ((unsigned)(*--p - '0') * MATHX_ONE) + fraction;
        fraction = scaled / MATH_TEXT_RADIX;
        *remainder = scaled % MATH_TEXT_RADIX;
        *residual |= *remainder != 0;
    }
    return fraction;
}

bool
mathx_parse(const char* s, const char** end, int32_t* out) {
    const char* p = s;
    bool negative = *p == '-';
    if (*p == '+' || *p == '-') {
        ++p;
    }
    bool digits = text_digit(*p);
    uint64_t whole;
    const uint64_t limit = (uint64_t)INT32_MAX + (negative ? 1 : 0);
    if (!text_fixed_whole(&p, limit, &whole)) {
        return false;
    }
    uint32_t fraction = 0;
    unsigned remainder = 0;
    bool residual = false;
    if (*p == '.') {
        ++p;
        digits |= text_digit(*p);
        fraction = text_fixed_fraction(&p, &remainder, &residual);
    }
    if (!digits || (end == NULL && *p != '\0')) {
        return false;
    }
    uint64_t raw = (whole * MATHX_ONE) + fraction;
    if (raw > limit || (raw == limit && residual)) {
        return false;
    }
    raw += remainder >= MATH_TEXT_RADIX / 2;
    if (raw > limit) {
        return false;
    }
    *out = negative ? (int32_t)-(int64_t)raw : (int32_t)raw;
    if (end != NULL) {
        *end = p;
    }
    return true;
}

int
mathx_format(char* buf, size_t cap, int32_t v) {
    const uint64_t magnitude = v < 0 ? (uint64_t)-(int64_t)v : (uint64_t)v;
    uint64_t scale = 1;
    char text[MATHX_TEXT_MAX];
    int length = 0;
    for (int digits = 0; digits <= MATHX_TEXT_FRACTION; ++digits) {
        uint64_t decimal = ((magnitude * scale) + (MATHX_ONE / 2)) / MATHX_ONE;
        if (digits == 0) {
            length = snprintf(text, sizeof text, "%s%" PRIu64, v < 0 ? "-" : "", decimal);
        } else {
            length = snprintf(text, sizeof text, "%s%" PRIu64 ".%0*" PRIu64, v < 0 ? "-" : "", decimal / scale, digits,
                              decimal % scale);
        }
        int32_t parsed;
        if (mathx_parse(text, NULL, &parsed) && parsed == v) {
            break;
        }
        scale *= MATH_TEXT_RADIX;
    }
    return math_text_copy(buf, cap, text, length);
}
