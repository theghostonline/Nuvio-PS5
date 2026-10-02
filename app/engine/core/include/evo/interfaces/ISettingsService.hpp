#ifndef EVO_I_SETTINGS_SERVICE_HPP
#define EVO_I_SETTINGS_SERVICE_HPP

#include "evo/Common.hpp"
#include <string>

namespace evo {

/**
 * @brief Interface for application configuration and user preferences.
 * 
 * Provides abstraction for persistent settings serialization, theme synchronization,
 * and user-defined runtime preferences.
 */
class ISettingsService {
public:
    virtual ~ISettingsService() = default;

    virtual bool loadSettings() = 0;
    virtual bool saveSettings() = 0;

    virtual bool isResumePlaybackEnabled() const = 0;
    virtual void setResumePlaybackEnabled(bool enabled) = 0;

    virtual ViewMode getDefaultViewMode() const = 0;
    virtual void setDefaultViewMode(ViewMode mode) = 0;
    virtual const char* getViewModeName(ViewMode mode) const = 0;

    virtual bool isAutoSubtitlesEnabled() const = 0;
    virtual void setAutoSubtitlesEnabled(bool enabled) = 0;

    virtual bool isSortFoldersFirst() const = 0;
    virtual void setSortFoldersFirst(bool enabled) = 0;

    virtual std::string getThemeName() const = 0;
    virtual void setThemeName(const std::string& themeName) = 0;

    virtual bool isSoundFeedbackEnabled() const = 0;
    virtual void setSoundFeedbackEnabled(bool enabled) = 0;

    virtual bool isLightbarFeedbackEnabled() const = 0;
    virtual void setLightbarFeedbackEnabled(bool enabled) = 0;

    virtual int getSubtitleFontFace() const = 0;
    virtual void setSubtitleFontFace(int face) = 0;

    /* Live TV: open a channel on a picker of the streams behind it (the URL as
     * listed, each HLS quality variant, the extension guess) instead of letting
     * EVO choose and fall back on its own. */
    virtual bool isAskStreamEnabled() const = 0;
    virtual void setAskStreamEnabled(bool enabled) = 0;

    /* #110: where the second line of dialogue goes (0 stacked above the
     * primary, 1 top of the screen) and its colour (0 yellow, 1 cyan, 2 white). */
    virtual int getSecondarySubtitlePosition() const = 0;
    virtual void setSecondarySubtitlePosition(int position) = 0;
    virtual int getSecondarySubtitleColor() const = 0;
    virtual void setSecondarySubtitleColor(int color) = 0;

    virtual int getKeyboardType() const = 0;
    virtual void setKeyboardType(int type) = 0;

    virtual DecoderPreference getVideoDecoderPreference() const = 0;
    virtual void setVideoDecoderPreference(DecoderPreference preference) = 0;
    virtual const char* getDecoderPreferenceBadge(DecoderPreference preference) const = 0;

    virtual Upscaler getUpscaler() const = 0;
    virtual void setUpscaler(Upscaler upscaler) = 0;
    virtual const char* getUpscalerName(Upscaler upscaler) const = 0;

    virtual AiNetwork getAiNetwork() const = 0;
    virtual void setAiNetwork(AiNetwork network) = 0;
    virtual const char* getAiNetworkName(AiNetwork network) const = 0;

    virtual bool isDebugOverlayEnabled() const = 0;
    virtual void setDebugOverlayEnabled(bool enabled) = 0;

    virtual RefreshRateMode getRefreshRateMode() const = 0;
    virtual void setRefreshRateMode(RefreshRateMode mode) = 0;
    virtual const char* getRefreshRateModeName(RefreshRateMode mode) const = 0;

    virtual HdrOutputMode getHdrOutputMode() const = 0;
    virtual void setHdrOutputMode(HdrOutputMode mode) = 0;
    virtual const char* getHdrOutputModeName(HdrOutputMode mode) const = 0;

    virtual void syncThemeToRmlUi() = 0;

};

} // namespace evo

#endif // EVO_I_SETTINGS_SERVICE_HPP
