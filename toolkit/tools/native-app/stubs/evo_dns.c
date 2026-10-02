/*
 * EVO Player - name resolution for the app module (#91).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * On retail PS5 FW 12.70 getaddrinfo / freeaddrinfo / getnameinfo /
 * gai_strerror are exported only by libScePosixForWebKit.sprx, and every symbol
 * in that stub resolves to the same address (a NULL stub): calling one faults
 * at rip=0. So this file provides them, compiled INTO eboot.bin as local defs
 * (never imports, never NULL).
 *
 * The lookup itself is the console's own resolver, libSceNet's
 * sceNetResolver*, so it uses whatever DNS the user configured in the PS5's
 * network settings - the same servers every other app on the machine uses.
 * The call sequence is the one the SDK's own libc.a (netdb.o) uses on payloads:
 *
 *     memid = sceNetPoolCreate(name, 16 KiB, 0)
 *     rid   = sceNetResolverCreate(name, memid, 0)
 *     sceNetResolverStartNtoa(rid, host, &addr, 0, 0, 0)   // timeout/retry 0 =
 *     sceNetResolverDestroy(rid)                           // the console's own
 *     sceNetPoolDestroy(memid)                             // defaults
 *
 * Two additions over that: a payload runs inside a process that already has
 * libSceNet up, an app module may not, so if the pool will not be created the
 * library is initialised once (sceNetInit) and the pool tried again; and every
 * real lookup is logged, so a console log shows what resolved, to what, and how
 * long it took.
 *
 * ALL the addresses, not the first. StartNtoa returns one record, but a host
 * usually has several, and one of them can be unreachable from a given network:
 * from this project's own network 185.199.109.133 (one of raw.githubusercontent
 * .com's four) times out while the other three connect in milliseconds, and the
 * router's DNS lists it first. A resolver that hands over only that one gives
 * FFmpeg nothing to fall back to, so every open of that host burned 3 x 5 s.
 * sceNetResolverStartNtoaMultipleRecordsEx returns up to ten, and getaddrinfo
 * chains them so FFmpeg's connect (which races the list, staggered) reaches a
 * live one within a fraction of a second.
 *
 * That call's result layout is not documented anywhere this project has, so it
 * is trusted only as far as it can be checked: the buffer is far larger than the
 * layout, a canary after it must survive, the counts must be in range, every
 * address must be a plausible IPv4 one. Anything that fails, or a call that
 * errors, falls back to the single-address StartNtoa the SDK itself uses - and
 * if StartNtoa then succeeds where the multi-record call did not, the multi-
 * record call is not tried again this run. It logs which path answered.
 * Creating /mnt/usb0/evo_dns_single turns it off without a rebuild.
 *
 * A 0.0.0.0 answer is not an address. DNS filtering (an ad-blocking router, a
 * family-safe resolver, an ISP block list) answers a blocked name with 0.0.0.0,
 * and the resolver hands it over as a success. Hardware, 2026-09-30:
 * play-lh.googleusercontent.com came back as 0.0.0.0, was read as "the
 * multi-record layout is wrong", switched that call off for the rest of the run,
 * and was then returned to the caller as an address to connect to. Such an
 * answer is skipped; a name with nothing else behind it is not found, and says
 * why in the log. Only a layout that does not parse switches the call off.
 *
 * This replaced two hand-rolled UDP DNS clients that hardcoded eight servers,
 * the first a developer's laptop, and bypassed the console's DNS entirely.
 *
 * IPv4 only, as before: an IPv6 family request is refused with EAI_FAMILY.
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
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifndef INADDR_ANY
#define INADDR_ANY ((uint32_t)0x00000000)
#endif
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((uint32_t)0x7f000001)
#endif

/* EVO's durable log (/mnt/usb0/evo.log). Weak, so this file also builds and
 * links without it: the host test supplies its own, and a build that lacks it
 * simply logs nothing. */
extern void evo_boot_log(const char *fmt, ...) __attribute__((weak));
#define DNS_LOG(...)                                       \
    do {                                                   \
        if (evo_boot_log) evo_boot_log("dns: " __VA_ARGS__); \
    } while (0)

