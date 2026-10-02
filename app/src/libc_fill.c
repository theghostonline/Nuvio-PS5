/*
 * C library functions the app's runtime does not provide.
 *
 * The native-app libc (sce_module/libc.prx plus the system modules) leaves
 * some standard calls as NULL imports: the program links, and the first call
 * jumps to address 0 (engine/media/src/evo_stream_io.c notes it for
 * strcasestr). Defined here they are local to the eboot and never imported.
 * The startup import check (nuvio_import_check) logs any other NULL import.
 */
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char *strcasestr(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0)
        return (char *)hay;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0)
            return (char *)hay;
    return NULL;
}

char *strndup(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len])
        len++;
    char *out = (char *)malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, s, len);
    out[len] = 0;
    return out;
}

int sprintf(char *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out, INT_MAX, fmt, ap);
    va_end(ap);
    return n;
}
