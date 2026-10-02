/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * Nuvio for PS5 (PPSA99176).
 *
 * Nuvio's interface is its TV web app, served by the nuvio-ps5 payload and
 * shown full screen in the system browser. When the page picks a stream it
 * hands it to the payload; this app takes it from there, closes the browser,
 * plays it on the Nuvio Player (nuvio_player.cpp) and opens Nuvio again.
 */
#include "nuvio_bridge.h"
#include "nuvio_control.h"
#include "nuvio_player.h"
#include "nuvio_webui.h"

#include "evo_adec.h"
#include "evo_agc_runtime.h"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"
#include "evo_direct_mem.h"
#include "evo_hw.h"
#include "evo_vdec.h"
#include "pp_playback.h"

#include <arpa/inet.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include <libavutil/cpu.h>
#include <libavutil/log.h>

int sceUserServiceInitialize(void *params);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
int scePadInit(void);
int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
int sceSystemServiceHideSplashScreen(void);
extern int g_ps5_user_id;
extern pp_playback g_pp_pb;
}

namespace evo {
extern int DisplayWidth;
extern int DisplayHeight;
}

namespace {

/* 128 MiB of direct memory for decode surfaces, as EVO Player sizes it. */
constexpr size_t kDirectMemPool = 128u * 1024u * 1024u;

void notify(const char *text)
{
    struct { char pad[45]; char msg[3075]; } n;
    std::memset(&n, 0, sizeof n);
    std::snprintf(n.msg, sizeof n.msg, "%s", text);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
}

void av_log_to_boot_log(void *, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_WARNING)
        return;
    char line[512];
    int n = std::vsnprintf(line, sizeof line, fmt, vl);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line)
        n = (int)sizeof line - 1;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n)
        evo_bt("ffmpeg[%d]: %s", level, line);
}

/*
 * A crash report straight to the payload's log (GET /api/logs): most
 * consoles have no USB stick for evo.log. Only async-signal-safe calls.
 */
void crash_handler(int sig, siginfo_t *si, void *)
{
    char body[160];
    int len = std::snprintf(body, sizeof body, "[evo] CRASH signal=%d addr=%p\n", sig,
                            si ? si->si_addr : nullptr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && len > 0) {
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(NUVIO_SERVICE_PORT);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            char head[128];
            int hl = std::snprintf(head, sizeof head,
                                   "POST /api/log HTTP/1.0\r\nContent-Length: %d\r\n\r\n", len);
            (void)write(fd, head, (size_t)hl);
            (void)write(fd, body, (size_t)len);
        }
        close(fd);
    }
    /* Not _exit(): that faults in an app process (SIGSYS) and the kernel then
     * reports the exit, not the crash. With the default action back, the
     * faulting instruction runs again and the kernel's crash report (klog:
     * registers, backtrace, libraries) describes the real fault. */
    signal(sig, SIG_DFL);
}

extern "C" __attribute__((noinline, used)) void main_entry_marker(void) {}

void install_crash_handler()
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

/* Display, decoders and controller, in EVO Player's order: the decoder probes
 * and module loads run before anything else touches the process. */
int init_hardware()
{
    evo_vdec_probe();
    evo_adec_native_probe();
    evo_hw_probe();

    if (evo_agc_runtime_init(evo::DisplayWidth, evo::DisplayHeight, 0) != 0) {
        evo_bt("nuvio: display init failed");
        return -1;
    }
    evo_agc_runtime_get_size(&evo::DisplayWidth, &evo::DisplayHeight);
    evo_agc_runtime_frame_begin();
    evo_agc_runtime_present();
    evo_bt("nuvio: display %dx%d", evo::DisplayWidth, evo::DisplayHeight);

    /* A game-category app keeps the system's launch splash (sce_sys/pic0,
     * the Nuvio art) over everything - the browser dialog included - until it
     * says it is ready. Once a frame is up, it is. */
    evo_bt("nuvio: hide splash -> %d", sceSystemServiceHideSplashScreen());

    nuvio_webui_init();

    av_log_set_callback(av_log_to_boot_log);
    av_log_set_level(AV_LOG_WARNING);
    av_force_cpu_flags(0);
    evo_direct_mem_init(kDirectMemPool);

    sceUserServiceInitialize(nullptr);
    scePadInit();
    int users[4] = {0};
    sceUserServiceGetLoginUserIdList(users);
    g_ps5_user_id = users[0];
    evo_bt("nuvio: user %d", users[0]);
    nuvio_player_init(users[0]);
    return 0;
}

