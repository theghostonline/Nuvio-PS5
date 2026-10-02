/*
 * Native replacement for Nuvio TV's services/plugin-http.cjs.
 *
 * The web app runs plugins (scrapers) in a QuickJS worker; every network call
 * a plugin makes is posted here as JSON and performed with libcurl, so it can
 * set any header (Referer, Cookie, User-Agent...) and ignore browser CORS.
 * Routes, payloads and responses match plugin-http protocol version 1.
 */

#include <curl/curl.h>
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "assets.h"
#include "nuvio.h"

NUVIO_INCASSET(cacert_pem, "assets/cacert.pem");

#define PLUGIN_PROTOCOL_VERSION 1
#define MAX_ACTIVE_REQUESTS 10
#define DEFAULT_RESPONSE_BYTES (1024 * 1024)
#define MAX_RESPONSE_BYTES (5 * 1024 * 1024)
#define MAX_REDIRECTS 20
#define DEFAULT_TIMEOUT_MS 60000
#define CONNECT_TIMEOUT_MS 30000
#define MAX_TRACKED_REQUESTS 64
#define DEFAULT_USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_slot_free = PTHREAD_COND_INITIALIZER;
static int g_active;
static unsigned long g_completed, g_failed;

/* In-flight request ids, so /plugin/cancel can abort a transfer. */
static struct {
  char id[129];
  volatile int cancelled;
  int used;
} g_requests[MAX_TRACKED_REQUESTS];

typedef struct {
  char *data;
  size_t len, cap, limit;
  int truncated;
} buffer_t;

typedef struct {
  json_t *headers;
  char status_text[128];
} header_state_t;

typedef struct {
  int slot;
} progress_state_t;

int nuvio_plugin_init(void) {
  CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
  nuvio_log("plugin: curl %s init -> %d", curl_version(), (int)rc);
  return rc == CURLE_OK ? 0 : -1;
}

static char *json_reply(json_t *obj) {
  char *text = json_dumps(obj, JSON_COMPACT);
  json_decref(obj);
  return text;
}

static char *error_reply(int *status, int code, const char *message, const char *request_id) {
  json_t *obj = json_object();
  *status = code;
  json_object_set_new(obj, "returnValue", json_false());
  json_object_set_new(obj, "errorText", json_string(message));
  if (request_id && request_id[0])
    json_object_set_new(obj, "requestId", json_string(request_id));
  return json_reply(obj);
}

static json_t *capabilities_object(void) {
  json_t *obj = json_object();
  json_object_set_new(obj, "returnValue", json_true());
  json_object_set_new(obj, "service", json_string("nuvio-plugin-network"));
  json_object_set_new(obj, "protocolVersion", json_integer(PLUGIN_PROTOCOL_VERSION));
  json_object_set_new(obj, "serviceVersion", json_integer(1));
  json_object_set_new(obj, "runtimeVersion", json_string("nuvio-ps5/" NUVIO_PS5_VERSION));
  json_object_set_new(obj, "quickjsVersion", json_string("quickjs-emscripten/0.32.0 (app-worker)"));
  json_object_set_new(obj, "workerSupport", json_true());
  json_object_set_new(obj, "maxConcurrency", json_integer(MAX_ACTIVE_REQUESTS));
  json_object_set_new(obj, "memoryTier", json_string("bounded"));
  json_object_set_new(obj, "defaultResponseBytes", json_integer(DEFAULT_RESPONSE_BYTES));
  json_object_set_new(obj, "maxResponseBytes", json_integer(MAX_RESPONSE_BYTES));
  json_object_set_new(obj, "jsPluginCapability", json_true());
  json_object_set_new(obj, "networkBoundary", json_true());
  json_object_set_new(obj, "port", json_integer(NUVIO_PORT));
  return obj;
}

/* ---- request tracking ---------------------------------------------------- */

