#ifndef EVO_THREAD_H
#define EVO_THREAD_H

#include <pthread.h>

/*
 * Playback threads run FFmpeg and the console's decoder libraries, and the
 * console's default pthread stack is too small for them: an ordinary H.264
 * stream overflowed it inside sceVideodec2Decode (SIGSEGV on the guard page
 * of the video decode thread). Every thread that decodes, demuxes or opens a
 * stream is created through this instead, with an explicit stack.
 */
#define EVO_THREAD_STACK_BYTES (1u << 20)

static inline int evo_thread_create(pthread_t *thread, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    int rc;

    if (pthread_attr_init(&attr) != 0)
        return pthread_create(thread, NULL, fn, arg);
    pthread_attr_setstacksize(&attr, EVO_THREAD_STACK_BYTES);
    rc = pthread_create(thread, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

#endif
