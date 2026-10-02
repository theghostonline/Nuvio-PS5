/*
 * nuvio-shell: runs as Nuvio's foreground app (BigApp, launched by the
 * nuvio-ps5 payload through hbldr) and shows the Nuvio web app full screen in
 * the PS5's system browser dialog, without the windowed browser's title bar.
 *
 * libSceWebBrowserDialog layout after SharpProspero
 * (Interop/Dialog/WebBrowserDialog.cs) as verified on hardware by EVO Player
 * (sainsaji/EVO-PLAYER-PS5, src/evo_webui.c).
 */

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NUVIO_URL "http://127.0.0.1:17600/"
#define SHELL_LOG "/data/nuvio/shell.log"
/* Written by the payload to close the dialog (e.g. native player handoff). */
#define CLOSE_REQUEST "/data/nuvio/shell.close"

#define SCE_SYSMODULE_WEB_BROWSER_DIALOG 0x00AB
#define WBD_MAGIC 0xC0D1A109u

enum {
  CDLG_STATUS_NONE = 0,
  CDLG_STATUS_INITIALIZED = 1,
  CDLG_STATUS_RUNNING = 2,
  CDLG_STATUS_FINISHED = 3
};

typedef struct {
  uint64_t size;
  uint8_t reserved[36];
  uint32_t magic; /* WBD_MAGIC + address of this block */
} common_dialog_base_t;

typedef struct {
  common_dialog_base_t base; /* 0 */
  uint64_t size;             /* 48 */
  int32_t mode;              /* 56: 1 default, 2 custom rectangle */
  int32_t user_id;           /* 60 */
  const char *url;           /* 64 */
  void *callback_init;       /* 72 */
  uint16_t width, height;    /* 80 */
  uint16_t pos_x, pos_y;     /* 84 */
  uint32_t parts;            /* 88 */
  uint16_t header_width;     /* 92 */
  uint16_t header_x, header_y; /* 94 */
  uint16_t pad0;             /* 98 */
  uint32_t control;          /* 100 */
  void *ime_param;           /* 104 */
  void *webview_param;       /* 112 */
  uint32_t animation;        /* 120 */
  uint8_t reserved[202];     /* 124 */
  uint16_t tail_pad;         /* 326 */
} web_browser_dialog_param_t;

typedef struct {
  int32_t result;
  int32_t pad0;
  void *callback_result;
  uint8_t reserved[240];
} web_browser_dialog_result_t;

_Static_assert(sizeof(common_dialog_base_t) == 48, "CommonDialogBaseParam");
_Static_assert(sizeof(web_browser_dialog_param_t) == 328, "WebBrowserDialogParam");
_Static_assert(sizeof(web_browser_dialog_result_t) == 256, "WebBrowserDialogResult");

int sceSysmoduleLoadModule(uint16_t id);
int sceCommonDialogInitialize(void);
int sceWebBrowserDialogInitialize(void);
int sceWebBrowserDialogOpen(web_browser_dialog_param_t *param);
int sceWebBrowserDialogUpdateStatus(void);
int sceWebBrowserDialogGetResult(web_browser_dialog_result_t *result);
int sceWebBrowserDialogClose(void);
int sceWebBrowserDialogTerminate(void);
int sceUserServiceInitialize(void *);
int sceUserServiceGetInitialUser(int *user_id);
int sceUserServiceGetForegroundUser(int *user_id);

/* Must stay valid while the dialog runs: its magic encodes the address. */
static web_browser_dialog_param_t g_param __attribute__((aligned(16)));

static void shell_log(const char *fmt, ...) {
  char line[512];
  struct tm tm;
  time_t now = time(NULL);
  va_list ap;
  int n, fd;

  localtime_r(&now, &tm);
  n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d [shell] ", tm.tm_year + 1900,
               tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof(line) - 2)
    n = (int)sizeof(line) - 2;
  line[n++] = '\n';
  if ((fd = open(SHELL_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644)) >= 0) {
    (void)write(fd, line, (size_t)n);
    close(fd);
  }
  (void)write(1, line, (size_t)n);
}

static int pick_user(void) {
  int uid = -1;
  if (sceUserServiceGetForegroundUser(&uid) == 0 && uid != -1 && uid != 0xff)
    return uid;
  if (sceUserServiceGetInitialUser(&uid) == 0 && uid != -1 && uid != 0xff)
    return uid;
  return uid;
}

static int open_dialog(int user_id, int mode) {
  memset(&g_param, 0, sizeof(g_param));
  g_param.base.size = sizeof(g_param.base);
  g_param.base.magic = (uint32_t)(WBD_MAGIC + (uint64_t)(uintptr_t)&g_param.base);
  g_param.size = sizeof(g_param);
  g_param.mode = mode;
  g_param.user_id = user_id;
  g_param.url = NUVIO_URL;
  if (mode == 2) {
    /* Whole screen, no header or footer parts. */
    g_param.pos_x = 0;
    g_param.pos_y = 0;
    g_param.width = 1920;
    g_param.height = 1080;
    g_param.parts = 0;
    g_param.control = 0;
    g_param.header_width = 1920;
  }
  return sceWebBrowserDialogOpen(&g_param);
}

int main(void) {
  web_browser_dialog_result_t result;
  struct stat st;
  int user_id, rc, status = -1, last_status = -1;

  syscall(SYS_thr_set_name, -1, "nuvio-shell.elf");
  shell_log("starting");

  rc = sceUserServiceInitialize(NULL);
  shell_log("sceUserServiceInitialize -> 0x%08x", (unsigned)rc);
  rc = sceSysmoduleLoadModule(SCE_SYSMODULE_WEB_BROWSER_DIALOG);
  shell_log("sceSysmoduleLoadModule(WebBrowserDialog) -> 0x%08x", (unsigned)rc);
  rc = sceCommonDialogInitialize();
  shell_log("sceCommonDialogInitialize -> 0x%08x (0x80b80002 = already up)", (unsigned)rc);
  rc = sceWebBrowserDialogInitialize();
  shell_log("sceWebBrowserDialogInitialize -> 0x%08x", (unsigned)rc);
  if (rc != 0)
    return 1;

  user_id = pick_user();
  unlink(CLOSE_REQUEST);

  rc = open_dialog(user_id, 2);
  shell_log("open custom full screen (user 0x%08x) -> 0x%08x", (unsigned)user_id, (unsigned)rc);
  if (rc != 0) {
    rc = open_dialog(user_id, 1);
    shell_log("open default mode -> 0x%08x", (unsigned)rc);
  }
  if (rc != 0) {
    sceWebBrowserDialogTerminate();
    return 2;
  }

  for (unsigned tick = 0;; tick++) {
    status = sceWebBrowserDialogUpdateStatus();
    if (status != last_status) {
      shell_log("dialog status %d", status);
      last_status = status;
    }
    if (status == CDLG_STATUS_FINISHED || status == CDLG_STATUS_NONE ||
        (status == CDLG_STATUS_INITIALIZED && tick > 120) || status < 0)
      break;
    if (tick % 30 == 0 && stat(CLOSE_REQUEST, &st) == 0) {
      unlink(CLOSE_REQUEST);
      shell_log("close requested by the payload");
      sceWebBrowserDialogClose();
    }
    usleep(16 * 1000);
  }

  memset(&result, 0, sizeof(result));
  rc = sceWebBrowserDialogGetResult(&result);
  shell_log("dialog finished: GetResult -> 0x%08x result=0x%08x", (unsigned)rc,
            (unsigned)result.result);
  sceWebBrowserDialogTerminate();
  shell_log("exiting");
  return 0;
}
