#ifndef EVO_PLAYBACK_CONTROLLER_HPP
#define EVO_PLAYBACK_CONTROLLER_HPP

#include "evo/interfaces/IPlaybackController.hpp"
#include "evo/fsm/StateMachine.hpp"
#include <string>
#include <vector>
#include <cstdint>

/* Opaque, at global scope: evo_stream_io.h is a C header this one does not
 * pull in, and declaring the struct inside namespace evo would make a
 * different type with the same spelling. */
struct evo_stream_io_ctx;

namespace evo {

class PlaybackController : public IPlaybackController {
public:
    PlaybackController();
    ~PlaybackController() override;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_playbackFsm; }
    const IStateMachine* getStateMachine() const override { return &m_playbackFsm; }

    PlaybackState getPlaybackState() const override {
        return m_playbackFsm.getCurrentState();
    }

    bool startPlayback(const std::string& filePath, double resumeOffset = 0.0) override;
    bool startPlaybackSource(const PlaybackSource& source,
                             double resumeOffset = 0.0) override;
    const std::string& getDisplayTitle() const override { return m_source.title; }
    bool isLiveSource() const override { return m_source.is_live; }
    void stopPlayback() override;

    void togglePause() override;
    void setPaused(bool paused) override;
    bool isPaused() const override;
    bool isActive() const override;
    bool isMusicMode() const override { return m_musicMode; }

    double getPositionSeconds() const override;
    double getDurationSeconds() const override { return m_durationSeconds; }
    double getPercentage() const override;
    const std::string& getCurrentFilePath() const override { return m_currentFilePath; }

    void seekTo(double targetSeconds) override;
    void beginScrub() override;
    void moveScrub(double deltaSeconds) override;
    bool confirmScrub() override;
    void cancelScrub() override;
    bool isScrubbing() const override;
    double getScrubTargetSeconds() const override { return m_scrubTargetSeconds; }
    void updateScrubHold(uint32_t heldButtons) override;
    void tickScrubAutoCommit() override;

    void jumpChapter(int direction) override;

    void cycleViewMode() override;
    ViewMode getViewMode() const override { return m_viewMode; }
    void setViewMode(ViewMode mode) override;

    void saveResumePosition() override;
    double loadResumePosition(const std::string& filePath) const override;

    bool playNextVideo() override;
    bool replay() override;

    std::vector<AudioTrackInfo> getAudioTracks() const override;
    int  getActiveAudioStream() const override;
    bool switchAudioTrack(int streamIndex) override;

    std::vector<VideoVariantInfo> getVideoVariants() const override;
    int  getActiveVideoStream() const override;
    bool isVideoQualityPinned() const override;
    bool switchVideoVariant(int streamIndex) override;

private:
    void initStateMachine();
    void applyViewMode();
    double clampScrubTarget(double target) const;
    void resetScrubHold();

    StateMachine<PlaybackState, PlaybackEvent> m_playbackFsm;

    std::string m_currentFilePath;
    /* #90: who this came from and what to call it. For a local file only
     * `url` is set and it equals m_currentFilePath. */
    PlaybackSource m_source;
    /* The stream-io context for the open format, so reconnect/timeout live in
     * one place for local and network alike (evo_stream_io.c). */
    ::evo_stream_io_ctx *m_streamIo = nullptr;
    /* Wall-clock of the last report_progress, so a provider gets an update
     * roughly every ten seconds rather than on every resume save. */
    double m_lastProgressReport = 0.0;
    double m_durationSeconds = 0.0;
    double m_resumeBaseOffset = 0.0;
    bool m_musicMode = false;
    ViewMode m_viewMode = ViewMode::Fit;

    // Scrub state
    double m_scrubTargetSeconds = 0.0;
    int m_scrubHoldDirection = 0;
    uint64_t m_scrubHoldStartMs = 0;
    uint64_t m_scrubHoldLastStepMs = 0;
    uint64_t m_scrubAutoCommitDeadlineMs = 0;

    /* Stream the next open() should pick, or -1 for "first decodable". Set
     * only across a switchAudioTrack() reopen and cleared by startPlayback. */
    int m_requestedAudioStream = -1;

    /* The video quality the user pinned in the Quality & Audio picker, and the
     * URL it was pinned for. Unlike m_requestedAudioStream this outlives one
     * open: a reopen for an audio switch keeps the quality. It is dropped when a
     * different source starts, because stream numbers mean nothing across
     * sources. -1 means EVO picks the best. */
    int m_pinnedVideoStream = -1;
    std::string m_pinnedVideoFor;
};

} // namespace evo

#endif // EVO_PLAYBACK_CONTROLLER_HPP
