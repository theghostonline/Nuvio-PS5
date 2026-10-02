/*
 * evo_provider.h — the provider seam (#90).
 *
 * A provider is a source of playable media that is not the USB stick: Emby,
 * Jellyfin, an IPTV playlist, a debrid link resolver. This header is the only
 * thing EVO's player knows about any of them.
 *
 * THE SPLIT THIS HEADER EXISTS TO ENFORCE
 *
 * The 0.6/0.7 Emby integration was wrong because EVO drew a bespoke screen for
 * it: a nine-row D-pad list that looked like EVO and nothing like Emby, and
 * that had to be rewritten by hand for every new source. So a provider is now
 * two halves with a hard line between them:
 *
 *   logic  - this vtable, compiled into the app module. Auth, catalog, search,
 *            link resolution, progress reporting. C, in-binary, trusted.
 *   look   - the provider's OWN .rml/.rcss bundle, fetched at runtime over
 *            HTTP and cached under /data/evoplayer/providers/<id>/. Markup and
 *            styling only; see evo_provider_bundle.h.
 *
 * The logic half publishes a data model. The downloaded markup binds to it by
 * name and never references an EVO element id. Arbitrary third-party *code*
 * therefore never reaches the console, while the presentation is genuinely the
 * service's own. EVO supplies playback, not presentation.
 *
 * ASYNC SHAPE
 *
 * Every call that touches the network returns immediately and completes
 * through a callback delivered on the main thread by evo_net_poll(), which
 * Application.cpp already pumps once per frame. Nothing in here may block the
 * 60 fps loop, and nothing in here may be called from the demux or decode
 * threads.
 *
 * The callback fires exactly once per accepted call. A negative return means
 * the call was NOT accepted and the callback will never fire, so the caller
 * still owns whatever it was going to free in there.
 */
#ifndef EVO_PROVIDER_H
#define EVO_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "evo_addon.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bumped when this header changes in a way a UI bundle can see: a new data
 * model field, a renamed one, a changed event contract. A bundle declares the
 * minimum it needs in its manifest and is refused if this is lower. Adding a
 * capability bit or a new optional vtable slot does not need a bump; removing
 * or renaming anything does.
 */
#define EVO_PROVIDER_API_VERSION 1

/* A provider id is a directory name under /data/evoplayer/providers/, so it is
 * deliberately narrow: lowercase ASCII, digits, '-' and '_'. Validated by
 * evo_provider_id_valid() rather than trusted. */
#define EVO_PROVIDER_MAX_ID       24

/*
 * URLs get 2048 to match evo_net's limit. A resolved Emby stream URL carries
 * an api_key, a debrid URL carries a signed token and an expiry, and an IPTV
 * playlist entry can be an arbitrarily deep CDN path - 512, which is what
 * evo_addon.h's stream_url still is, truncated all three into a valid-looking
 * request for the wrong resource.
 */
#define EVO_PROVIDER_MAX_URL      2048

/* Per-item text. Sized from what the five target services actually return,
 * not from round numbers: Emby titles run long, IPTV channel names do not. */
#define EVO_PROVIDER_MAX_ITEM_ID  160
#define EVO_PROVIDER_MAX_TITLE    192
#define EVO_PROVIDER_MAX_SUBTITLE 160
#define EVO_PROVIDER_MAX_OVERVIEW 320
#define EVO_PROVIDER_MAX_ART_URL  384

/* One page. 500 is the figure #90's hardware checklist names ("a 500+ channel
 * playlist - frame rate holds with a data-bound list"), so it is the page size
 * rather than an arbitrary cap; longer catalogs page. */
#define EVO_PROVIDER_PAGE_MAX     512

/* ------------------------------------------------------------------------- */
/* Capabilities                                                              */
/* ------------------------------------------------------------------------- */
/*
 * What a provider actually implements. This is what lets Torbox and
 * Real-Debrid sit behind the same vtable as Emby: they contribute no catalog
 * at all, only RESOLVE, and the registry's resolver chain hands them a catalog
 * item from somewhere else to turn into a playable URL.
 *
 * A vtable slot may be NULL exactly when its capability bit is clear. The
 * registry checks this at registration time so a mismatch is a startup error
 * rather than a null call later.
 */
