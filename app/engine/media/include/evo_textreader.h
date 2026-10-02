/*
 * evo_textreader — reading plain text on a television, from a controller.
 *
 * Opens .txt/.log/.md and the subtitle formats the player already understands,
 * wraps the text to the screen and scrolls it. No decoder, no VideoOut: this
 * is a buffer of lines and an offset, which is why it lives here as its own
 * module and can be rendered on the host by tools/uiview.sh.
 *
 * WHAT A READER ON A TV HAS TO GET RIGHT, AND WHAT THIS DOES ABOUT IT
 *
 *   Encoding. Real text is UTF-8 and full of characters the font has never
 *   had — curly quotes, em dashes, ellipses, non-breaking spaces. The atlas
 *   gained ASCII punctuation (see evo_font.h); everything beyond that is
 *   folded to its nearest ASCII spelling at load time rather than drawn as a
 *   hole. A file that is not valid UTF-8 still renders: bytes that cannot be
 *   decoded become a middle dot instead of aborting the load.
 *
 *   Size. A log can be hundreds of megabytes and the console has other plans
 *   for its RAM, so the loader reads at most EVO_TEXT_MAX_BYTES and says so
 *   in the UI rather than pretending the file ended.
 *
 *   Long lines. Logs and minified files have lines thousands of characters
 *   long. Wrapping is by word where a word fits and by character where it
 *   does not, so a single 4000-character token cannot produce one unreadable
 *   line or an infinite loop.
 *
 *   Position. Scrolling is in wrapped display lines, not source lines, so the
 *   scrollbar tracks what is actually on screen. Re-wrapping on a font-size
 *   change keeps the top line stable, because the alternative — keeping the
 *   line *number* — throws the reader to a different part of the document
 *   every time the size changes.
 */
#ifndef EVO_TEXTREADER_H
#define EVO_TEXTREADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cap on what is read from disk. Two megabytes is far more text than anyone
 * will scroll through on a controller, and small enough that the load is
 * imperceptible next to opening the file. */
#define EVO_TEXT_MAX_BYTES (2u * 1024u * 1024u)

/* Ceiling on wrapped display lines, which is what actually bounds memory:
 * one pointer and a length per line. */
#define EVO_TEXT_MAX_LINES 60000

typedef enum {
    EVO_TEXT_OK = 0,
    EVO_TEXT_ERR_OPEN,        /* could not open the file */
    EVO_TEXT_ERR_EMPTY,       /* opened, nothing in it */
    EVO_TEXT_ERR_MEMORY,      /* allocation failed */
    EVO_TEXT_ERR_BINARY       /* looks like a binary, refused */
} evo_text_status;

typedef struct {
    const char *begin;        /* into the loaded buffer; not NUL-terminated */
    int         len;
    int         source_line;  /* 1-based line in the file, for the position readout */
} evo_text_line;

typedef struct {
    char           *buf;            /* the decoded, folded document */
    size_t          buf_len;

    evo_text_line  *lines;          /* wrapped display lines */
    int             line_count;
    int             line_cap;

    int             top;            /* first visible display line */
    int             face;           /* 0 SMALL .. 3 TITLE - the reading size */
    int             wrap_px;        /* width the current wrap was built for */
    int             wrap_face;      /* ...and the face, which changes every advance */

    int             source_lines;   /* lines in the file itself */
    int             truncated;      /* the file was larger than the cap */
    size_t          file_bytes;     /* true size on disk, even if truncated */

    char            path[512];
    char            title[160];
    evo_text_status status;
} evo_text_doc;

/**
 * Load `path`. Always leaves `doc` in a usable state - on failure the status
 * says why and the reader draws that rather than an empty page.
 */
evo_text_status evo_text_load(evo_text_doc *doc, const char *path);

/** Release everything. Safe on a zeroed or already-freed doc. */
void evo_text_free(evo_text_doc *doc);

/**
 * (Re)wrap to `wrap_px` at the current face, keeping the top of the view on
 * the same place in the document. Cheap enough to call when nothing changed:
 * it returns immediately if the width and face already match.
 *
 * `measure` is a caller-supplied per-face width estimate (main.c's
 * reader_measure since #81), so the module never has to know which program it
 * is linked into or how text is finally rendered.
 */
void evo_text_wrap(evo_text_doc *doc, int wrap_px,
                   int (*measure)(const char *s, int len, int face));

/** True when the document has no readable content. */
int evo_text_is_empty(const evo_text_doc *doc);

/* ---- scrolling ---------------------------------------------------------- */

/**
 * Move by `delta` display lines, clamped so the view can never scroll past
 * the end or above the start. `visible` is how many lines fit on screen.
 */
void evo_text_scroll(evo_text_doc *doc, int delta, int visible);

/** Jump to a fraction of the document, 0.0 to 1.0. */
void evo_text_seek_fraction(evo_text_doc *doc, double f, int visible);

/** 0.0 to 1.0 - how far through the document the view is. */
double evo_text_progress(const evo_text_doc *doc, int visible);

#ifdef __cplusplus
}
#endif

#endif /* EVO_TEXTREADER_H */