static int track_request(const char *id) {
  int slot = -1;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_TRACKED_REQUESTS; i++) {
    if (!g_requests[i].used) {
      g_requests[i].used = 1;
      g_requests[i].cancelled = 0;
      snprintf(g_requests[i].id, sizeof(g_requests[i].id), "%s", id ? id : "");
      slot = i;
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return slot;
}

static void untrack_request(int slot) {
  if (slot < 0)
    return;
  pthread_mutex_lock(&g_lock);
  g_requests[slot].used = 0;
  g_requests[slot].id[0] = '\0';
  pthread_mutex_unlock(&g_lock);
}

static int cancel_request(const char *id) {
  int found = 0;
  if (!id || !id[0])
    return 0;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_TRACKED_REQUESTS; i++) {
    if (g_requests[i].used && !strcmp(g_requests[i].id, id)) {
      g_requests[i].cancelled = 1;
      found = 1;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return found;
}

/* Waits for one of the MAX_ACTIVE_REQUESTS slots, like plugin-http's queue. */
static int acquire_slot(int timeout_ms) {
  struct timespec deadline;
  int rc = 0;

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&g_lock);
  while (g_active >= MAX_ACTIVE_REQUESTS && rc == 0)
    rc = pthread_cond_timedwait(&g_slot_free, &g_lock, &deadline);
  if (g_active < MAX_ACTIVE_REQUESTS) {
    g_active++;
    rc = 0;
  } else {
    rc = -1;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

static void release_slot(int failed) {
  pthread_mutex_lock(&g_lock);
  g_active--;
  if (failed)
    g_failed++;
  else
    g_completed++;
  pthread_cond_signal(&g_slot_free);
  pthread_mutex_unlock(&g_lock);
}

/* ---- curl callbacks ------------------------------------------------------ */

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
  buffer_t *buf = userdata;
  size_t n = size * nmemb;
  size_t room;

  if (buf->len >= buf->limit) {
    buf->truncated = 1;
    return 0; /* stops the transfer; reported as a truncated response */
  }
  room = buf->limit - buf->len;
  if (n > room) {
    buf->truncated = 1;
    n = room;
  }
  if (buf->len + n + 1 > buf->cap) {
    size_t cap = buf->cap ? buf->cap : 65536;
    while (cap < buf->len + n + 1)
      cap *= 2;
    char *grown = realloc(buf->data, cap);
    if (!grown)
      return 0;
    buf->data = grown;
    buf->cap = cap;
  }
  memcpy(buf->data + buf->len, ptr, n);
  buf->len += n;
  buf->data[buf->len] = '\0';
  return buf->truncated ? 0 : size * nmemb;
}

static size_t on_header(char *ptr, size_t size, size_t nmemb, void *userdata) {
  header_state_t *state = userdata;
  size_t n = size * nmemb;
  char line[4096];
  char *colon, *value, *end;

  if (n >= sizeof(line))
    return n;
  memcpy(line, ptr, n);
  line[n] = '\0';
  line[strcspn(line, "\r\n")] = '\0';

  /* Each redirect hop starts a new header block; keep only the final one. */
  if (!strncmp(line, "HTTP/", 5)) {
    json_object_clear(state->headers);
    state->status_text[0] = '\0';
    char *reason = strchr(line, ' ');
    if (reason && (reason = strchr(reason + 1, ' ')))
      snprintf(state->status_text, sizeof(state->status_text), "%s", reason + 1);
    return n;
  }
  if (!(colon = strchr(line, ':')))
    return n;
  *colon = '\0';
  for (char *p = line; *p; p++)
    if (*p >= 'A' && *p <= 'Z')
      *p = (char)(*p - 'A' + 'a');
  value = colon + 1;
  while (*value == ' ' || *value == '\t')
    value++;
  end = value + strlen(value);
  while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
    *--end = '\0';

  json_t *existing = json_object_get(state->headers, line);
  if (existing && json_is_string(existing)) {
    char joined[8192];
    snprintf(joined, sizeof(joined), "%s, %s", json_string_value(existing), value);
    json_object_set_new(state->headers, line, json_string(joined));
  } else {
    json_t *str = json_string(value);
    if (str)
      json_object_set_new(state->headers, line, str);
  }
  return n;
}

static int on_progress(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow) {
  progress_state_t *state = userdata;
  (void)dltotal, (void)dlnow, (void)ultotal, (void)ulnow;
  return state->slot >= 0 && g_requests[state->slot].cancelled ? 1 : 0;
}

/* ---- body decoding -------------------------------------------------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const uint8_t *data, size_t len) {
  size_t out_len = 4 * ((len + 2) / 3);
  char *out = malloc(out_len + 1), *p = out;
  size_t i;

  if (!out)
    return NULL;
  for (i = 0; i + 2 < len; i += 3) {
    *p++ = B64[data[i] >> 2];
    *p++ = B64[((data[i] & 3) << 4) | (data[i + 1] >> 4)];
    *p++ = B64[((data[i + 1] & 15) << 2) | (data[i + 2] >> 6)];
    *p++ = B64[data[i + 2] & 63];
  }
  if (i < len) {
    *p++ = B64[data[i] >> 2];
    if (i + 1 < len) {
      *p++ = B64[((data[i] & 3) << 4) | (data[i + 1] >> 4)];
      *p++ = B64[(data[i + 1] & 15) << 2];
    } else {
      *p++ = B64[(data[i] & 3) << 4];
      *p++ = '=';
    }
    *p++ = '=';
  }
  *p = '\0';
  return out;
}

static int b64_value(int c) {
  const char *pos = c ? strchr(B64, c) : NULL;
  return pos ? (int)(pos - B64) : -1;
}

static uint8_t *base64_decode(const char *text, size_t *out_len) {
  size_t len = strlen(text);
  uint8_t *out = malloc(len / 4 * 3 + 3);
  size_t o = 0;
  int acc = 0, bits = 0;

  if (!out)
    return NULL;
  for (size_t i = 0; i < len; i++) {
    int v = b64_value((unsigned char)text[i]);
    if (v < 0)
      continue; /* padding and whitespace */
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (uint8_t)((acc >> bits) & 0xff);
    }
  }
  *out_len = o;
  return out;
}

