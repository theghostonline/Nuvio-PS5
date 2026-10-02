/*
 * evo_changelog — what changed in each release, for the About section.
 */
#ifndef EVO_CHANGELOG_H
#define EVO_CHANGELOG_H

typedef enum {
    EVO_CL_NEW = 0,
    EVO_CL_FIXED,
    EVO_CL_IMPROVED,
    EVO_CL_REMOVED,
    EVO_CL_VERSION
} evo_changelog_kind;

typedef struct evo_changelog_item {
    evo_changelog_kind kind;
    const char        *text;
} evo_changelog_item;

typedef struct evo_changelog_release {
    const char               *version;      /* "0.7.0" */
    const char               *tagline;      /* "EMBY PASSWORD AUTH & REFINED UI" */
    const char               *date;         /* "AUGUST 2026" */
    const evo_changelog_item *items;
    int                       item_count;
} evo_changelog_release;

/*
 * Release 0.10.0
 *
 * Five weeks and 315 commits: the payload became a real app, the picture moved
 * onto the console's own decoder and the interface onto its GPU. Trimmed to 12
 * items on purpose - EVO_RMLUI_CL_ITEMS is 12 because #changelog-detail is
 * 714dp of fixed, overflow:hidden height and a .clitem costs 46dp. Anything
 * past that is clipped rather than scrolled, so the list is curated to what
 * fits instead of relying on the "+ N MORE" tail.
 */
static const evo_changelog_item EVO_CL_0100[] = {
    { EVO_CL_NEW,      "EVO IS A REAL PS5 APP - LAUNCH IT FROM THE GAMES ROW" },
    { EVO_CL_NEW,      "HARDWARE VIDEO DECODE - 4K H.264, HEVC AND VP9" },
    { EVO_CL_NEW,      "10-BIT HDR WITH HDR10 AND HLG TONE MAPPING" },
    { EVO_CL_NEW,      "THE WHOLE INTERFACE RENDERS ON THE GPU" },
    { EVO_CL_NEW,      "AN INTERFACE REBUILT FROM SCRATCH - EVERY SCREEN" },
    { EVO_CL_NEW,      "COLOUR THEMES - MIDNIGHT, CARBON, EMBER, AURORA, USB" },
    { EVO_CL_NEW,      "STORAGE BROWSER REBUILT WITH A THUMBNAIL ON EVERY CARD" },
    { EVO_CL_NEW,      "SCREENSHOT CAPTURE, AND A QUIT THAT CLOSES CLEANLY" },
    { EVO_CL_FIXED,    "THE SCRUB HEAD STARTS WHERE THE PICTURE IS" },
    { EVO_CL_FIXED,    "CHOOSING A THEME ACTUALLY CHANGES THE COLOURS" },
    { EVO_CL_FIXED,    "SEEKING KEEPS THE AUDIO IN STEP WITH THE PICTURE" },
    { EVO_CL_REMOVED,  "EMBY IS OFF WHILE IT IS REWORKED - IT RETURNS SOON" }
};

/* Release 0.7.0 */
static const evo_changelog_item EVO_CL_070[] = {
    { EVO_CL_NEW,      "EMBY PASSWORD AUTHENTICATION WITH SECURE INPUT & PERSISTENCE" },
    { EVO_CL_NEW,      "NATIVE OPENSSL HTTPS AND TLS CLIENT INTEGRATION" },
    { EVO_CL_NEW,      "REDESIGNED CHANGELOG VIEWER WITH MASTER-DETAIL HIERARCHY" },
    { EVO_CL_NEW,      "DIRECT MEMORY BUFFER MANAGER FOR STREAMING I/O PERFORMANCE" },
    { EVO_CL_NEW,      "NATIVE PS5 IME DIALOG INTEGRATION WITH MULTI-LANGUAGE SUPPORT" },
    { EVO_CL_NEW,      "INTERACTIVE MEDIA DIRECTORY SEARCH IN FILE BROWSER" },
    { EVO_CL_FIXED,    "CUSTOM AVIO LIFECYCLE STABILITY FOR NETWORK STREAMS" },
    { EVO_CL_IMPROVED, "PARALLEL CPU AVX2 SIMD YUV PIPELINE OPTIMIZATIONS" }
};

