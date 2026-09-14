// A small iconv for the NetSurf PAPP (the toolchain's newlib has none built
// in). NetSurf converts between UTF-8 and a page's encoding for form data
// (falling back to ISO-8859-1//TRANSLIT) and to Windows-1252 for text export;
// page decoding itself uses libparserutils' codecs. Supported: UTF-8,
// US-ASCII, ISO-8859-1, Windows-1252, UTF-16 and UTF-32 (BE/LE). With
// //TRANSLIT an unrepresentable character becomes '?'.
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "iconv.h"

enum enc { ENC_UTF8, ENC_ASCII, ENC_LATIN1, ENC_CP1252, ENC_UTF16BE, ENC_UTF16LE, ENC_UTF32BE, ENC_UTF32LE };

struct papp_iconv {
    enum enc from;
    enum enc to;
    bool translit;
};

// Windows-1252 0x80..0x9F (0 where the code point is unassigned).
static const uint16_t cp1252_high[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

static bool parse_name(const char *name, enum enc *out, bool *translit)
{
    char base[32];
    size_t n = 0;
    while (name[n] != '\0' && name[n] != '/' && n < sizeof(base) - 1) {
        base[n] = name[n];
        n++;
    }
    base[n] = '\0';
    const char *suffix = strchr(name, '/');
    if (suffix != NULL && strcasecmp(suffix, "//TRANSLIT") == 0) {
        *translit = true;
    }
    static const struct {
        const char *name;
        enum enc enc;
    } names[] = {
        {"UTF-8", ENC_UTF8},          {"UTF8", ENC_UTF8},
        {"US-ASCII", ENC_ASCII},      {"ASCII", ENC_ASCII},       {"ANSI_X3.4-1968", ENC_ASCII},
        {"ISO-8859-1", ENC_LATIN1},   {"ISO8859-1", ENC_LATIN1},  {"ISO_8859-1", ENC_LATIN1},
        {"LATIN1", ENC_LATIN1},       {"L1", ENC_LATIN1},
        {"CP1252", ENC_CP1252},       {"WINDOWS-1252", ENC_CP1252},
        {"UTF-16", ENC_UTF16BE},      {"UTF-16BE", ENC_UTF16BE},  {"UTF-16LE", ENC_UTF16LE},
        {"UCS-2", ENC_UTF16BE},       {"UCS-2BE", ENC_UTF16BE},   {"UCS-2LE", ENC_UTF16LE},
        {"UTF-32", ENC_UTF32BE},      {"UTF-32BE", ENC_UTF32BE},  {"UTF-32LE", ENC_UTF32LE},
        {"UCS-4", ENC_UTF32BE},       {"UCS-4BE", ENC_UTF32BE},   {"UCS-4LE", ENC_UTF32LE},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(base, names[i].name) == 0) {
            *out = names[i].enc;
            return true;
        }
    }
    return false;
}

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
    enum enc from, to;
    bool translit = false, ignored = false;
    if (tocode == NULL || fromcode == NULL || !parse_name(tocode, &to, &translit) ||
        !parse_name(fromcode, &from, &ignored)) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    struct papp_iconv *cd = malloc(sizeof(*cd));
    if (cd == NULL) {
        errno = ENOMEM;
        return (iconv_t)-1;
    }
    cd->from = from;
    cd->to = to;
    cd->translit = translit;
    return cd;
}

int iconv_close(iconv_t cd)
{
    if (cd == (iconv_t)-1 || cd == NULL) {
        errno = EBADF;
        return -1;
    }
    free(cd);
    return 0;
}

// Decode one character: returns bytes used, 0 for an incomplete sequence at
// the end of the input, -1 for an invalid one.
static int decode(enum enc enc, const uint8_t *in, size_t len, uint32_t *uc)
{
    switch (enc) {
    case ENC_ASCII:
        if (in[0] > 0x7F) {
            return -1;
        }
        *uc = in[0];
        return 1;
    case ENC_LATIN1:
        *uc = in[0];
        return 1;
    case ENC_CP1252:
        if (in[0] >= 0x80 && in[0] <= 0x9F) {
            *uc = cp1252_high[in[0] - 0x80];
            return *uc ? 1 : -1;
        }
        *uc = in[0];
        return 1;
    case ENC_UTF16BE:
    case ENC_UTF16LE: {
        if (len < 2) {
            return 0;
        }
        uint32_t u = enc == ENC_UTF16BE ? (uint32_t)(in[0] << 8 | in[1]) : (uint32_t)(in[1] << 8 | in[0]);
        if (u >= 0xD800 && u < 0xDC00) {
            if (len < 4) {
                return 0;
            }
            uint32_t lo = enc == ENC_UTF16BE ? (uint32_t)(in[2] << 8 | in[3]) : (uint32_t)(in[3] << 8 | in[2]);
            if (lo < 0xDC00 || lo > 0xDFFF) {
                return -1;
            }
            *uc = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            return 4;
        }
        if (u >= 0xDC00 && u <= 0xDFFF) {
            return -1;
        }
        *uc = u;
        return 2;
    }
    case ENC_UTF32BE:
    case ENC_UTF32LE:
        if (len < 4) {
            return 0;
        }
        *uc = enc == ENC_UTF32BE ? ((uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3])
                                 : ((uint32_t)in[3] << 24 | (uint32_t)in[2] << 16 | (uint32_t)in[1] << 8 | in[0]);
        return *uc > 0x10FFFF ? -1 : 4;
    case ENC_UTF8:
    default: {
        const uint8_t c = in[0];
        int n;
        uint32_t u;
        if (c < 0x80) {
            *uc = c;
            return 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            n = 2;
            u = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            n = 3;
            u = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            n = 4;
            u = c & 0x07;
        } else {
            return -1;
        }
        for (int i = 1; i < n; i++) {
            if ((size_t)i >= len) {
                return 0;
            }
            if ((in[i] & 0xC0) != 0x80) {
                return -1;
            }
            u = (u << 6) | (in[i] & 0x3F);
        }
        if ((n == 3 && u < 0x800) || (n == 4 && (u < 0x10000 || u > 0x10FFFF)) || (u >= 0xD800 && u <= 0xDFFF)) {
            return -1;
        }
        *uc = u;
        return n;
    }
    }
}