/* libSceNet. The SDK ships the link stub (libSceNet.so) but no header. Signatures
 * as SharpProspero's interop documents them and as the SDK's netdb.o calls them.
 * The multi-record call takes the same shape as StartNtoa with an info block in
 * place of the single address. */
extern int sceNetInit(void);
extern int sceNetPoolCreate(const char *name, int size, int flags);
extern int sceNetPoolDestroy(int memid);
extern int sceNetResolverCreate(const char *name, int memid, int flags);
extern int sceNetResolverStartNtoa(int rid, const char *hostname,
                                   struct in_addr *addr, int timeout,
                                   int retry, int flags);
extern int sceNetResolverStartNtoaMultipleRecordsEx(int rid, const char *hostname,
                                                    void *info, int timeout,
                                                    int retry, int flags);
extern int sceNetResolverDestroy(int rid);

#ifndef EVO_DNS_SINGLE_FLAG
#define EVO_DNS_SINGLE_FLAG "/mnt/usb0/evo_dns_single"
#endif

#define DNS_POOL_NAME  "evo_dns"
#define DNS_POOL_BYTES 0x4000          /* what the SDK's own resolver uses */
#define DNS_HOST_MAX   255             /* longest legal name */
#define DNS_ADDR_MAX   10              /* most records one lookup returns */

/* SceNetResolverInfoEx, as SharpProspero lays it out: ten 32-byte address
 * records (16-byte IPv4/IPv6 union, int family, 12 reserved), then the record
 * counts. */
#define DNS_REC_BYTES       32
#define DNS_REC_FAMILY_AT   16
#define DNS_INFO_RECORDS_AT (DNS_ADDR_MAX * DNS_REC_BYTES)   /* 320 */
#define DNS_INFO_DNS4_AT    (DNS_INFO_RECORDS_AT + 4)
#define DNS_INFO_DOC_BYTES  384        /* the documented struct */
#define DNS_INFO_BUF_BYTES  1024       /* what the call is given: room to spare */
#define DNS_CANARY          0xA5

#define DNS_CACHE_SIZE 64
#define DNS_CACHE_TTL_SEC 300 /* 5 minutes */

typedef struct {
    char host[128];
    struct in_addr addrs[DNS_ADDR_MAX];
    int count;
    time_t expires;
    int valid;
} dns_cache_entry_t;

static dns_cache_entry_t s_dns_cache[DNS_CACHE_SIZE];
static pthread_mutex_t   s_dns_mutex = PTHREAD_MUTEX_INITIALIZER;

static int parse_ipv4_literal(const char *host, struct in_addr *out)
{
    if (!host) return 0;
    unsigned int b0, b1, b2, b3;
    char tail = 0;
    if (sscanf(host, "%u.%u.%u.%u%c", &b0, &b1, &b2, &b3, &tail) == 4) {
        if (b0 <= 255 && b1 <= 255 && b2 <= 255 && b3 <= 255) {
            uint32_t ip = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | (uint32_t)b3;
            out->s_addr = htonl(ip);
            return 1;
        }
    }
    return 0;
}

/* Copies a cached answer into out[max] and returns how many, or 0. */
static int dns_cache_lookup(const char *host, struct in_addr *out, int max)
{
    time_t now = time(NULL);
    int n = 0;
    pthread_mutex_lock(&s_dns_mutex);
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (s_dns_cache[i].valid && strcasecmp(s_dns_cache[i].host, host) == 0) {
            if (s_dns_cache[i].expires >= now) {
                n = s_dns_cache[i].count < max ? s_dns_cache[i].count : max;
                memcpy(out, s_dns_cache[i].addrs, (size_t)n * sizeof(out[0]));
                break;
            } else {
                s_dns_cache[i].valid = 0;
            }
        }
    }
    pthread_mutex_unlock(&s_dns_mutex);
    return n;
}