typedef enum {
    EVO_PROVIDER_CAP_CATALOG  = 1u << 0,  /* list_catalog     */
    EVO_PROVIDER_CAP_SEARCH   = 1u << 1,  /* search           */
    EVO_PROVIDER_CAP_RESOLVE  = 1u << 2,  /* resolve          */
    EVO_PROVIDER_CAP_PROGRESS = 1u << 3,  /* report_progress  */
    EVO_PROVIDER_CAP_AUTH     = 1u << 4,  /* auth             */
    EVO_PROVIDER_CAP_UI       = 1u << 5,  /* ui_bundle_url    */
    /* The catalog contains live streams: no duration, not seekable. The OSD,
     * the resume store and the seek path all assume a seekable file, so this
     * has to be visible to the player and not inferred from a failed seek. */
    EVO_PROVIDER_CAP_LIVE     = 1u << 6,
    /* get_source / set_source: the provider can be pointed at a different
     * source at runtime, from a UI, and persists it itself. */
    EVO_PROVIDER_CAP_CONFIG   = 1u << 7,
    /* web_ui_url: the provider's UI is its own website, opened in the
     * system browser beside the rail (evo_webui.c, #101) instead of an RmlUi
     * bundle. The site's player is rerouted to EVO's. */
    EVO_PROVIDER_CAP_WEBUI    = 1u << 8
} evo_provider_caps_t;

/* ------------------------------------------------------------------------- */
/* Data model                                                                */
/* ------------------------------------------------------------------------- */

/*
 * One playable representation of an item.
 *
 * A single Emby movie is one choice. An IPTV channel with three bitrate
 * variants is three. A debrid magnet is one choice with needs_resolver set,
 * which the resolver chain then replaces with the real thing.
 */
typedef struct evo_stream_choice {
    char    url[EVO_PROVIDER_MAX_URL];
    char    label[64];          /* "1080p", "Direct", "Original" - shown as-is */
    char    container[16];      /* "mkv", "mp4", "ts", "m3u8" - hint, not truth */
    char    video_codec[16];
    char    audio_codec[16];
    int     width;
    int     height;
    int64_t bitrate_bps;        /* 0 if unknown */
    int64_t size_bytes;         /* 0 if unknown or live */

    /* No duration, not seekable. Set from EVO_PROVIDER_CAP_LIVE providers and
     * from any HLS playlist with no EXT-X-ENDLIST. */
    int     is_live;

    /*
     * This URL is not yet playable - it is a magnet, a hoster page or a
     * provider-internal handle that a RESOLVE-capable provider must turn into
     * an http(s) URL. evo_provider_resolve_chain() does that; nothing should
     * hand a choice with this set to the player.
     */
    int     needs_resolver;
} evo_stream_choice_t;

/*
 * One catalog row.
 *
 * `id` is opaque. It is whatever the provider needs to identify the item again
 * and EVO must never parse, split or build one. That is the difference from
 * BrowserEntry's fullPath, which is a filesystem path the browser happily
 * manipulates - a remote item has no path.
 */
typedef struct evo_provider_item {
    char    id[EVO_PROVIDER_MAX_ITEM_ID];
    char    parent_id[EVO_PROVIDER_MAX_ITEM_ID];
    char    title[EVO_PROVIDER_MAX_TITLE];
    char    subtitle[EVO_PROVIDER_MAX_SUBTITLE];  /* year, group-title, S01E02 */
    char    overview[EVO_PROVIDER_MAX_OVERVIEW];
    char    art_url[EVO_PROVIDER_MAX_ART_URL];    /* poster / channel logo */
    char    backdrop_url[EVO_PROVIDER_MAX_ART_URL];

    int64_t duration_sec;       /* 0 for a folder or a live stream */
    int64_t resume_pos_sec;     /* 0 if none */

    evo_media_kind_t kind;
    int     is_folder;
    int     is_live;

    /* Now-and-next, when the provider has an EPG. Empty otherwise. A full
     * XMLTV grid is explicitly out of #90's scope; two strings are not. */
    char    now_title[128];
    char    next_title[128];
} evo_provider_item_t;

