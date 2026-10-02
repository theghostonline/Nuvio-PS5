#include "nuvio_bridge.h"

#include "cJSON.h"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* A play request carries the sources, episodes and UI strings: allow 2 MB. */
#define RESPONSE_CAP (2 * 1024 * 1024)

static int connect_service(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(NUVIO_SERVICE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* One HTTP/1.0 exchange. Returns the status code (or -1) and the body in
 * *body (malloc'd, NUL-terminated; NULL when empty). */
static int http_send(const char *method, const char *path, const char *type,
                     const void *data, size_t len, char **body)
{
    char head[256];
    int fd, status = -1;
    char *buf;
    size_t got = 0;

    if (body)
        *body = NULL;
    if ((fd = connect_service()) < 0)
        return -1;
    snprintf(head, sizeof head,
             "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: %s\r\n"
             "Content-Length: %zu\r\n\r\n", method, path, type, len);
    if (send_all(fd, head, strlen(head)) != 0 || (len && send_all(fd, data, len) != 0)) {
        close(fd);
        return -1;
    }
    if (!(buf = malloc(RESPONSE_CAP + 1))) {
        close(fd);
        return -1;
    }
    for (;;) {
        ssize_t r = read(fd, buf + got, RESPONSE_CAP - got);
        if (r <= 0)
            break;
        got += (size_t)r;
        if (got == RESPONSE_CAP)
            break;
    }
    close(fd);
    buf[got] = 0;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1)
        status = -1;
    if (body) {
        char *sep = strstr(buf, "\r\n\r\n");
        if (sep && sep[4]) {
            *body = strdup(sep + 4);
        }
    }
    free(buf);
    return status;
}

static int http_call(const char *method, const char *path, const char *json, char **body)
{
    return http_send(method, path, "application/json", json, json ? strlen(json) : 0, body);
}

int nuvio_service_up(void)
{
    int fd = connect_service();
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static void copy_str(char *dst, size_t cap, const cJSON *item)
{
    const char *s = cJSON_GetStringValue(item);
    snprintf(dst, cap, "%s", s ? s : "");
}

int nuvio_bridge_next(char **json)
{
    char *body = NULL;
    int status = http_call("GET", "/api/player/next", NULL, &body);

    *json = NULL;
    if (status == 204) {
        free(body);
        return 0;
    }
    if (status != 200 || !body) {
        free(body);
        return status < 0 ? -1 : 0;
    }
    /* Only a request with a url is a stream; the rest is the page's business. */
    cJSON *root = cJSON_Parse(body);
    const char *url = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "url")) : NULL;
    char id[64] = {0};
    copy_str(id, sizeof id, root ? cJSON_GetObjectItem(root, "id") : NULL);
    const int ok = url && *url;
    if (ok)
        evo_bt("nuvio: play request id=%s (%zu bytes)", id, strlen(body));
    cJSON_Delete(root);
    if (!ok) {
        free(body);
        return 0;
    }
    *json = body;
    return 1;
}

void nuvio_bridge_state_json(const char *json)
{
    if (json)
        http_call("POST", "/api/player/state", json, NULL);
}

void nuvio_bridge_state(const char *id, const char *state, double position,
                        double duration, const char *error)
{
    cJSON *o = cJSON_CreateObject();
    char *json;

    cJSON_AddStringToObject(o, "id", id ? id : "");
    cJSON_AddStringToObject(o, "state", state);
    cJSON_AddNumberToObject(o, "position", position);
    cJSON_AddNumberToObject(o, "duration", duration);
    if (error && error[0])
        cJSON_AddStringToObject(o, "error", error);
    json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json) {
        http_call("POST", "/api/player/state", json, NULL);
        free(json);
    }
}

int nuvio_bridge_get(const char *path, char **body)
{
    return http_call("GET", path, NULL, body);
}

int nuvio_bridge_commands(nuvio_command *out, int max)
{
    char *body = NULL;
    int n = 0;
    cJSON *root, *item;

    if (http_call("GET", "/api/player/control", NULL, &body) != 200 || !body) {
        free(body);
        return 0;
    }
    root = cJSON_Parse(body);
    free(body);
    cJSON_ArrayForEach(item, root) {
        const cJSON *v;
        if (n >= max)
            break;
        memset(&out[n], 0, sizeof out[n]);
        copy_str(out[n].cmd, sizeof out[n].cmd, cJSON_GetObjectItem(item, "cmd"));
        copy_str(out[n].button, sizeof out[n].button, cJSON_GetObjectItem(item, "button"));
        v = cJSON_GetObjectItem(item, "hold_ms");
        out[n].value = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
        if (out[n].cmd[0])
            n++;
    }
    cJSON_Delete(root);
    return n;
}

int nuvio_bridge_post_blob(const char *path, const char *type, const void *data, size_t len)
{
    int status = http_send("POST", path, type, data, len, NULL);
    return (status >= 200 && status < 300) ? 0 : -1;
}