static void dns_cache_insert(const char *host, const struct in_addr *addrs, int count)
{
    if (!host || strlen(host) >= sizeof(s_dns_cache[0].host)) return;
    if (count < 1) return;
    if (count > DNS_ADDR_MAX) count = DNS_ADDR_MAX;
    time_t now = time(NULL);
    pthread_mutex_lock(&s_dns_mutex);
    static int s_next_slot = 0;
    int slot = -1;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (s_dns_cache[i].valid && strcasecmp(s_dns_cache[i].host, host) == 0) {
            slot = i;
            break;
        }
        if (!s_dns_cache[i].valid || s_dns_cache[i].expires < now) {
            if (slot == -1) slot = i;
        }
    }
    if (slot == -1) {
        slot = s_next_slot;
        s_next_slot = (s_next_slot + 1) % DNS_CACHE_SIZE;
    }
    strncpy(s_dns_cache[slot].host, host, sizeof(s_dns_cache[slot].host) - 1);
    s_dns_cache[slot].host[sizeof(s_dns_cache[slot].host) - 1] = '\0';
    memcpy(s_dns_cache[slot].addrs, addrs, (size_t)count * sizeof(addrs[0]));
    s_dns_cache[slot].count = count;
    s_dns_cache[slot].expires = now + DNS_CACHE_TTL_SEC;
    s_dns_cache[slot].valid = 1;
    pthread_mutex_unlock(&s_dns_mutex);
}

/* ---------------------------------------------------------- the console DNS -- */

typedef enum {
    DNS_FOUND = 0,
    DNS_NOT_FOUND,     /* the resolver ran and had no answer: EAI_NONAME        */
    DNS_UNAVAILABLE    /* no resolver to ask (pool / resolver would not create) */
} dns_result_t;

/* libSceNet may not be initialised in an app module the way it is in a
 * payload's process. One sceNetInit, on the first failure to make a pool; the
 * lock makes a second thread that fails at the same moment wait for it instead
 * of failing on a resolver that is about to work. */
static pthread_mutex_t s_net_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int             s_net_init_done;

static void net_init_once(void)
{
    pthread_mutex_lock(&s_net_init_lock);
    if (!s_net_init_done) {
        int rc = sceNetInit();
        s_net_init_done = 1;
        DNS_LOG("sceNetInit() = 0x%x (the first resolver pool would not create)",
                (unsigned)rc);
    }
    pthread_mutex_unlock(&s_net_init_lock);
}

static long dns_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/* Set once the multi-record call has been seen to fail where StartNtoa worked,
 * or to return something that is not the layout above. Not tried again. */
static int s_multi_broken;

static int multi_enabled(void)
{
    if (s_multi_broken) return 0;
    /* One access() per real lookup, next to a DNS round trip. */
    return access(EVO_DNS_SINGLE_FLAG, F_OK) != 0;
}

/* A resolver's "no address here": DNS filtering answers a blocked name with
 * 0.0.0.0, and 255.255.255.255 is no host either. */
static int addr_usable(uint32_t s_addr)
{
    return s_addr != 0 && s_addr != 0xffffffffu;
}

/* Reads the address records out of the multi-record call's buffer. Returns how
 * many distinct usable IPv4 addresses it holds - 0 is a valid answer (an
 * IPv6-only name, or one the network's DNS blocks) - or -1 if the buffer is not
 * the layout this expects, in which case the caller uses none of it. */
