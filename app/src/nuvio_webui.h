#pragma once
/*
 * Nuvio's web UI in the system browser (libSceWebBrowserDialog), full screen.
 *
 * The page is served by the nuvio-ps5 payload on 127.0.0.1:17600. The dialog
 * is opaque (it cannot sit over the app's video), so it is closed while the
 * Nuvio Player plays and opened again afterwards.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Loads and initialises the dialog library. Call once, early. 0 on success. */
int nuvio_webui_init(void);

/* Opens the page (path relative to the payload, e.g. "/"). 0 on success. */
int nuvio_webui_open(const char *path);

/* 1 while the dialog is up. Pumps the library; call every frame or so. */
int nuvio_webui_running(void);

/* Closes a running dialog and waits (up to ~2 s) until it is gone. */
void nuvio_webui_close(void);

#ifdef __cplusplus
}
#endif