/* ------------------------------------------------------------------------- */
/* Callbacks                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * All three are delivered on the main thread from evo_net_poll(). The arrays
 * they receive are owned by the caller of the callback and are valid only for
 * the duration of the call - copy what you need.
 *
 * `ok` is 0 on failure, and `msg`/`count` then describe why: a failed call
 * still fires its callback exactly once, so a UI waiting on one can always
 * leave its loading state.
 */
typedef void (*evo_provider_auth_cb)(int ok, const char *msg, void *ud);

/* `has_more` is 1 when a further page exists for the same parent_id/query. */
typedef void (*evo_provider_items_cb)(int ok,
                                      const evo_provider_item_t *items,
                                      int count,
                                      int has_more,
                                      void *ud);

typedef void (*evo_provider_resolve_cb)(int ok,
                                        const evo_stream_choice_t *choices,
                                        int count,
                                        void *ud);

/* Playback state handed to report_progress. */
typedef enum {
    EVO_PROVIDER_PLAY_START  = 0,
    EVO_PROVIDER_PLAY_UPDATE = 1,
    EVO_PROVIDER_PLAY_STOP   = 2
} evo_provider_play_state_t;

/* ------------------------------------------------------------------------- */
/* The vtable                                                                */
/* ------------------------------------------------------------------------- */

typedef struct evo_provider {
    /* Identity. All three are static strings owned by the provider's TU. */
    const char *id;             /* "iptv", "emby" - see EVO_PROVIDER_MAX_ID  */
    const char *name;           /* "IPTV", "Emby" - shown to the user        */
    const char *icon;           /* asset name under assets/icons/, or NULL   */

    uint32_t    caps;           /* evo_provider_caps_t bits                  */
    int         api_version;    /* EVO_PROVIDER_API_VERSION at compile time  */

    /*
     * init/shutdown are called once each by the registry. init() loads
     * persisted credentials and must not touch the network - it runs during
     * boot, before the self-unjail has necessarily opened /data, and a DNS
     * lookup there would stall the splash.
     */
    int  (*init)(void);
    void (*shutdown)(void);

    /* 1 when there are enough credentials to try. A provider that needs no
     * credentials (a public M3U URL that is already set) returns 1. */
    int  (*is_configured)(void);

    /* CAP_AUTH. Exchange stored credentials for a session. */
    int  (*auth)(evo_provider_auth_cb cb, void *ud);

    /*
     * CAP_CATALOG. `parent_id` NULL or "" means the root. `page` is 0-based.
     * A provider with no paging ignores `page` and reports has_more = 0.
     */
    int  (*list_catalog)(const char *parent_id, int page,
                         evo_provider_items_cb cb, void *ud);

    /* CAP_SEARCH. */
    int  (*search)(const char *query, int page,
                   evo_provider_items_cb cb, void *ud);

    /*
     * CAP_RESOLVE. Turn an item id into the playable choices for it, best
     * first. This is the slot that makes a debrid provider fit: it implements
     * only this, and the id it is handed came from someone else's catalog.
     */
    int  (*resolve)(const char *item_id,
                    evo_provider_resolve_cb cb, void *ud);

    /* CAP_PROGRESS. Fire-and-forget; no callback, never fails visibly. */
    void (*report_progress)(const char *item_id, int64_t pos_sec,
                            int64_t dur_sec, evo_provider_play_state_t state);

    /*
     * CAP_UI. Where this provider's UI bundle lives, or NULL to use the
     * embedded fallback skin. A static string, or a pointer into the
     * provider's own config buffer - read immediately, do not stash.
     */
    const char *(*ui_bundle_url)(void);

    /*
     * CAP_CONFIG. The provider's PRIMARY source string, as one line of text a
     * user can type: an M3U URL for IPTV, a server host for Emby.
     *
     * This exists so the generic provider screen can offer "point this at your
     * service" without knowing which provider it is hosting - the host must not
     * include a provider-specific header, or the seam is gone. It is
     * deliberately one string and not a config schema: anything richer (an
     * Xtream host/user/password triple, Emby credentials) belongs in that
     * provider's own setup screen, which is a separate story.
     *
     * set_source persists the value itself and invalidates whatever it had
     * cached, so the next list_catalog re-reads from the new source. Returns 0
     * on success.
     */
    const char *(*get_source)(void);
    int         (*set_source)(const char *value);

    /*
     * CAP_WEBUI. The site to open, as http[s]://<host>:<port> with no path, or
     * NULL / "" while it is not set up. Read immediately, do not stash.
     */
    const char *(*web_ui_url)(void);
    /* CAP_WEBUI. The page to open on that site ("/web/index.html" for a media
     * server, "/" for a single-page app); NULL means "/". */
    const char *web_ui_path;
    /* CAP_WEBUI. Which evo_webui.c hook profile catches this site's player:
     * NULL for the media-server one (Emby/Jellyfin stream URLs), "nuvio" for
     * Nuvio's #player / #videoPlayer. */
    const char *web_ui_hook;
} evo_provider_t;

