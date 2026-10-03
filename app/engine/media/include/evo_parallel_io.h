/*
 * evo_parallel_io.h - Nuvio PS5: parallel read-ahead for big network files.
 */
#ifndef EVO_PARALLEL_IO_H
#define EVO_PARALLEL_IO_H

#include <libavformat/avio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evo_pio evo_pio;

/* NULL when the URL is not a big file served with byte ranges (then FFmpeg's
 * own single connection is used as before). */
evo_pio *evo_pio_open(const char *url, const char *headers, const char *user_agent);
AVIOContext *evo_pio_avio(evo_pio *p);
/* Fail the reads that are waiting (playback is being stopped). */
void evo_pio_abort(evo_pio *p);
/* After avformat_close_input(). */
void evo_pio_close(evo_pio *p);

#ifdef __cplusplus
}
#endif

#endif