/* Latin-1 → UTF-8, used when the provider declares an ISO-8859/Windows charset. */
static char *latin1_to_utf8(const uint8_t *data, size_t len) {
  char *out = malloc(len * 2 + 1), *p = out;
  if (!out)
    return NULL;
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x80) {
      *p++ = (char)data[i];
    } else {
      *p++ = (char)(0xc0 | (data[i] >> 6));
      *p++ = (char)(0x80 | (data[i] & 0x3f));
    }
  }
  *p = '\0';
  return out;
}

/* Copies UTF-8, replacing invalid sequences with U+FFFD like Node's decoder. */
static char *sanitize_utf8(const uint8_t *s, size_t len) {
  char *out = malloc(len * 3 + 1), *p = out;
  size_t i = 0;

  if (!out)
    return NULL;
  while (i < len) {
    uint8_t c = s[i];
    size_t need = 0;
    uint32_t cp = c;
    int valid = 1;

    if (c == 0) {
      valid = 0; /* body text is a C string; NUL becomes U+FFFD */
    } else if (c >= 0x80) {
      if ((c & 0xe0) == 0xc0) {
        need = 1;
        cp = c & 0x1f;
      } else if ((c & 0xf0) == 0xe0) {
        need = 2;
        cp = c & 0x0f;
      } else if ((c & 0xf8) == 0xf0) {
        need = 3;
        cp = c & 0x07;
      } else {
        valid = 0;
      }
    }
    for (size_t k = 1; valid && k <= need; k++) {
      if (i + k >= len || (s[i + k] & 0xc0) != 0x80)
        valid = 0;
      else
        cp = (cp << 6) | (s[i + k] & 0x3f);
    }
    if (valid && need && ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
                          (need == 3 && (cp < 0x10000 || cp > 0x10ffff)) ||
                          (cp >= 0xd800 && cp <= 0xdfff)))
      valid = 0;
    if (valid) {
      memcpy(p, s + i, need + 1);
      p += need + 1;
      i += need + 1;
    } else {
      *p++ = (char)0xef;
      *p++ = (char)0xbf;
      *p++ = (char)0xbd;
      i++;
    }
  }
  *p = '\0';
  return out;
}

static int is_latin1_charset(const char *content_type) {
  const char *charset = content_type ? strcasestr(content_type, "charset=") : NULL;
  if (!charset)
    return 0;
  charset += 8;
  if (*charset == '"')
    charset++;
  return !strncasecmp(charset, "iso-8859-1", 10) || !strncasecmp(charset, "latin1", 6) ||
         !strncasecmp(charset, "windows-1252", 12) || !strncasecmp(charset, "us-ascii", 8);
}

/* ---- routes -------------------------------------------------------------- */

static int header_present(json_t *headers, const char *name) {
  const char *key;
  json_t *value;
  json_object_foreach(headers, key, value) {
    if (!strcasecmp(key, name))
      return 1;
  }
  return 0;
}

