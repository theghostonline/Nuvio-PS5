#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nuvio.h"

#define MAX_POST_BYTES (8 * 1024 * 1024)
#define LOG_TAIL_BYTES (96 * 1024)

volatile int nuvio_keep_running = 1;

static struct MHD_Daemon *g_daemon;
static char g_www_dir[256];

typedef struct {
  char *data;
  size_t len;
  int too_large;
} post_body_t;

static const struct {
  const char *ext;
  const char *type;
} MIME_TYPES[] = {
    {".html", "text/html; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".mjs", "text/javascript; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".webp", "image/webp"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".otf", "font/otf"},
    {".wasm", "application/wasm"},
    {".txt", "text/plain; charset=utf-8"},
    {".md", "text/plain; charset=utf-8"},
    {".xml", "application/xml"},
    {".vtt", "text/vtt; charset=utf-8"},
    {".mp4", "video/mp4"},
    {".m3u8", "application/vnd.apple.mpegurl"},
};

static const char *mime_for(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot)
    return "application/octet-stream";
  for (size_t i = 0; i < sizeof(MIME_TYPES) / sizeof(MIME_TYPES[0]); i++)
    if (!strcasecmp(dot, MIME_TYPES[i].ext))
      return MIME_TYPES[i].type;
  return "application/octet-stream";
}

