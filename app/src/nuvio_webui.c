#include "nuvio_webui.h"

#include "evo_boot_log.h"
#include "evo_boot_trace.h"
#include "nuvio_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* libSceWebBrowserDialog parameter layout, after SharpProspero and EVO
 * Player's hardware-verified structs; the same full-screen rectangle the
 * nuvio-ps5 launcher opened on firmware 13.60. */
typedef struct {
    uint64_t size;
    uint8_t  reserved[36];
    uint32_t magic;
} common_dialog_base;

typedef struct {
    common_dialog_base base;
    uint64_t size;
    int32_t  mode;            /* 1 default, 2 custom rectangle */
    int32_t  user_id;
    const char *url;
    void    *callback_init;
    uint16_t width;
    uint16_t height;
    uint16_t pos_x;
    uint16_t pos_y;
    uint32_t parts;
    uint16_t header_width;
    uint16_t header_x;
    uint16_t header_y;
    uint16_t pad0;
    uint32_t control;
    void    *ime_param;
    void    *webview_param;
    uint32_t animation;
    uint8_t  reserved[202];
    uint16_t tail_pad;
} web_dialog_param;

typedef struct {
    int32_t result;
    int32_t pad0;
    void   *callback_result;
    uint8_t reserved[240];
} web_dialog_result;

_Static_assert(sizeof(common_dialog_base) == 48, "dialog base size");
_Static_assert(sizeof(web_dialog_param) == 328, "dialog param size");
_Static_assert(sizeof(web_dialog_result) == 256, "dialog result size");

#define SYSMODULE_WEB_BROWSER_DIALOG 0x00AB
#define DIALOG_MAGIC 0xC0D1A109u

enum { STATUS_NONE = 0, STATUS_INITIALIZED = 1, STATUS_RUNNING = 2, STATUS_FINISHED = 3 };

int sceSysmoduleLoadModule(uint16_t id);
int sceCommonDialogInitialize(void);
int sceWebBrowserDialogInitialize(void);
int sceWebBrowserDialogOpen(web_dialog_param *param);
int sceWebBrowserDialogUpdateStatus(void);
int sceWebBrowserDialogGetResult(web_dialog_result *result);
int sceWebBrowserDialogClose(void);
int sceUserServiceGetForegroundUser(int *user_id);
int sceUserServiceGetInitialUser(int *user_id);

/* Its magic encodes this block's address, so it must not move while open. */
static web_dialog_param s_param __attribute__((aligned(16)));
static char s_url[512];
static int s_open;

static int pick_user(void)
{
    int user_id = -1;
    if (sceUserServiceGetForegroundUser(&user_id) == 0 && user_id != -1 && user_id != 0xff)
        return user_id;
    if (sceUserServiceGetInitialUser(&user_id) == 0)
        return user_id;
    return -1;
}

int nuvio_webui_init(void)
{
    int rc = sceSysmoduleLoadModule(SYSMODULE_WEB_BROWSER_DIALOG);
    evo_bt("webui: load module -> 0x%08x", (unsigned)rc);
    rc = sceCommonDialogInitialize();
    evo_bt("webui: common dialog init -> 0x%08x (0x80b80002 = already up)", (unsigned)rc);
    rc = sceWebBrowserDialogInitialize();
    evo_bt("webui: browser dialog init -> 0x%08x", (unsigned)rc);
    return rc;
}

static int open_mode(int mode)
{
    memset(&s_param, 0, sizeof s_param);
    s_param.base.size = sizeof s_param.base;
    s_param.base.magic = (uint32_t)(DIALOG_MAGIC + (uint64_t)(uintptr_t)&s_param.base);
    s_param.size = sizeof s_param;
    s_param.mode = mode;
    s_param.user_id = pick_user();
    s_param.url = s_url;
    if (mode == 2) {
        /* The whole screen, without the browser's header and footer. */
        s_param.width = 1920;
        s_param.height = 1080;
        s_param.header_width = 1920;
    }
    return sceWebBrowserDialogOpen(&s_param);
}

int nuvio_webui_open(const char *path)
{
    int rc;
    snprintf(s_url, sizeof s_url, "http://127.0.0.1:%d%s", NUVIO_SERVICE_PORT,
             (path && path[0] == '/') ? path : "/");
    rc = open_mode(2);
    if (rc != 0) {
        evo_bt("webui: full-screen open -> 0x%08x, trying the default layout", (unsigned)rc);
        rc = open_mode(1);
    }
    evo_bt("webui: open %s -> 0x%08x", s_url, (unsigned)rc);
    s_open = rc == 0;
    return rc;
}

int nuvio_webui_running(void)
{
    int status;
    web_dialog_result result;

    if (!s_open)
        return 0;
    status = sceWebBrowserDialogUpdateStatus();
    if (status == STATUS_RUNNING || status == STATUS_INITIALIZED)
        return 1;
    memset(&result, 0, sizeof result);
    sceWebBrowserDialogGetResult(&result);
    evo_bt("webui: dialog ended (status %d, result %d)", status, result.result);
    sceWebBrowserDialogClose();
    s_open = 0;
    return 0;
}

void nuvio_webui_close(void)
{
    if (!s_open)
        return;
    evo_bt("webui: closing for playback");
    sceWebBrowserDialogClose();
    /* FINISHED follows a running dialog's Close by ~25 frames. */
    for (int i = 0; i < 120 && nuvio_webui_running(); i++)
        usleep(16 * 1000);
    s_open = 0;
}