static char *handle_fetch(json_t *payload, int *status) {
  const char *url = json_string_value(json_object_get(payload, "url"));
  const char *method = json_string_value(json_object_get(payload, "method"));
  const char *body = json_string_value(json_object_get(payload, "body"));
  const char *body_b64 = json_string_value(json_object_get(payload, "bodyBase64"));
  const char *body_kind = json_string_value(json_object_get(payload, "bodyKind"));
  const char *encoding = json_string_value(json_object_get(payload, "responseEncoding"));
  const char *request_id = json_string_value(json_object_get(payload, "requestId"));
  json_t *headers = json_object_get(payload, "headers");
  json_int_t timeout_ms = json_integer_value(json_object_get(payload, "timeoutMs"));
  json_int_t max_bytes = json_integer_value(json_object_get(payload, "maxResponseBytes"));
  buffer_t response = {0};
  header_state_t header_state = {0};
  progress_state_t progress = {-1};
  struct curl_slist *header_list = NULL;
  uint8_t *binary_body = NULL;
  size_t binary_len = 0;
  char errbuf[CURL_ERROR_SIZE] = {0};
  long http_status = 0;
  CURLcode rc;
  CURL *curl;
  char *reply = NULL;
  int has_ua = 0;

  if (!url || (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)))
    return error_reply(status, 400, "Only http and https plugin URLs are allowed", request_id);
  if (!method || !method[0])
    method = "GET";
  if (timeout_ms <= 0)
    timeout_ms = DEFAULT_TIMEOUT_MS;
  if (timeout_ms > 120000)
    timeout_ms = 120000;
  if (max_bytes <= 0)
    max_bytes = DEFAULT_RESPONSE_BYTES;
  if (max_bytes > MAX_RESPONSE_BYTES)
    max_bytes = MAX_RESPONSE_BYTES;

  if (acquire_slot((int)timeout_ms) != 0)
    return error_reply(status, 503, "Plugin service is busy", request_id);
  progress.slot = track_request(request_id);

  if (!(curl = curl_easy_init())) {
    untrack_request(progress.slot);
    release_slot(1);
    return error_reply(status, 500, "curl_easy_init failed", request_id);
  }

  header_state.headers = json_object();
  response.limit = (size_t)max_bytes;

  if (json_is_object(headers)) {
    const char *key;
    json_t *value;
    json_object_foreach(headers, key, value) {
      char line[8192];
      const char *text = json_string_value(value);
      if (!text || !strcasecmp(key, "accept-encoding") || !strcasecmp(key, "host") ||
          !strcasecmp(key, "content-length"))
        continue;
      if (!strcasecmp(key, "user-agent"))
        has_ua = 1;
      if (text[0])
        snprintf(line, sizeof(line), "%s: %s", key, text);
      else
        snprintf(line, sizeof(line), "%s;", key);
      header_list = curl_slist_append(header_list, line);
    }
  }
  if (!has_ua)
    header_list = curl_slist_append(header_list, "User-Agent: " DEFAULT_USER_AGENT);
  header_list = curl_slist_append(header_list, "Expect:");

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)MAX_REDIRECTS);
  curl_easy_setopt(curl, CURLOPT_POSTREDIR, 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)CONNECT_TIMEOUT_MS);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_state);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
  if (!header_present(headers, "Range"))
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  {
    struct curl_blob blob = {(void *)cacert_pem, cacert_pem_size, CURL_BLOB_NOCOPY};
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
  }

  if (!strcasecmp(method, "GET")) {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else if (!strcasecmp(method, "HEAD")) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else {
    int send_body = !strcasecmp(method, "POST") || !strcasecmp(method, "PUT") ||
                    !strcasecmp(method, "PATCH") ||
                    (!strcasecmp(method, "DELETE") && body_kind && strcmp(body_kind, "none"));
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (send_body) {
      if (body_kind && !strcmp(body_kind, "base64") && body_b64) {
        binary_body = base64_decode(body_b64, &binary_len);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)binary_len);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, binary_body ? (char *)binary_body : "");
      } else {
        const char *text = body ? body : "";
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(text));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, text);
      }
    }
  }

  rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

  if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && response.truncated)) {
    char message[512];
    int cancelled = progress.slot >= 0 && g_requests[progress.slot].cancelled;
    snprintf(message, sizeof(message), "%s",
             cancelled ? "Plugin request cancelled" : errbuf[0] ? errbuf : curl_easy_strerror(rc));
    nuvio_log("plugin: %s %s failed: %s", method, url, message);
    reply = error_reply(status, 502, message, request_id);
  } else {
    json_t *obj = json_object();
    const char *content_type = json_string_value(json_object_get(header_state.headers, "content-type"));
    const uint8_t *bytes = (const uint8_t *)(response.data ? response.data : "");
    char *text = is_latin1_charset(content_type) ? latin1_to_utf8(bytes, response.len)
                                                 : sanitize_utf8(bytes, response.len);

    json_object_set_new(obj, "returnValue", json_true());
    if (request_id)
      json_object_set_new(obj, "requestId", json_string(request_id));
    json_object_set_new(obj, "ok", json_boolean(http_status >= 200 && http_status < 300));
    json_object_set_new(obj, "status", json_integer(http_status));
    json_object_set_new(obj, "statusText", json_string(header_state.status_text));
    json_object_set_new(obj, "url", json_string(url));
    json_object_set_new(obj, "body", json_string(text ? text : ""));
    if (encoding && !strcmp(encoding, "base64")) {
      char *b64 = base64_encode(bytes, response.len);
      json_object_set_new(obj, "bodyBase64", json_string(b64 ? b64 : ""));
      free(b64);
    }
    json_object_set(obj, "headers", header_state.headers);
    json_object_set_new(obj, "truncated", json_boolean(response.truncated));
    free(text);
    *status = 200;
    reply = json_reply(obj);
  }

  curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  json_decref(header_state.headers);
  free(response.data);
  free(binary_body);
  untrack_request(progress.slot);
  release_slot(reply == NULL || *status != 200);
  return reply;
}