/* ------------------------------------------------------------------------- */
/* Registry (evo_provider_mgr.c)                                             */
/* ------------------------------------------------------------------------- */

/*
 * Bring up the static table of compiled-in providers: validate each vtable
 * against its capability bits, call init(), and load the per-provider enable
 * flags from the store. Safe to call twice.
 *
 * Returns the number of providers registered, or negative if the table itself
 * is malformed (a duplicate id, an invalid id, a capability with a NULL slot)
 * - which is a programming error in this repo, not a runtime condition.
 */
int  evo_provider_mgr_init(void);
void evo_provider_mgr_shutdown(void);

/*
 * Re-read every provider's persisted config.
 *
 * The app module boots with /data closed and only reaches its real data root
 * once evo_jailbreak_self() lands, which can be several seconds in. Everything
 * loaded before that came from the transient /download0 fallback (#46), so the
 * providers have to be told to look again - the same reason recent_load() and
 * the settings service are re-run at that point.
 *
 * Does NOT re-run the vtable validation or re-create anything; it is init()
 * again plus a fresh read of the enable flags.
 */
void evo_provider_mgr_rebind(void);

int  evo_provider_count(void);
const evo_provider_t *evo_provider_at(int index);
const evo_provider_t *evo_provider_find(const char *id);

/* 1 when `id` is safe to use as a directory name. Everything that builds a
 * path from a provider id goes through this first. */
int  evo_provider_id_valid(const char *id);

/*
 * Per-provider enable, persisted in the store next to the credentials. A
 * provider that is compiled in but disabled is invisible to the UI and its
 * vtable is never called.
 */
int  evo_provider_is_enabled(const char *id);

/*
 * #101: parse a web-UI provider's source - "[http[s]://]<host>[:<port>][/]" -
 * into a host (a name or an IPv4 address), a port and *tls (1 for https).
 * With no port: default_port for http (a media server's own port), 443 for
 * https (the usual reverse-proxied deployment). Returns 0 on success.
 */
int  evo_provider_parse_web_source(const char *value, char *host, size_t host_cap,
                                   int *port, int *tls, int default_port);
void evo_provider_set_enabled(const char *id, int enabled);
int  evo_provider_save_state(void);

/*
 * The resolver chain.
 *
 * Resolve `item_id` from `provider_id`, then - if the best choice comes back
 * with needs_resolver set - hand that choice's url to each enabled
 * RESOLVE-capable provider in turn until one returns something playable. That
 * is how a Stremio-style magnet from one catalog becomes a Real-Debrid HTTP
 * URL, without either provider knowing the other exists.
 *
 * The callback fires once, with the final playable choices or ok = 0.
 */
int  evo_provider_resolve_chain(const char *provider_id,
                                const char *item_id,
                                evo_provider_resolve_cb cb,
                                void *ud);

/* ------------------------------------------------------------------------- */
/* Helpers shared by provider implementations                                */
/* ------------------------------------------------------------------------- */

/* Zero an item and set the fields that have a non-zero sane default. */
void evo_provider_item_clear(evo_provider_item_t *it);
void evo_provider_stream_choice_clear(evo_stream_choice_t *c);

/*
 * Percent-encode `in` into `out` for use inside a URL query value. Returns the
 * length written, or negative on overflow. Providers build query strings by
 * hand and a raw '&' or space in a search term or a channel name otherwise
 * produces a different request than the one intended.
 */
int  evo_provider_url_escape(const char *in, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* EVO_PROVIDER_H */
