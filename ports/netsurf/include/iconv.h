// <iconv.h> for the NetSurf PAPP (papp_iconv.c): UTF-8, US-ASCII,
// ISO-8859-1, Windows-1252, UTF-16 and UTF-32, with //TRANSLIT. Page
// decoding does not use it: libparserutils has its own codecs
// (WITHOUT_ICONV_FILTER); NetSurf needs it for form data and text export.
#ifndef PAPP_ICONV_H
#define PAPP_ICONV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *iconv_t;

iconv_t iconv_open(const char *tocode, const char *fromcode);
size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft);
int iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif

#endif
