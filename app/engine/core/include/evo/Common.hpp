#ifndef EVO_COMMON_HPP
#define EVO_COMMON_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace evo {

/*
 * The size EVO actually renders at. Resolved once at startup from
 * sceVideoOutGetResolutionStatus (see evo_agc_runtime_init) and fixed for the
 * session - not a constant, because a 1440p panel should be driven at 1440p
 * rather than upscaled from 1080p.
 *
 * This is NOT the UI's unit system. Stylesheets stay authored against the
 * EVO_UI_DESIGN_W x _H canvas in dp (ui/include/evo_metrics.h); the RmlUi
 * contexts get a density-independent pixel ratio of DisplayWidth /
 * EVO_UI_DESIGN_W, so layout and glyph rasterisation happen at the real
 * pixel size while the source geometry never changes.
 */
extern int DisplayWidth;
extern int DisplayHeight;

// Screen Identifiers (compatible with legacy UI router)
enum class ScreenId : int {
    MainMenu = 0,
    UsbBrowser = 1,
    Player = 2,
    ResumePrompt = 3,
    Settings = 10,
    ProfileSelect = 11,
    RecentFiles = 12,
    Favorites = 13,
    AboutSupport = 14,
    DeveloperTools = 15,
    MediaInfo = 16,
    PlaybackFinished = 17,
    SubtitlePicker = 18,
    Changelog = 19,
    ExitConfirm = 20,
    TextReader = 21,
    EmbySetup = 22,
    EmbyBrowse = 23,
    SettingsPlayback = 24,
    SettingsSubtitles = 25,
    SettingsInterface = 26,
    SettingsSystem = 27,
    SurroundTest = 28,
    ThemeSelect = 29,
    ImageViewer = 30,
    AudioTrackPicker = 31,
    SafeToClose = 32     /* the frame EVO parks on after QUIT (SafeToCloseScreen) */
};

// Playback Quality Profile
enum class PlaybackProfile : int {
    Balanced = 0,
    Performance = 1,
    Compatibility = 2,
    Debug = 3
};

// Video Scaling / Aspect Ratio View Mode
enum class ViewMode : int {
    Fit = 0,
    Fill = 1,
    Stretch = 2
};

// Video Decoder Backend Preference
enum class DecoderPreference : int {
    Auto = 0,
    NativeHardware = 1,
    FFmpegSoftware = 2
};

// Video upscaler for sources smaller than the panel (#103). Values match
// EVO_AGC_UPSCALE_* in evo_agc_runtime.h.
enum class Upscaler : int {
    Off = 0,
    Sharp = 1,   // FSR1 EASU + RCAS
    AI = 2       // Anime4K CNN x2 in shaders (larger network on PS5 Pro)
};

// Which network AI upscaling runs (#103). Values match EVO_AGC_UPNET_*.
// Auto follows PS5 Pro detection; the others override it, because the probe
// cannot identify every Pro. Large falls back to Standard on its own.
enum class AiNetwork : int {
    Auto = 0,
    Standard = 1,   // Anime4K S
    Large = 2,      // Anime4K M
    Maximum = 3     // Anime4K UL - sized for a PS5 Pro
};

// Refresh rate output mode (60 Hz vs 120 Hz)
enum class RefreshRateMode : int {
    Off = 0,
    Always = 1,
    PlaybackOnly = 2
};

// HDR10 output: Auto switches the TV into HDR10 while an HDR10 / HLG video is
// on screen (and back to SDR otherwise); Off keeps SDR and tone-maps HDR.
enum class HdrOutputMode : int {
    Auto = 0,
    Off = 1
};


// File Classification for Storage Browser
enum class FileCategory : int {
    Unknown = 0,
    Video,
    Audio,
    Image,
    Document,
    DiscImage,
    Homebrew,
    Folder
};

// Synthesized UI Sound Effects
enum class SoundEffect : int {
    None = 0,
    Move = 1,
    Confirm = 2,
    Back = 3,
    Toggle = 4
};

// Controller Button Masks (Standard SCE Layout)
namespace PadButtons {
    constexpr uint32_t Options  = 0x0008;
    constexpr uint32_t Up       = 0x0010;
    constexpr uint32_t Right    = 0x0020;
    constexpr uint32_t Down     = 0x0040;
    constexpr uint32_t Left     = 0x0080;
    constexpr uint32_t L2       = 0x0100;
    constexpr uint32_t R2       = 0x0200;
    constexpr uint32_t L1       = 0x0400;
    constexpr uint32_t R1       = 0x0800;
    constexpr uint32_t Triangle = 0x1000;
    constexpr uint32_t Circle   = 0x2000;
    constexpr uint32_t Cross    = 0x4000;
    constexpr uint32_t Square   = 0x8000;
    constexpr uint32_t L3            = 0x0002;
    constexpr uint32_t R3            = 0x0004;
    constexpr uint32_t TouchPad      = 0x00100000; // Physical touchpad click
    constexpr uint32_t TouchPadLeft  = 0x00200000; // Touchpad click/swipe on left half
    constexpr uint32_t TouchPadRight = 0x00400000; // Touchpad click/swipe on right half
}

// Color packing helper for 0xAABBGGRR (BGRA in little-endian uint32_t)
inline constexpr uint32_t MakeColorBgra(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8)  |
           (static_cast<uint32_t>(r));
}

} // namespace evo

#endif // EVO_COMMON_HPP
