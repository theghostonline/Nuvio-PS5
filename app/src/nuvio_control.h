#pragma once
/*
 * The app's background line to the payload while it runs: takes queued
 * commands (GET /api/player/control) and reports playback progress
 * (POST /api/player/state), so the render loop never waits on a socket.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Starts the worker (once). */
void nuvio_control_start(void);

/* Whether the player is up: commands are taken every 100 ms while it is,
 * every 250 ms otherwise; key commands only reach a playing player. */
void nuvio_control_set_playing(int playing);

/* 1 once after a "stop" command. */
int nuvio_control_take_stop(void);

/* 1 after a "quit" command (stays set). */
int nuvio_control_quit_requested(void);

/* Latest playback position for the next progress report (sent every 5 s).
 * The final state is posted by the player itself, synchronously, so it is
 * at the payload before Nuvio's page reopens and asks for it. */
void nuvio_control_report(const char *id, double position, double duration);

#ifdef __cplusplus
}
#endif