/* Release 0.6.0 */
static const evo_changelog_item EVO_CL_060[] = {
    { EVO_CL_NEW,      "EMBY MEDIA SERVER ADDON - BROWSE AND STREAM DIRECTLY" },
    { EVO_CL_NEW,      "ON-SCREEN VIRTUAL KEYBOARD FOR SERVER AND CREDENTIAL SETUP" },
    { EVO_CL_NEW,      "SURROUND SOUND STUDIO - 360-DEG TOP-DOWN THEATER" },
    { EVO_CL_NEW,      "8-CHANNEL HARDWARE CALIBRATION FOR 5.1 AND 7.1" },
    { EVO_CL_NEW,      "AUTO TEST CYCLES AND 360-DEG ROTATION SWEEP" },
    { EVO_CL_NEW,      "DYNAMIC 5.1 SPEAKER HIDING AND 2D SPATIAL DPAD NAVIGATION" },
    { EVO_CL_NEW,      "ORGANIZED SETTINGS HIERARCHY WITH EXPANDED SECTIONS" },
    { EVO_CL_NEW,      "CONFIGURABLE DEFAULT SUBTITLE SIZING PREFERENCE" },
    { EVO_CL_FIXED,    "PS5 S16_8CH GAIN INITIALIZATION AND CLICK-FREE FADES" }
};

/* Release 0.5.0 */
static const evo_changelog_item EVO_CL_050[] = {
    { EVO_CL_NEW,      "TEXT READER - OPEN TXT LOG MD NFO AND SUBTITLES" },
    { EVO_CL_NEW,      "SCROLL WITH DPAD - SHOULDERS PAGE - TRIANGLE RESIZES" },
    { EVO_CL_NEW,      "THE FONT HAS PUNCTUATION AT LAST" },
    { EVO_CL_FIXED,    "THE RELEASE NOW SHIPS THE FILE THAT ACTUALLY RUNS" },
    { EVO_CL_FIXED,    "THE MEDIA TILE REPORTS THE RIGHT VERSION" }
};

/* Release 0.4.0 */
static const evo_changelog_item EVO_CL_040[] = {
    { EVO_CL_NEW,      "CIRCLE ASKS BEFORE IT STOPS PLAYBACK" },
    { EVO_CL_NEW,      "COVER ART IS LARGER - TILES READ AS POSTERS" },
    { EVO_CL_NEW,      "SOUND AND LIGHTBAR SURVIVE A RELAUNCH" },
    { EVO_CL_FIXED,    "SMOOTHER 4K - LESS WORK PER FRAME CONVERTED" },
    { EVO_CL_FIXED,    "LESS STUTTER - NO THREAD CHURN EVERY FRAME" },
    { EVO_CL_FIXED,    "TILE ART IS CROPPED - NOT SQUASHED" },
    { EVO_CL_FIXED,    "THE RAIL NO LONGER OPENS OVER A PANEL" }
};

/* Release 0.3.0 */
static const evo_changelog_item EVO_CL_030[] = {
    { EVO_CL_NEW,      "HOME SCREEN TILE - OPEN FROM MEDIA - NO BROWSER" },
    { EVO_CL_NEW,      "THIS CHANGELOG - UNDER ABOUT" },
    { EVO_CL_NEW,      "A REAL APPLICATION ICON - DRAWN FROM VECTORS" },
    { EVO_CL_FIXED,    "THE TILE KEEPS ITS OWN COPY OF THE PLAYER" }
};

/* Release 0.2.0 */
static const evo_changelog_item EVO_CL_020[] = {
    { EVO_CL_NEW,      "SUBTITLE TRACK PICKER - PRESS DOWN WHILE PLAYING" },
    { EVO_CL_NEW,      "TRACK NAMES FROM LANGUAGE CODES - 50 MAPPED" },
    { EVO_CL_FIXED,    "TRACKS RANKED BY CUE COUNT - NOT BY METADATA" },
    { EVO_CL_FIXED,    "NEAR EMPTY TRACKS MARKED SIGNS ONLY - STILL OFFERED" },
    { EVO_CL_FIXED,    "MARQUEE SCROLLS AT ONE SPEED AT ANY FRAME RATE" }
};

