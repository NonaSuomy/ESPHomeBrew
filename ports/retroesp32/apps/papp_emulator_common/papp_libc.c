/* FILE objects belong to the launcher: never let app-newlib dereference them.
 * Track handles so a fatal core error can unwind without leaking SD files. */
#define PAPP_APP_SIDE 1
#include "psram_app.h"
#include <stdio.h>
#include <errno.h>
extern const app_services_t *_papp_svc;
static FILE *streams[32];

FILE *__wrap_fopen(const char *path, const char *mode)
{
    for (unsigned i=0; i<32; ++i) if (!streams[i]) {
        streams[i] = (FILE *)_papp_svc->file_open(path,mode);
        return streams[i];
    }
    errno = EMFILE;
    return NULL;
}
int __wrap_fclose(FILE *f)
{
    if (!f) return EOF;
    for (unsigned i=0; i<32; ++i) if (streams[i] == f) {
        streams[i] = NULL;
        return _papp_svc->file_close(f);
    }
    errno = EBADF;
    return EOF;
}
size_t __wrap_fread(void *p, size_t s, size_t n, FILE *f) { return _papp_svc->file_read(p,s,n,f); }
size_t __wrap_fwrite(const void *p, size_t s, size_t n, FILE *f) { return _papp_svc->file_write(p,s,n,f); }
int __wrap_fseek(FILE *f, long o, int w) { return _papp_svc->file_seek(f,o,w); }
long __wrap_ftell(FILE *f) { return _papp_svc->file_tell(f); }
void __wrap_rewind(FILE *f)
{
    if (_papp_svc && f)
        (void)_papp_svc->file_seek(f, 0, SEEK_SET);
}

/* Newlib's getc() normally dereferences its private FILE object. PAPP file
 * handles are opaque launcher-owned tokens, so route byte reads through the
 * service table just like fread(). Spectrum's tape/config readers use getc
 * extensively and otherwise fault as soon as they open a sidecar ROM. */
int __wrap_getc(FILE *f)
{
    unsigned char c = 0;
    if (!f || !_papp_svc || !_papp_svc->file_read ||
        _papp_svc->file_read(&c, 1, 1, f) != 1)
        return EOF;
    return (int)c;
}
int __wrap_fgetc(FILE *f) { return __wrap_getc(f); }

void papp_close_streams(void)
{
    for (unsigned i=0; i<32; ++i) if (streams[i]) __wrap_fclose(streams[i]);
}
