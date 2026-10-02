#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nuvio.h"

typedef struct notify_request {
  char reserved[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *request,
                                     size_t size, int blocking);

void nuvio_notify(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;

  memset(&req, 0, sizeof(req));
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);

  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  nuvio_log("notify: %s", req.message);
}