static int parse_multi_records(const unsigned char *buf, struct in_addr *out, int max)
{
    /* The canary past the documented struct: if the call wrote there, the
     * layout is bigger than documented and nothing else can be trusted. */
    for (int i = DNS_INFO_DOC_BYTES; i < DNS_INFO_BUF_BYTES; i++)
        if (buf[i] != DNS_CANARY) return -1;

    int32_t records, dns4;
    memcpy(&records, buf + DNS_INFO_RECORDS_AT, sizeof(records));
    memcpy(&dns4, buf + DNS_INFO_DNS4_AT, sizeof(dns4));
    if (records < 1 || records > DNS_ADDR_MAX) return -1;
    if (dns4 < 0 || dns4 > DNS_ADDR_MAX) return -1;

    int n = 0;
    for (int i = 0; i < records; i++) {
        const unsigned char *rec = buf + (size_t)i * DNS_REC_BYTES;
        int32_t family;
        memcpy(&family, rec + DNS_REC_FAMILY_AT, sizeof(family));
        if (family != AF_INET) continue;              /* an IPv6 record: not ours */

        struct in_addr a;
        memcpy(&a, rec, sizeof(a));
        if (!addr_usable(a.s_addr))                    /* blocked / not a host */
            continue;

        int dup = 0;
        for (int k = 0; k < n; k++)
            if (out[k].s_addr == a.s_addr) dup = 1;
        if (!dup && n < max) out[n++] = a;
    }
    return n;
}

/* One lookup on the console's own DNS: every address, in the resolver's order.
 * `*rc` is the failing call's raw result. */
static dns_result_t dns_resolve(const char *host, struct in_addr *out, int max,
                                int *count, int *rc, const char **why)
{
    *count = 0;
    *why = "no such name";

    int memid = sceNetPoolCreate(DNS_POOL_NAME, DNS_POOL_BYTES, 0);
    if (memid < 0) {
        net_init_once();
        memid = sceNetPoolCreate(DNS_POOL_NAME, DNS_POOL_BYTES, 0);
    }
    if (memid < 0) {
        *rc = memid;
        return DNS_UNAVAILABLE;
    }

    dns_result_t result = DNS_UNAVAILABLE;
    int rid = sceNetResolverCreate(DNS_POOL_NAME, memid, 0);
    *rc = rid;
    if (rid >= 0) {
        int multi_rc = 0, tried_multi = 0;

        if (multi_enabled()) {
            unsigned char *buf = (unsigned char *)malloc(DNS_INFO_BUF_BYTES);
            if (buf) {
                memset(buf, 0, DNS_INFO_DOC_BYTES);
                memset(buf + DNS_INFO_DOC_BYTES, DNS_CANARY,
                       DNS_INFO_BUF_BYTES - DNS_INFO_DOC_BYTES);
                tried_multi = 1;
                multi_rc = sceNetResolverStartNtoaMultipleRecordsEx(rid, host, buf, 0, 0, 0);
                if (multi_rc >= 0) {
                    const int n = parse_multi_records(buf, out, max);
                    if (n > 0) {
                        *count = n;
                        *rc = multi_rc;
                        result = DNS_FOUND;
                    } else if (n == 0) {
                        /* The layout is right and there is nothing to connect
                         * to: blocked by the network's DNS, or IPv6 only. The
                         * resolver has answered; asking again would not change
                         * it. */
                        *rc = multi_rc;
                        *why = "no usable IPv4 address (blocked by the network's DNS, or IPv6 only)";
                        result = DNS_NOT_FOUND;
                        free(buf);
                        goto done;
                    } else {
                        s_multi_broken = 1;
                        DNS_LOG("multi-record lookup returned an unusable layout for %s "
                                "(rc 0x%x) - single address from here on", host,
                                (unsigned)multi_rc);
                    }
                }
                free(buf);
            }
        }

        if (result != DNS_FOUND) {
            struct in_addr addr;
            memset(&addr, 0, sizeof(addr));
            /* timeout 0 / retry 0: the console's own defaults, as the SDK's libc. */
            *rc = sceNetResolverStartNtoa(rid, host, &addr, 0, 0, 0);
            if (*rc >= 0 && addr_usable(addr.s_addr)) {
                out[0] = addr;
                *count = 1;
                result = DNS_FOUND;
                if (tried_multi && multi_rc < 0 && !s_multi_broken) {
                    /* It has an answer the multi-record call could not give. */
                    s_multi_broken = 1;
                    DNS_LOG("multi-record lookup failed (rc 0x%x) where the single "
                            "lookup for %s worked - single address from here on",
                            (unsigned)multi_rc, host);
                }
            } else {
                if (*rc >= 0)
                    *why = "the network's DNS answered 0.0.0.0 (a blocked name)";
                result = DNS_NOT_FOUND;
            }
        }
done:
        sceNetResolverDestroy(rid);
    }
    sceNetPoolDestroy(memid);
    return result;
}