void wait_for_service()
{
    bool warned = false;
    while (!nuvio_service_up()) {
        if (!warned) {
            warned = true;
            notify("Nuvio: waiting for the Nuvio payload\n"
                   "Load nuvio-ps5.elf, or add it to your autoloader");
        }
        usleep(1 * 1000 * 1000);
    }
}

/* A "quit" command: leave the way EVO's QUIT does - playback already
 * stopped, the GPU drained and VideoOut closed before the process ends, so
 * the kernel never reclaims memory the GPU might still be using. */
int shutdown_app()
{
    evo_bt("nuvio: quit");
    nuvio_webui_close();
    pp_playback_shutdown(&g_pp_pb);
    evo_agc_runtime_shutdown();
    evo_bt("nuvio: shut down");
    evo_boot_log_flush();
    usleep(1000 * 1000);   /* the log thread posts every 500 ms */
    return 0;
}

/* Imports the console left NULL (see scripts/build.sh). The weak stand-ins
 * cover the first link pass, before the generated table exists. */
extern "C" __attribute__((weak)) int nuvio_import_count(void) { return 0; }
extern "C" __attribute__((weak)) const char *nuvio_import_null(int) { return nullptr; }

void check_imports()
{
    int missing = 0;
    for (int i = 0; i < nuvio_import_count(); i++)
        if (const char *name = nuvio_import_null(i)) {
            evo_bt("nuvio: import %s is NULL on this console", name);
            missing++;
        }
    evo_bt("nuvio: %d imports, %d NULL", nuvio_import_count(), missing);
}

} // namespace

int main()
{
    install_crash_handler();
    evo_bt("nuvio: app start");
    check_imports();
    if (init_hardware() != 0) {
        notify("Nuvio: the display could not be started");
        for (;;)
            usleep(1 * 1000 * 1000);
    }
    wait_for_service();
    nuvio_control_start();

    /* The browser dialog is the app's whole interface. It stays up until the
     * page queues a stream; a dialog the user closes is simply reopened, and
     * the app is left with the PS button like any other. */
    /* After a playback the page is told so (?ps5return=1): it then picks the
     * session up, and never shows the profile picker, even if the context
     * that would take it back to the stream list did not survive. */
    bool after_play = false;
    for (;;) {
        if (nuvio_webui_open(after_play ? "/?ps5return=1" : "/") != 0) {
            if (nuvio_control_quit_requested())   /* a quit must not wait on the browser */
                return shutdown_app();
            notify("Nuvio: the browser could not open");
            usleep(3 * 1000 * 1000);
            continue;
        }
        char *req = nullptr;
        int got = 0;
        int tick = 0;
        while (nuvio_webui_running() && !nuvio_control_quit_requested()) {
            /* ~50 ms: the page has already gone black for the hand-off. */
            if ((tick++ % 3) == 0 && (got = nuvio_bridge_next(&req)) == 1)
                break;
            usleep(16 * 1000);
        }
        if (nuvio_control_quit_requested()) {
            free(req);
            return shutdown_app();
        }
        if (got == 1) {
            nuvio_webui_close();
            nuvio_player_run(req);
            after_play = true;
            free(req);
            if (nuvio_control_quit_requested())
                return shutdown_app();
        }
        evo_boot_log_flush();
    }
}