// Encode one character: returns bytes written, 0 when it does not fit,
// -1 when the target cannot represent it.
static int encode(enum enc enc, uint32_t uc, uint8_t *out, size_t room)
{
    switch (enc) {
    case ENC_ASCII:
        if (uc > 0x7F) {
            return -1;
        }
        if (room < 1) {
            return 0;
        }
        out[0] = (uint8_t)uc;
        return 1;
    case ENC_LATIN1:
        if (uc > 0xFF) {
            return -1;
        }
        if (room < 1) {
            return 0;
        }
        out[0] = (uint8_t)uc;
        return 1;
    case ENC_CP1252: {
        int b = -1;
        if (uc < 0x80 || (uc >= 0xA0 && uc <= 0xFF)) {
            b = (int)uc;
        } else {
            for (int i = 0; i < 32; i++) {
                if (cp1252_high[i] != 0 && cp1252_high[i] == uc) {
                    b = 0x80 + i;
                }
            }
        }
        if (b < 0) {
            return -1;
        }
        if (room < 1) {
            return 0;
        }
        out[0] = (uint8_t)b;
        return 1;
    }
    case ENC_UTF16BE:
    case ENC_UTF16LE: {
        uint16_t units[2];
        int n = 1;
        if (uc >= 0x10000) {
            uc -= 0x10000;
            units[0] = (uint16_t)(0xD800 + (uc >> 10));
            units[1] = (uint16_t)(0xDC00 + (uc & 0x3FF));
            n = 2;
        } else {
            units[0] = (uint16_t)uc;
        }
        if (room < (size_t)n * 2) {
            return 0;
        }
        for (int i = 0; i < n; i++) {
            out[i * 2 + (enc == ENC_UTF16BE ? 0 : 1)] = (uint8_t)(units[i] >> 8);
            out[i * 2 + (enc == ENC_UTF16BE ? 1 : 0)] = (uint8_t)(units[i] & 0xFF);
        }
        return n * 2;
    }
    case ENC_UTF32BE:
    case ENC_UTF32LE:
        if (room < 4) {
            return 0;
        }
        for (int i = 0; i < 4; i++) {
            const uint8_t v = (uint8_t)(uc >> (8 * (3 - i)));
            out[enc == ENC_UTF32BE ? i : 3 - i] = v;
        }
        return 4;
    case ENC_UTF8:
    default:
        if (uc < 0x80) {
            if (room < 1) {
                return 0;
            }
            out[0] = (uint8_t)uc;
            return 1;
        } else if (uc < 0x800) {
            if (room < 2) {
                return 0;
            }
            out[0] = (uint8_t)(0xC0 | (uc >> 6));
            out[1] = (uint8_t)(0x80 | (uc & 0x3F));
            return 2;
        } else if (uc < 0x10000) {
            if (room < 3) {
                return 0;
            }
            out[0] = (uint8_t)(0xE0 | (uc >> 12));
            out[1] = (uint8_t)(0x80 | ((uc >> 6) & 0x3F));
            out[2] = (uint8_t)(0x80 | (uc & 0x3F));
            return 3;
        }
        if (room < 4) {
            return 0;
        }
        out[0] = (uint8_t)(0xF0 | (uc >> 18));
        out[1] = (uint8_t)(0x80 | ((uc >> 12) & 0x3F));
        out[2] = (uint8_t)(0x80 | ((uc >> 6) & 0x3F));
        out[3] = (uint8_t)(0x80 | (uc & 0x3F));
        return 4;
    }
}

size_t iconv(iconv_t handle, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft)
{
    struct papp_iconv *cd = handle;
    if (cd == NULL || cd == (iconv_t)-1) {
        errno = EBADF;
        return (size_t)-1;
    }
    if (inbuf == NULL || *inbuf == NULL) {
        return 0;  // no shift state to reset
    }
    size_t irreversible = 0;
    const uint8_t *in = (const uint8_t *)*inbuf;
    size_t in_left = *inbytesleft;
    uint8_t *out = (uint8_t *)*outbuf;
    size_t out_left = *outbytesleft;
    size_t result = 0;
    while (in_left > 0) {
        uint32_t uc = 0;
        int used = decode(cd->from, in, in_left, &uc);
        if (used == 0) {
            errno = EINVAL;
            result = (size_t)-1;
            break;
        }
        if (used < 0) {
            if (!cd->translit) {
                errno = EILSEQ;
                result = (size_t)-1;
                break;
            }
            used = 1;
            uc = '?';
            irreversible++;
        }
        int wrote = encode(cd->to, uc, out, out_left);
        if (wrote < 0) {
            if (!cd->translit) {
                errno = EILSEQ;
                result = (size_t)-1;
                break;
            }
            wrote = encode(cd->to, '?', out, out_left);
            irreversible++;
        }
        if (wrote == 0) {
            errno = E2BIG;
            result = (size_t)-1;
            break;
        }
        in += used;
        in_left -= (size_t)used;
        out += wrote;
        out_left -= (size_t)wrote;
    }
    *inbuf = (char *)in;
    *inbytesleft = in_left;
    *outbuf = (char *)out;
    *outbytesleft = out_left;
    return result == (size_t)-1 ? result : irreversible;
}