static dns_result_t resolve_hostname(const char *hostname, struct in_addr *out,
                                     int max, int *count)
{
    *count = 0;
    if (!hostname || !*hostname || strlen(hostname) > DNS_HOST_MAX)
        return DNS_NOT_FOUND;

    if (parse_ipv4_literal(hostname, &out[0])) {
        *count = 1;
        return DNS_FOUND;
    }

    if (strcmp(hostname, "localhost") == 0) {
        out[0].s_addr = htonl(INADDR_LOOPBACK);
        *count = 1;
        return DNS_FOUND;
    }

    if ((*count = dns_cache_lookup(hostname, out, max)) > 0)
        return DNS_FOUND;

    int rc = 0;
    const char *why = "no such name";
    const long t0 = dns_now_ms();
    dns_result_t result = dns_resolve(hostname, out, max, count, &rc, &why);
    const long ms = dns_now_ms() - t0;

    switch (result) {
    case DNS_FOUND: {
        char list[DNS_ADDR_MAX * 17 + 1];
        size_t used = 0;
        list[0] = '\0';
        for (int i = 0; i < *count; i++) {
            const unsigned char *b = (const unsigned char *)&out[i].s_addr;
            int w = snprintf(list + used, sizeof(list) - used, "%s%u.%u.%u.%u",
                             i ? ", " : "", b[0], b[1], b[2], b[3]);
            if (w < 0 || (size_t)w >= sizeof(list) - used) break;
            used += (size_t)w;
        }
        dns_cache_insert(hostname, out, *count);
        DNS_LOG("%s -> %s  (console DNS, %d %s, %ld ms)", hostname, list, *count,
                *count == 1 ? "address" : "addresses", ms);
        break;
    }
    case DNS_NOT_FOUND:
        DNS_LOG("%s: no answer - %s  (rc 0x%x, %ld ms)", hostname, why, (unsigned)rc, ms);
        break;
    default:
        DNS_LOG("%s: console resolver unavailable  (rc 0x%x, %ld ms)", hostname,
                (unsigned)rc, ms);
        break;
    }
    return result;
}

static int parse_service(const char *service, int socktype, int flags, int *out_port)
{
    (void)socktype;
    if (!service || !*service) {
        *out_port = 0;
        return 0;
    }

    char *end = NULL;
    unsigned long val = strtoul(service, &end, 10);
    if (*end == '\0') {
        if (val > 65535) return EAI_SERVICE;
        *out_port = (int)val;
        return 0;
    }

    if (flags & AI_NUMERICSERV)
        return EAI_SERVICE;

    if (strcasecmp(service, "http") == 0)        *out_port = 80;
    else if (strcasecmp(service, "https") == 0)  *out_port = 443;
    else if (strcasecmp(service, "rtsp") == 0)   *out_port = 554;
    else if (strcasecmp(service, "domain") == 0) *out_port = 53;
    else if (strcasecmp(service, "ftp") == 0)    *out_port = 21;
    else if (strcasecmp(service, "ssh") == 0)    *out_port = 22;
    else if (strcasecmp(service, "ntp") == 0)    *out_port = 123;
    else return EAI_SERVICE;

    return 0;
}

void freeaddrinfo(struct addrinfo *res);

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints,
                struct addrinfo **res)
{
    if (!res) return EAI_FAIL;
    *res = NULL;

    if (!node && !service) return EAI_NONAME;

    int flags    = hints ? hints->ai_flags : 0;
    int family   = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;

    if (family != AF_UNSPEC && family != AF_INET) {
        return EAI_FAMILY;
    }

    int port = 0;
    int err = parse_service(service, socktype, flags, &port);
    if (err != 0) return err;

    struct in_addr addrs[DNS_ADDR_MAX];
    int naddr = 0;
    memset(addrs, 0, sizeof(addrs));