static int is_loopback(struct MHD_Connection *conn) {
  const union MHD_ConnectionInfo *info =
      MHD_get_connection_info(conn, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
  const struct sockaddr_in *addr;

  if (!info || !info->client_addr)
    return 0;
  addr = (const struct sockaddr_in *)info->client_addr;
  return addr->sin_family == AF_INET && (ntohl(addr->sin_addr.s_addr) >> 24) == 127;
}

static enum MHD_Result send_buffer(struct MHD_Connection *conn, int status,
                                   const char *type, const void *data, size_t len,
                                   enum MHD_ResponseMemoryMode mode) {
  struct MHD_Response *resp =
      MHD_create_response_from_buffer(len, (void *)data, mode);
  enum MHD_Result ret;

  MHD_add_response_header(resp, "Content-Type", type);
  MHD_add_response_header(resp, "Cache-Control", "no-store");
  MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
  ret = MHD_queue_response(conn, (unsigned)status, resp);
  MHD_destroy_response(resp);
  return ret;
}

static enum MHD_Result send_json(struct MHD_Connection *conn, int status, char *json) {
  if (!json) {
    static const char fallback[] = "{\"returnValue\":false,\"errorText\":\"Out of memory\"}";
    return send_buffer(conn, 500, "application/json", fallback, sizeof(fallback) - 1,
                       MHD_RESPMEM_PERSISTENT);
  }
  return send_buffer(conn, status, "application/json; charset=utf-8", json, strlen(json),
                     MHD_RESPMEM_MUST_FREE);
}

static enum MHD_Result send_text(struct MHD_Connection *conn, int status, const char *text) {
  return send_buffer(conn, status, "text/plain; charset=utf-8", text, strlen(text),
                     MHD_RESPMEM_MUST_COPY);
}

/* MHD_create_response_from_fd uses sendfile(), which stalls after ~32 KiB on
 * the PS5, so file bodies are streamed with pread() through a callback. */
static ssize_t read_file_block(void *cls, uint64_t pos, char *buf, size_t max) {
  ssize_t n = pread((int)(intptr_t)cls, buf, max, (off_t)pos);
  if (n == 0)
    return MHD_CONTENT_READER_END_OF_STREAM;
  return n < 0 ? MHD_CONTENT_READER_END_WITH_ERROR : n;
}

static void close_file(void *cls) {
  close((int)(intptr_t)cls);
}

static void http_date(time_t when, char *out, size_t size) {
  struct tm tm;
  gmtime_r(&when, &tm);
  strftime(out, size, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static enum MHD_Result serve_static(struct MHD_Connection *conn, const char *url) {
  char path[1024];
  char last_modified[64];
  const char *since;
  struct stat st;
  struct MHD_Response *resp;
  enum MHD_Result ret;
  int fd;

  if (strstr(url, "..") || strchr(url, '\\'))
    return send_text(conn, 400, "Bad path\n");
  if (!strcmp(url, "/") || !url[0])
    url = "/index.html";

  snprintf(path, sizeof(path), "%s%s", g_www_dir, url);
  path[strcspn(path, "?#")] = '\0';
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    /* Unknown routes fall back to the app shell. */
    if (!strchr(strrchr(url, '/') ? strrchr(url, '/') : url, '.')) {
      snprintf(path, sizeof(path), "%s/index.html", g_www_dir);
      if (stat(path, &st) != 0)
        return send_text(conn, 404, "Not found\n");
    } else {
      return send_text(conn, 404, "Not found\n");
    }
  }

  http_date(st.st_mtime, last_modified, sizeof(last_modified));
  since = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "If-Modified-Since");
  if (since && !strcmp(since, last_modified)) {
    resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(resp, "Last-Modified", last_modified);
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    ret = MHD_queue_response(conn, MHD_HTTP_NOT_MODIFIED, resp);
    MHD_destroy_response(resp);
    return ret;
  }

  if ((fd = open(path, O_RDONLY)) < 0)
    return send_text(conn, 404, "Not found\n");
  resp = MHD_create_response_from_callback((uint64_t)st.st_size, 64 * 1024, read_file_block,
                                           (void *)(intptr_t)fd, close_file);
  if (!resp) {
    close(fd);
    return send_text(conn, 500, "Response failed\n");
  }
  MHD_add_response_header(resp, "Content-Type", mime_for(path));
  MHD_add_response_header(resp, "Last-Modified", last_modified);
  /* Revalidate every load: builds are swapped in place by the payload. */
  MHD_add_response_header(resp, "Cache-Control", "no-cache");
  ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return ret;
}

static char *read_tail(const char *path, size_t max_bytes) {
  struct stat st;
  size_t start, len;
  char *buf;
  FILE *f;

  if (stat(path, &st) != 0 || !(f = fopen(path, "rb")))
    return strdup("");
  start = (size_t)st.st_size > max_bytes ? (size_t)st.st_size - max_bytes : 0;
  len = (size_t)st.st_size - start;
  if (!(buf = malloc(len + 1))) {
    fclose(f);
    return strdup("");
  }
  fseek(f, (long)start, SEEK_SET);
  len = fread(buf, 1, len, f);
  buf[len] = '\0';
  fclose(f);
  return buf;
}

static enum MHD_Result serve_logs(struct MHD_Connection *conn) {
  char *service = read_tail(NUVIO_LOG_PATH, LOG_TAIL_BYTES);
  char *app = read_tail(NUVIO_APP_LOG_PATH, LOG_TAIL_BYTES);
  size_t len = strlen(service) + strlen(app) + 128;
  char *out = malloc(len);

  if (out)
    snprintf(out, len, "=== %s ===\n%s\n=== %s ===\n%s", NUVIO_LOG_PATH, service,
             NUVIO_APP_LOG_PATH, app);
  free(service);
  free(app);
  if (!out)
    return send_text(conn, 500, "Out of memory\n");
  return send_buffer(conn, 200, "text/plain; charset=utf-8", out, strlen(out),
                     MHD_RESPMEM_MUST_FREE);
}

static enum MHD_Result serve_status(struct MHD_Connection *conn) {
  char json[512];
  snprintf(json, sizeof(json),
           "{\"returnValue\":true,\"service\":\"nuvio-ps5\",\"version\":\"%s\","
           "\"titleId\":\"%s\",\"port\":%d,\"www\":\"%s\"}",
           NUVIO_PS5_VERSION, NUVIO_TITLE_ID, NUVIO_PORT, g_www_dir);
  return send_json(conn, 200, strdup(json));
}

static enum MHD_Result open_stream(struct MHD_Connection *conn, post_body_t *body) {
  json_error_t error;
  json_t *parsed = body && body->len ? json_loadb(body->data, body->len, 0, &error) : NULL;
  const char *stream_url = json_string_value(json_object_get(parsed, "url"));
  json_t *headers = json_object_get(parsed, "headers");
  const char *names[32], *values[32];
  int count = 0, status = 200;
  char *reply;

  if (json_is_object(headers)) {
    const char *key;
    json_t *value;
    json_object_foreach(headers, key, value) {
      if (count < 32 && json_is_string(value)) {
        names[count] = key;
        values[count] = json_string_value(value);
        count++;
      }
    }
  }
  reply = nuvio_stream_open(stream_url, names, values, count, &status);
  json_decref(parsed);
  return send_json(conn, status, reply);
}

/* /stream/<id>/index.m3u8 | init.mp4 | <n>.m4s */
static enum MHD_Result serve_stream(struct MHD_Connection *conn, const char *path) {
  char id[32];
  const char *slash = strchr(path, '/');
  const char *file;
  size_t len = 0;
  void *data = NULL;
  const char *type = "application/octet-stream";

  if (!slash || slash - path >= (long)sizeof(id))
    return send_text(conn, 404, "Not found\n");
  memcpy(id, path, (size_t)(slash - path));
  id[slash - path] = '\0';
  file = slash + 1;

  if (!strcmp(file, "index.m3u8")) {
    data = nuvio_stream_playlist(id, &len);
    type = "application/vnd.apple.mpegurl";
  } else if (!strcmp(file, "init.mp4")) {
    data = nuvio_stream_init(id, &len);
    type = "video/mp4";
  } else {
    char *end = NULL;
    long index = strtol(file, &end, 10);
    if (end && !strcmp(end, ".m4s")) {
      data = nuvio_stream_segment(id, (int)index, &len);
      type = "video/iso.segment";
    }
  }
  if (!data)
    return send_text(conn, 404, "Stream segment unavailable\n");
  return send_buffer(conn, 200, type, data, len, MHD_RESPMEM_MUST_FREE);
}

/* The player mailbox and the debug routes, from a test machine on the LAN:
 * only while /data/nuvio/lan-debug exists. Storage (sign-in tokens) and the
 * rest of /api stay loopback-only regardless. */
static int lan_debug_route(const char *url) {
  struct stat st;
  return (!strncmp(url, "/api/player/", 12) || !strncmp(url, "/api/debug/", 11)) &&
         stat(NUVIO_LAN_DEBUG_FLAG, &st) == 0;
}

static enum MHD_Result handle_request(struct MHD_Connection *conn, const char *url,
                                      const char *method, post_body_t *body) {
  int loopback = is_loopback(conn);

  if (!strcmp(method, "OPTIONS")) {
    struct MHD_Response *resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result ret;
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(resp, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    MHD_add_response_header(resp, "Access-Control-Allow-Headers", "Content-Type");
    ret = MHD_queue_response(conn, 204, resp);
    MHD_destroy_response(resp);
    return ret;
  }

  if (body && body->too_large)
    return send_text(conn, 413, "Request too large\n");

  if (!strncmp(url, "/plugin/", 8)) {
    int status = 200;
    char *reply;
    if (!loopback)
      return send_json(conn, 403, strdup("{\"returnValue\":false,\"errorText\":\"Plugin service "
                                         "accepts loopback clients only\"}"));
    reply = nuvio_plugin_handle(url + 8, method, body ? body->data : NULL,
                                body ? body->len : 0, &status);
    return send_json(conn, status, reply);
  }

  /* Tile deeplink: start the full-screen shell, falling back to windowed. */
  if (!strcmp(url, "/launch")) {
    static const char page[] =
        "<!doctype html><html><head><meta charset=utf-8><title>Nuvio</title>"
        "<style>html,body{margin:0;height:100%;background:#0d0e12;overflow:hidden}</style>"
        "</head><body><script>(function(){var n=0;function go(){location.replace('/');}"
        "function poll(){var x=new XMLHttpRequest();x.open('GET','/api/launch/status',true);"
        "x.onload=function(){var s='';try{s=JSON.parse(x.responseText).state;}catch(e){}"
        "if(s==='running'){try{window.close();}catch(e){}return;}"
        "if(s==='failed'||++n>80){go();return;}setTimeout(poll,250);};"
        "x.onerror=function(){if(++n>80){go();return;}setTimeout(poll,250);};x.send();}"
        "poll();})();</script></body></html>";
    if (!loopback)
      return send_text(conn, 403, "Loopback only\n");
    if (!NUVIO_FULLSCREEN_SHELL) {
      /* hbldr cannot start an unregistered host on FW 13.60 (LaunchApp 0x18);
       * open the app in the browser window until the shell has a safe path. */
      struct MHD_Response *resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
      enum MHD_Result ret;
      MHD_add_response_header(resp, "Location", "/");
      MHD_add_response_header(resp, "Cache-Control", "no-store");
      ret = MHD_queue_response(conn, 302, resp);
      MHD_destroy_response(resp);
      return ret;
    }
    nuvio_launch_shell_async();
    return send_buffer(conn, 200, "text/html; charset=utf-8", page, sizeof(page) - 1,
                       MHD_RESPMEM_PERSISTENT);
  }

  if (!strcmp(url, "/api/launch/status")) {
    char json[96];
    snprintf(json, sizeof(json), "{\"returnValue\":true,\"state\":\"%s\"}", nuvio_launch_state());
    return send_json(conn, 200, strdup(json));
  }

  if (!strncmp(url, "/stream/", 8))
    return serve_stream(conn, url + 8);

  if (!strcmp(url, "/api/stream/open") && !strcmp(method, "POST")) {
    struct stat st;
    if (!loopback && stat(NUVIO_LAN_DEBUG_FLAG, &st) != 0)
      return send_text(conn, 403, "Loopback only\n");
    return open_stream(conn, body);
  }

  if (!strcmp(url, "/api/debug/speed")) {
    struct stat st;
    const char *target = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "url");
    const char *buffer = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "buffer");
    if (stat(NUVIO_LAN_DEBUG_FLAG, &st) != 0 || !target)
      return send_text(conn, 403, "Debug only\n");
    return send_json(conn, 200, nuvio_speed_test(target, buffer ? atol(buffer) : 0));
  }

  if (!strcmp(url, "/api/stream/close") && !strcmp(method, "POST")) {
    json_error_t error;
    json_t *parsed = body && body->len ? json_loadb(body->data, body->len, 0, &error) : NULL;
    const char *id = json_string_value(json_object_get(parsed, "id"));
    if (id)
      nuvio_stream_close(id);
    json_decref(parsed);
    return send_json(conn, 200, strdup("{\"returnValue\":true}"));
  }

  if (!strcmp(url, "/api/status"))
    return serve_status(conn);

  if (!strcmp(url, "/api/logs"))
    return serve_logs(conn);

  if (!strcmp(url, "/api/log") && !strcmp(method, "POST")) {
    if (body && body->len)
      nuvio_app_log_append(body->data, body->len);
    return send_buffer(conn, 204, "text/plain", "", 0, MHD_RESPMEM_PERSISTENT);
  }

  if (!strncmp(url, "/api/", 5) && !loopback && !lan_debug_route(url))
    return send_text(conn, 403, "Loopback only\n");

  if (!strcmp(url, "/api/debug/shot")) {
    if (!strcmp(method, "POST")) {
      nuvio_debug_shot_store(body ? body->data : NULL, body ? body->len : 0);
      return send_buffer(conn, 204, "text/plain", "", 0, MHD_RESPMEM_PERSISTENT);
    }
    {
      size_t len = 0;
      void *shot = nuvio_debug_shot_copy(&len);
      return shot ? send_buffer(conn, 200, "image/bmp", shot, len, MHD_RESPMEM_MUST_FREE)
                  : send_text(conn, 404, "No screenshot\n");
    }
  }

  /* Decoder tuning for hardware tests: the app cannot open /data itself. */
  if (!strcmp(url, "/api/debug/vdec-conf")) {
    FILE *f = fopen(NUVIO_DATA_DIR "/vdec.conf", "rb");
    char buf[512];
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f)
      fclose(f);
    buf[n] = 0;
    return send_text(conn, 200, buf);
  }

  if (!strcmp(url, "/api/debug/last-request")) {
    size_t len = 0;
    char *req = nuvio_debug_last_request(&len);
    return req ? send_buffer(conn, 200, "application/json", req, len, MHD_RESPMEM_MUST_FREE)
               : send_text(conn, 404, "No request\n");
  }

  if (!strcmp(url, "/api/debug/launch") && !strcmp(method, "POST")) {
    int status = 200;
    return send_json(conn, status, nuvio_debug_launch_app(&status));
  }

  /* Mirror of Nuvio's localStorage; see ps5-boot.js. Holds sign-in tokens,
   * so it is served to loopback clients only (checked above). */
  if (!strcmp(url, "/api/storage")) {
    if (!strcmp(method, "POST")) {
      json_error_t error;
      json_t *parsed = body && body->len ? json_loadb(body->data, body->len, 0, &error) : NULL;
      int ok = parsed && json_is_object(parsed) &&
               nuvio_write_file(NUVIO_STORAGE_PATH, body->data, body->len, 0600) == 0;
      json_decref(parsed);
      return send_json(conn, ok ? 200 : 400,
                       strdup(ok ? "{\"returnValue\":true}" : "{\"returnValue\":false}"));
    }
    {
      char *saved = read_tail(NUVIO_STORAGE_PATH, 16 * 1024 * 1024);
      if (!saved[0]) {
        free(saved);
        saved = strdup("{\"stamp\":0,\"items\":{}}");
      }
      return send_json(conn, 200, saved);
    }
  }

  if (!strncmp(url, "/api/player/", 12)) {
    const char *op = url + 12;
    char *json = NULL;
    int status = 200;
    if (!strcmp(op, "play") && !strcmp(method, "POST")) {
      json = nuvio_player_play(body ? body->data : NULL, body ? body->len : 0, &status);
      return json ? send_json(conn, status, json) : send_text(conn, 500, "Out of memory\n");
    }
    if (!strcmp(op, "state") && !strcmp(method, "POST"))
      return nuvio_player_state(body ? body->data : NULL, body ? body->len : 0) == 0
                 ? send_buffer(conn, 204, "text/plain", "", 0, MHD_RESPMEM_PERSISTENT)
                 : send_text(conn, 400, "Bad state\n");
    if (!strcmp(op, "control")) {
      if (!strcmp(method, "POST"))
        return nuvio_player_control_post(body ? body->data : NULL, body ? body->len : 0) == 0
                   ? send_buffer(conn, 204, "text/plain", "", 0, MHD_RESPMEM_PERSISTENT)
                   : send_text(conn, 400, "Bad command\n");
      json = nuvio_player_control_take();
    } else if (!strcmp(op, "next"))
      json = nuvio_player_next();
    else if (!strcmp(op, "result"))
      json = nuvio_player_result();
    else if (!strcmp(op, "status"))
      json = nuvio_player_status();
    else
      return send_text(conn, 404, "Unknown player call\n");
    return json ? send_json(conn, 200, json)
                : send_buffer(conn, 204, "text/plain", "", 0, MHD_RESPMEM_PERSISTENT);
  }

  if (!strcmp(url, "/api/tile/uninstall")) {
    int rc = nuvio_tile_uninstall();
    char json[96];
    snprintf(json, sizeof(json), "{\"returnValue\":true,\"result\":%d}", rc);
    return send_json(conn, 200, strdup(json));
  }

  if (!strcmp(url, "/api/shutdown")) {
    nuvio_keep_running = 0;
    return send_json(conn, 200, strdup("{\"returnValue\":true}"));
  }

  if (!strcmp(method, "GET") || !strcmp(method, "HEAD"))
    return serve_static(conn, url);

  return send_text(conn, 405, "Method not allowed\n");
}