char *nuvio_plugin_handle(const char *route, const char *method, const char *body,
                          size_t body_len, int *status) {
  json_t *payload;
  json_error_t error;
  char *reply;

  *status = 200;
  if (!strcmp(route, "health") || !strcmp(route, "capabilities"))
    return json_reply(capabilities_object());

  if (!strcmp(route, "diagnostics")) {
    json_t *obj = json_object();
    pthread_mutex_lock(&g_lock);
    json_object_set_new(obj, "returnValue", json_true());
    json_object_set_new(obj, "protocolVersion", json_integer(PLUGIN_PROTOCOL_VERSION));
    json_object_set_new(obj, "activeRequests", json_integer(g_active));
    json_object_set_new(obj, "queuedRequests", json_integer(0));
    json_object_set_new(obj, "completedRequests", json_integer((json_int_t)g_completed));
    json_object_set_new(obj, "failedRequests", json_integer((json_int_t)g_failed));
    pthread_mutex_unlock(&g_lock);
    json_object_set_new(obj, "recentEvents", json_array());
    return json_reply(obj);
  }

  if (strcmp(method, "POST") != 0)
    return error_reply(status, 404, "Plugin service route not found", NULL);

  if (!strcmp(route, "cache/clear")) {
    json_t *obj = json_object();
    json_object_set_new(obj, "returnValue", json_true());
    json_object_set_new(obj, "cleared", json_true());
    return json_reply(obj);
  }

  payload = json_loadb(body ? body : "{}", body_len ? body_len : 2, 0, &error);
  if (!payload || !json_is_object(payload)) {
    json_decref(payload);
    return error_reply(status, 400, "Invalid JSON request", NULL);
  }

  if (!strcmp(route, "cancel")) {
    const char *id = json_string_value(json_object_get(payload, "requestId"));
    json_t *obj = json_object();
    json_object_set_new(obj, "returnValue", json_true());
    json_object_set_new(obj, "requestId", json_string(id ? id : ""));
    json_object_set_new(obj, "cancelled", json_boolean(cancel_request(id)));
    json_decref(payload);
    return json_reply(obj);
  }

  if (!strcmp(route, "fetch")) {
    reply = handle_fetch(payload, status);
    json_decref(payload);
    return reply;
  }

  json_decref(payload);
  return error_reply(status, 404, "Plugin service route not found", NULL);
}


/* Debug: downloads up to max_bytes from url and reports throughput. */
static size_t discard_body(char *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t *total = userdata;
  (void)ptr;
  *total += size * nmemb;
  return *total > 64u * 1024 * 1024 ? 0 : size * nmemb;
}

char *nuvio_speed_test(const char *url, long buffer_bytes) {
  CURL *curl = curl_easy_init();
  size_t total = 0;
  double seconds = 0;
  char range[64];
  char *out = malloc(256);
  CURLcode rc;

  if (!curl || !out) {
    free(out);
    return NULL;
  }
  snprintf(range, sizeof(range), "0-%d", 16 * 1024 * 1024 - 1);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_RANGE, range);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &total);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  if (buffer_bytes > 0)
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, buffer_bytes);
  rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &seconds);
  snprintf(out, 256, "{\"returnValue\":true,\"curl\":%d,\"bytes\":%zu,\"seconds\":%.2f,\"mbps\":%.2f}",
           (int)rc, total, seconds, seconds > 0 ? total / seconds / 1e6 : 0);
  curl_easy_cleanup(curl);
  return out;
}
