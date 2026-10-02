#ifndef EVO_SETTINGS_SERVICE_HPP
#define EVO_SETTINGS_SERVICE_HPP

#include "evo/interfaces/ISettingsService.hpp"
#include <string>

namespace evo {

class SettingsService : public ISettingsService {
public:
    SettingsService();
    ~SettingsService() override = default;

    bool loadSettings() override;
    bool saveSettings() override;

    bool isResumePlaybackEnabled() const override { return m_resumePlaybackEnabled; }
    void setResumePlaybackEnabled(bool enabled) override { m_resumePlaybackEnabled = enabled; }

    ViewMode getDefaultViewMode() const override { return m_defaultViewMode; }
    void setDefaultViewMode(ViewMode mode) override { m_defaultViewMode = mode; }
    const char* getViewModeName(ViewMode mode) const override;

    bool isAutoSubtitlesEnabled() const override { return m_autoSubtitlesEnabled; }
    void setAutoSubtitlesEnabled(bool enabled) override { m_autoSubtitlesEnabled = enabled; }

    bool isSortFoldersFirst() const override { return m_sortFoldersFirst; }
    void setSortFoldersFirst(bool enabled) override { m_sortFoldersFirst = enabled; }

    std::string getThemeName() const override { return m_themeName; }
    void setThemeName(const std::string& themeName) override;

    bool isSoundFeedbackEnabled() const override { return m_soundFeedbackEnabled; }
    void setSoundFeedbackEnabled(bool enabled) override;

    bool isLightbarFeedbackEnabled() const override { return m_lightbarFeedbackEnabled; }
    void setLightbarFeedbackEnabled(bool enabled) override;

    int getSubtitleFontFace() const override { return m_subtitleFontFace; }
    void setSubtitleFontFace(int face) override { m_subtitleFontFace = face; }

    bool isAskStreamEnabled() const override { return m_askStream; }
    void setAskStreamEnabled(bool enabled) override { m_askStream = enabled; }

    int getSecondarySubtitlePosition() const override { return m_secondarySubtitlePosition; }
    void setSecondarySubtitlePosition(int position) override;
    int getSecondarySubtitleColor() const override { return m_secondarySubtitleColor; }
    void setSecondarySubtitleColor(int color) override;

    int getKeyboardType() const override { return m_keyboardType; }
    void setKeyboardType(int type) override;

    DecoderPreference getVideoDecoderPreference() const override { return m_decoderPreference; }
    void setVideoDecoderPreference(DecoderPreference preference) override { m_decoderPreference = preference; }
    const char* getDecoderPreferenceBadge(DecoderPreference preference) const override;

    Upscaler getUpscaler() const override { return m_upscaler; }
    void setUpscaler(Upscaler upscaler) override { m_upscaler = upscaler; }
    const char* getUpscalerName(Upscaler upscaler) const override;

    AiNetwork getAiNetwork() const override { return m_aiNetwork; }
    void setAiNetwork(AiNetwork network) override { m_aiNetwork = network; }
    const char* getAiNetworkName(AiNetwork network) const override;

    bool isDebugOverlayEnabled() const override { return m_debugOverlayEnabled; }
    void setDebugOverlayEnabled(bool enabled) override { m_debugOverlayEnabled = enabled; }

    RefreshRateMode getRefreshRateMode() const override { return m_refreshRateMode; }
    void setRefreshRateMode(RefreshRateMode mode) override { m_refreshRateMode = mode; }
    const char* getRefreshRateModeName(RefreshRateMode mode) const override;

    HdrOutputMode getHdrOutputMode() const override { return m_hdrOutputMode; }
    void setHdrOutputMode(HdrOutputMode mode) override { m_hdrOutputMode = mode; }
    const char* getHdrOutputModeName(HdrOutputMode mode) const override;

    void syncThemeToRmlUi() override;

private:
    bool m_resumePlaybackEnabled = true;
    ViewMode m_defaultViewMode = ViewMode::Fit;
    bool m_autoSubtitlesEnabled = true;
    bool m_sortFoldersFirst = true;
    std::string m_themeName = "EVO Dark";
    bool m_soundFeedbackEnabled = true;
    bool m_lightbarFeedbackEnabled = true;
    int m_subtitleFontFace = 1; // Medium
    bool m_askStream = true;    // Live TV: pick the stream yourself
    int m_secondarySubtitlePosition = 0; // #110: stacked above the primary
    int m_secondarySubtitleColor = 0;    // #110: yellow
    /* EVO_KEYBOARD_TYPE_NATIVE (1) - the enum has VIRTUAL = 0, and this
     * used to be 0 under a "Native IME" comment, so a fresh install saved
     * (and kept) the virtual keyboard nobody picked. */
    int m_keyboardType = 1;
    DecoderPreference m_decoderPreference = DecoderPreference::Auto;
    // Off until the upscaler is hardware-verified (#103), then Sharp.
    Upscaler m_upscaler = Upscaler::Off;
    AiNetwork m_aiNetwork = AiNetwork::Auto;
    RefreshRateMode m_refreshRateMode = RefreshRateMode::Off;
    HdrOutputMode m_hdrOutputMode = HdrOutputMode::Auto;
    bool m_debugOverlayEnabled = false;
};


} // namespace evo

#endif // EVO_SETTINGS_SERVICE_HPP