static enum MHD_Result on_request(void *cls, struct MHD_Connection *conn, const char *url,
                                  const char *method, const char *version,
                                  const char *upload_data, size_t *upload_data_size,
                                  void **con_cls) {
  post_body_t *body = *con_cls;
  (void)cls;
  (void)version;

  if (strcmp(method, "POST") != 0 && strcmp(method, "PUT") != 0)
    return handle_request(conn, url, method, NULL);

  if (!body) {
    body = calloc(1, sizeof(*body));
    if (!body)
      return MHD_NO;
    *con_cls = body;
    return MHD_YES;
  }

  if (*upload_data_size) {
    if (body->len + *upload_data_size > MAX_POST_BYTES) {
      body->too_large = 1;
    } else if (!body->too_large) {
      char *grown = realloc(body->data, body->len + *upload_data_size + 1);
      if (!grown) {
        body->too_large = 1;
      } else {
        body->data = grown;
        memcpy(body->data + body->len, upload_data, *upload_data_size);
        body->len += *upload_data_size;
        body->data[body->len] = '\0';
      }
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  return handle_request(conn, url, method, body);
}

static void on_completed(void *cls, struct MHD_Connection *conn, void **con_cls,
                         enum MHD_RequestTerminationCode toe) {
  post_body_t *body = *con_cls;
  (void)cls;
  (void)conn;
  (void)toe;
  if (body) {
    free(body->data);
    free(body);
    *con_cls = NULL;
  }
}

int nuvio_http_start(const char *www_dir) {
  snprintf(g_www_dir, sizeof(g_www_dir), "%s", www_dir ? www_dir : NUVIO_WWW_DIR);
  g_daemon = MHD_start_daemon(
      MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
      NUVIO_PORT, NULL, NULL, &on_request, NULL, MHD_OPTION_NOTIFY_COMPLETED, &on_completed,
      NULL, MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)120, MHD_OPTION_LISTENING_ADDRESS_REUSE,
      (unsigned int)1, MHD_OPTION_END);
  if (!g_daemon) {
    nuvio_log("http: failed to start on port %d (errno=%d)", NUVIO_PORT, errno);
    return -1;
  }
  nuvio_log("http: serving %s on port %d", g_www_dir, NUVIO_PORT);
  return 0;
}

void nuvio_http_stop(void) {
  if (g_daemon) {
    MHD_stop_daemon(g_daemon);
    g_daemon = NULL;
  }
}
