/*
 * EVO Player - Phase 1b task 4: small libc gap fillers for the app module.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The static SDK libc.a (linked last, for __emutls_get_address) already carries
 * real C-locale implementations of the xlocale `*_l` family (no-locale.o),
 * gmtime_r/localtime_r, nl_langinfo, __assert, posix_fadvise, etc. This file
 * only covers what libc.a still can't give us cleanly:
 *
 *   _setjmp / _longjmp   - not in this libc.a; alias to the plain forms
 *                          (which the console libc exports).
 *   dladdr               - pulling libc.a's dladdr.o drags in the private
 *                          rtld interface (__dlopen ...) that no SDK stub
 *                          resolves; a local no-op def keeps that object out.
 *   __dl* internals      - belt-and-braces stubs in case something else pulls
 *                          the rtld path; harmless if unreferenced.
 *   recvmmsg / sendmmsg  - absent everywhere; EVO's networking is TCP, and the
 *                          UDP callers all fall back per-message.
 *   getaddrinfo / etc    - now in evo_dns.c (#91): the console's own resolver,
 *                          not a hand-rolled DNS client.
 *
 * Compiled INTO eboot.bin as local defs (never imports, never NULL).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

#ifndef INADDR_ANY
#define INADDR_ANY ((uint32_t)0x00000000)
#endif
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((uint32_t)0x7f000001)
#endif

/* ------------------------------------------------------------------ setjmp -- */
/* _setjmp/_longjmp differ from setjmp/longjmp only by not touching the signal
 * mask. Tail-jump to the plain forms. */
__asm__(
    ".globl _setjmp\n"
    "_setjmp:\n"
    "    jmp *setjmp@GOTPCREL(%rip)\n"
    ".globl _longjmp\n"
    "_longjmp:\n"
    "    jmp *longjmp@GOTPCREL(%rip)\n"
);

/* --------------------------------------------------------------- dl* / dladdr */
int dladdr(const void *addr, void *info)
{
    (void)addr; (void)info;
    return 0;   /* "not found" - callers use it only for backtrace symbols */
}

void *__dlopen(const char *path, int mode) { (void)path; (void)mode; return NULL; }
void *__dlsym(void *h, const char *sym)    { (void)h; (void)sym; return NULL; }
int   __dlclose(void *h)                   { (void)h; return -1; }
char *__dlerror(void)                      { return (char *)"unsupported"; }

/* ------------------------------------------------------------ batch sockets -- */
int recvmmsg(int s, void *msgvec, unsigned int vlen, int flags, const void *timeout)
{
    (void)s; (void)msgvec; (void)vlen; (void)flags; (void)timeout;
    errno = ENOSYS;
    return -1;
}
int sendmmsg(int s, void *msgvec, unsigned int vlen, int flags)
{
    (void)s; (void)msgvec; (void)vlen; (void)flags;
    errno = ENOSYS;
    return -1;
}