/* Release 0.1.0 */
static const evo_changelog_item EVO_CL_010[] = {
    { EVO_CL_NEW,      "LAUNCH SCREEN - RESUME HERO AND TWO SHELVES" },
    { EVO_CL_NEW,      "BROWSER INSPECTOR - PREVIEW FRAME AND CODEC DETAIL" },
    { EVO_CL_NEW,      "SIDE NAVIGATION RAIL - BACK IS A STACK NOW" },
    { EVO_CL_NEW,      "HOLD TO SCROLL - SHOULDERS PAGE - L2/R2 JUMP A-Z" },
    { EVO_CL_NEW,      "L3 CAPTURES A SCREENSHOT DURING PLAYBACK" },
    { EVO_CL_FIXED,    "THE EIGHTH SETTINGS ROW WAS UNREACHABLE" },
    { EVO_CL_FIXED,    "AUDIO FAILURES NO LONGER ALL BLAMED ON E-AC3" },
    { EVO_CL_FIXED,    "PREVIEWS NO LONGER PIXELATED BY DOUBLE RESAMPLING" },
    { EVO_CL_FIXED,    "4K CONVERTER KEEPS ITS WORKERS - 11.6 TO 9.2 MS" },
    { EVO_CL_REMOVED,  "HAPTICS - EVERY VIBRATION PATH IS DEAD IN THIS SLOT" }
};

/* Release 0.0.2 */
static const evo_changelog_item EVO_CL_002[] = {
    { EVO_CL_NEW,      "THEME FILES FROM USB - FOUR THEMES BUILT IN" },
    { EVO_CL_NEW,      "CARDS DRAWN FROM SDF - GENERATED VECTOR ICONS" },
    { EVO_CL_NEW,      "NAVIGATION SOUNDS AND A THEMED LIGHTBAR" }
};

/* Release 0.0.1 */
static const evo_changelog_item EVO_CL_001[] = {
    { EVO_CL_NEW,      "7.1 SURROUND OUTPUT WITH STEREO FALLBACK" },
    { EVO_CL_NEW,      "FLIP SYNCHRONISED PRESENTATION - NO TEARING" },
    { EVO_CL_NEW,      "FASTER TILE SWIZZLE AND FOLDERS FIRST BROWSING" }
};

static const evo_changelog_release EVO_CHANGELOG_RELEASES[] = {
    { "0.10.0", "THE PS5 APP, GPU & HARDWARE DECODE", "SEPTEMBER 2026", EVO_CL_0100, sizeof(EVO_CL_0100)/sizeof(EVO_CL_0100[0]) },
    { "0.7.0", "EMBY AUTH & SYSTEM ENHANCEMENTS", "AUGUST 2026", EVO_CL_070, sizeof(EVO_CL_070)/sizeof(EVO_CL_070[0]) },
    { "0.6.0", "EMBY ADDON & SURROUND SOUND STUDIO", "AUGUST 2026", EVO_CL_060, sizeof(EVO_CL_060)/sizeof(EVO_CL_060[0]) },
    { "0.5.0", "A TEXT READER & FIXED INSTALL", "AUGUST 2026", EVO_CL_050, sizeof(EVO_CL_050)/sizeof(EVO_CL_050[0]) },
    { "0.4.0", "SMOOTHER PLAYBACK & SAFER STOP", "JULY 2026", EVO_CL_040, sizeof(EVO_CL_040)/sizeof(EVO_CL_040[0]) },
    { "0.3.0", "LAUNCH FROM THE CONSOLE", "JULY 2026", EVO_CL_030, sizeof(EVO_CL_030)/sizeof(EVO_CL_030[0]) },
    { "0.2.0", "SUBTITLES & TRACK PICKER", "JULY 2026", EVO_CL_020, sizeof(EVO_CL_020)/sizeof(EVO_CL_020[0]) },
    { "0.1.0", "A REBUILD OF THE INTERFACE", "JULY 2026", EVO_CL_010, sizeof(EVO_CL_010)/sizeof(EVO_CL_010[0]) },
    { "0.0.2", "THEMES & SDF VECTOR ICONS", "JULY 2026", EVO_CL_002, sizeof(EVO_CL_002)/sizeof(EVO_CL_002[0]) },
    { "0.0.1", "THE INITIAL FORK", "JULY 2026", EVO_CL_001, sizeof(EVO_CL_001)/sizeof(EVO_CL_001[0]) },
};

#define EVO_CHANGELOG_RELEASE_COUNT \
    ((int)(sizeof(EVO_CHANGELOG_RELEASES) / sizeof(EVO_CHANGELOG_RELEASES[0])))

#endif /* EVO_CHANGELOG_H */
