#ifndef EVO_I_PLAYBACK_CONTROLLER_HPP
#define EVO_I_PLAYBACK_CONTROLLER_HPP

#include "evo/Common.hpp"
#include "evo/interfaces/IStatefulFeature.hpp"
#include <string>
#include <vector>

namespace evo {

/**
 * @brief Discrete playback engine states.
 */
enum class PlaybackState : int {
    Stopped = 0,
    Opening = 1,
    Playing = 2,
    Paused = 3,
    Scrubbing = 4,
    Seeking = 5,
    Finished = 6,
    Error = 7
};

/**
 * @brief Discrete playback trigger events.
 */
enum class PlaybackEvent : int {
    Open = 0,
    Play = 1,
    Pause = 2,
    Resume = 3,
    StartScrub = 4,
    MoveScrub = 5,
    ConfirmScrub = 6,
    CancelScrub = 7,
    Seek = 8,
    Finish = 9,
    Stop = 10,
    Fail = 11
};

/**
 * @brief Where the thing being played came from, and what to call it.
 *
 * #90. A bare path string was enough while every source was a file on the USB
 * stick: the filename was the identity, the title and the thing FFmpeg opened
 * all at once. A provider item is none of those - its URL carries an auth
 * token, its title is metadata the service returned, and its identity is an
 * opaque id that outlives the URL.
 *
 * Conflating them is the root cause of #9 (the OSD showing a URL instead of a
 * title) and of a token ending up in Recent, where it is both a leak and
 * useless: a signed URL that has expired is not a thing you can replay.
 */
struct PlaybackSource {
    /** What avformat actually opens. May carry a token; never displayed, and
     *  never persisted. */
    std::string url;
    /** What the OSD shows. For a local file this is left empty and the title
     *  is derived from the filename as before. */
    std::string title;
    /** Provider id, or empty for a local file. */
    std::string provider;
    /** Opaque provider item id, or empty for a local file. */
    std::string item_id;
    /** No duration, not seekable: the OSD, the resume store and the seek path
     *  all assume otherwise, so this has to be known rather than discovered
     *  by a seek that fails. */
    bool is_live = false;

    bool isProvider() const { return !provider.empty(); }
};

/**
 * @brief Interface for playback session management, A/V synchronization, scrubbing, and transport controls.
 *
 * Implements IStatefulFeature to guarantee state integrity across media transitions.
 */
class IPlaybackController : public IStatefulFeature {
public:
    virtual ~IPlaybackController() = default;

    virtual bool startPlayback(const std::string& filePath, double resumeOffset = 0.0) = 0;
    /** #90: the general form. startPlayback() is this with a local-file
     *  source, and every caller that has more than a path should use this. */
    virtual bool startPlaybackSource(const PlaybackSource& source,
                                     double resumeOffset = 0.0) = 0;
    virtual void stopPlayback() = 0;

    /** The title to put on screen: the provider's, when there is one, else
     *  empty so the caller falls back to deriving it from the filename. */
    virtual const std::string& getDisplayTitle() const = 0;
    virtual bool isLiveSource() const = 0;

    virtual void togglePause() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual bool isPaused() const = 0;
    virtual bool isActive() const = 0;
    virtual bool isMusicMode() const = 0;

    virtual PlaybackState getPlaybackState() const = 0;

    virtual double getPositionSeconds() const = 0;
    virtual double getDurationSeconds() const = 0;
    virtual double getPercentage() const = 0;
    virtual const std::string& getCurrentFilePath() const = 0;

    virtual void seekTo(double targetSeconds) = 0;
    virtual void beginScrub() = 0;
    virtual void moveScrub(double deltaSeconds) = 0;
    virtual bool confirmScrub() = 0;
    virtual void cancelScrub() = 0;
    virtual bool isScrubbing() const = 0;
    virtual double getScrubTargetSeconds() const = 0;
    virtual void updateScrubHold(uint32_t heldButtons) = 0;
    virtual void tickScrubAutoCommit() = 0;

    virtual void jumpChapter(int direction) = 0;

    virtual void cycleViewMode() = 0;
    virtual ViewMode getViewMode() const = 0;
    virtual void setViewMode(ViewMode mode) = 0;

    virtual void saveResumePosition() = 0;
    virtual double loadResumePosition(const std::string& filePath) const = 0;

    virtual bool playNextVideo() = 0;
    virtual bool replay() = 0;

    /* --- audio tracks ------------------------------------------------- */
    struct AudioTrackInfo {
        int         streamIndex = -1;
        std::string title;       /* stream metadata title, if any */
        std::string language;    /* "eng", "jpn", ... or "UND" */
        std::string codecName;
        int         channels = 0;
        int         sampleRate = 0;
    };

    /* Every audio stream in the open file that has a usable decoder. */
    virtual std::vector<AudioTrackInfo> getAudioTracks() const = 0;
    virtual int  getActiveAudioStream() const = 0;
    /*
     * Switch the live audio stream. Re-opens the file at the current position
     * with that stream selected - there is no way to swap the decoder under a
     * running session, which is also why this is a picker and not a cycle.
     */
    virtual bool switchAudioTrack(int streamIndex) = 0;

    /* --- video quality ------------------------------------------------ */
    /*
     * An HLS master exposes every quality variant as its own video stream, and
     * EVO opens the best by default. These list the ones the open stream has and
     * let the user pin another. A file with a single video stream has one entry
     * and nothing to choose.
     */
    struct VideoVariantInfo {
        int         streamIndex = -1;
        int         width = 0;
        int         height = 0;
        int64_t     bitrate = 0;      /* bits/s, 0 if the stream does not say */
        double      fps = 0.0;        /* 0 if unknown */
        std::string codecName;
    };

    /* Every video stream with a usable size, best first. */
    virtual std::vector<VideoVariantInfo> getVideoVariants() const = 0;
    virtual int  getActiveVideoStream() const = 0;
    /* True when the user has pinned a variant (false: EVO picks the best). */
    virtual bool isVideoQualityPinned() const = 0;
    /*
     * Pin variant `streamIndex`, or pass -1 to go back to EVO's own choice (the
     * best). Re-opens the stream at the current position, as switchAudioTrack
     * does, and keeps the title, provider identity and live flag.
     */
    virtual bool switchVideoVariant(int streamIndex) = 0;
};

} // namespace evo

#endif // EVO_I_PLAYBACK_CONTROLLER_HPP