    if (!node) {
        addrs[0].s_addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
        naddr = 1;
    } else {
        char clean_node[256];
        const char *host_str = node;
        size_t nlen = strlen(node);
        if (nlen == 0) {
            return EAI_NONAME;
        }
        /* "[host]": brackets stripped. */
        if (nlen > 2 && nlen < sizeof(clean_node) &&
            node[0] == '[' && node[nlen - 1] == ']') {
            memcpy(clean_node, node + 1, nlen - 2);
            clean_node[nlen - 2] = '\0';
            host_str = clean_node;
        }

        if (parse_ipv4_literal(host_str, &addrs[0])) {
            naddr = 1;
        } else {
            if (flags & AI_NUMERICHOST) {
                return EAI_NONAME;
            }
            switch (resolve_hostname(host_str, addrs, DNS_ADDR_MAX, &naddr)) {
            case DNS_FOUND:       break;
            case DNS_UNAVAILABLE: return EAI_AGAIN;
            default:              return EAI_NONAME;
            }
        }
    }

    /* One addrinfo per address, in the resolver's order: the caller (FFmpeg's
     * connect) walks - and races - the chain, so a dead first address is not
     * the end of the host. */
    struct addrinfo *head = NULL, *tail = NULL;
    for (int i = 0; i < naddr; i++) {
        struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
        struct sockaddr_in *sin = (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
        if (!ai || !sin) {
            if (ai) free(ai);
            if (sin) free(sin);
            freeaddrinfo(head);
            return EAI_MEMORY;
        }

        sin->sin_family = AF_INET;
        sin->sin_port = htons((uint16_t)port);
        sin->sin_addr = addrs[i];

        ai->ai_family   = AF_INET;
        ai->ai_socktype = socktype ? socktype : SOCK_STREAM;
        ai->ai_protocol = protocol ? protocol : (ai->ai_socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
        ai->ai_addrlen  = sizeof(struct sockaddr_in);
        ai->ai_addr     = (struct sockaddr *)sin;

        if (i == 0 && (flags & AI_CANONNAME) && node) {
            ai->ai_canonname = strdup(node);
        }

        if (tail) tail->ai_next = ai; else head = ai;
        tail = ai;
    }

    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        if (res->ai_canonname) {
            free(res->ai_canonname);
        }
        if (res->ai_addr) {
            free(res->ai_addr);
        }
        free(res);
        res = next;
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen,
                char *host, size_t hostlen,
                char *serv, size_t servlen, int flags)
{
    (void)flags;
    if (!sa || salen < sizeof(struct sockaddr_in))
        return EAI_FAMILY;
    if (sa->sa_family != AF_INET)
        return EAI_FAMILY;

    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    if (host && hostlen > 0) {
        const unsigned char *b = (const unsigned char *)&sin->sin_addr.s_addr;
        int n = snprintf(host, hostlen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        if (n < 0 || (size_t)n >= hostlen)
            return EAI_OVERFLOW;
    }

    if (serv && servlen > 0) {
        int n = snprintf(serv, servlen, "%u", (unsigned int)ntohs(sin->sin_port));
        if (n < 0 || (size_t)n >= servlen)
            return EAI_OVERFLOW;
    }

    return 0;
}

const char *gai_strerror(int errcode)
{
    switch (errcode) {
        case 0:            return "Success";
        case EAI_AGAIN:    return "Temporary failure in name resolution";
        case EAI_BADFLAGS: return "Invalid value for ai_flags";
        case EAI_FAIL:     return "Non-recoverable failure in name resolution";
        case EAI_FAMILY:   return "ai_family not supported";
        case EAI_MEMORY:   return "Memory allocation failure";
        case EAI_NONAME:   return "Name or service not known";
        case EAI_SERVICE:  return "Servname not supported for ai_socktype";
        case EAI_SOCKTYPE: return "ai_socktype not supported";
        case EAI_SYSTEM:   return "System error";
        case EAI_OVERFLOW: return "Argument buffer overflow";
        default:           return "Unknown error";
    }
}
