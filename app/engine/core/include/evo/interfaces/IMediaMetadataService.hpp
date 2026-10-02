#ifndef EVO_I_MEDIA_METADATA_SERVICE_HPP
#define EVO_I_MEDIA_METADATA_SERVICE_HPP

#include "evo/Common.hpp"
#include <string>
#include <vector>

struct AVFormatContext;

namespace evo {

struct ChapterInfo {
    double startTimeSeconds = 0.0;
    std::string title;
};

struct MediaMetadataInfo {
    std::string filePath;
    std::string title;
    std::string container;
    std::string videoCodec;
    std::string audioCodec;
    int width = 0;
    int height = 0;
    double durationSeconds = 0.0;
    int64_t fileSizeBytes = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    bool hasSubtitles = false;
};

/**
 * @brief Interface for inspecting media containers, streams, chapters and metadata.
 */
class IMediaMetadataService {
public:
    virtual ~IMediaMetadataService() = default;

    virtual MediaMetadataInfo extractBasicMetadata(const std::string& filePath) = 0;
    virtual MediaMetadataInfo extractMetadataFromFormat(AVFormatContext* formatContext, const std::string& filePath) = 0;

    virtual void loadChapters(AVFormatContext* formatContext) = 0;
    virtual void clearChapters() = 0;
    virtual size_t getChapterCount() const = 0;
    virtual const ChapterInfo* getChapter(size_t index) const = 0;
    virtual int getChapterIndexAtPosition(double positionSeconds) const = 0;

    virtual void cleanMediaTitle(const std::string& filePath, std::string& outTitle, std::string& outCategory) = 0;
    virtual std::string formatDuration(double durationSeconds) = 0;
    virtual std::string formatFileSize(int64_t bytes) = 0;
    virtual std::string formatRemainingTime(double currentPosition, double duration) = 0;
    virtual int calculateProgressPermille(double currentPosition, double duration) = 0;

    virtual bool exportCompatibilityReport(const std::string& outputPath,
                                           const MediaMetadataInfo& metadata,
                                           double playbackPosition,
                                           int recentCount,
                                           int favoriteCount,
                                           bool isPlaying) = 0;
};

} // namespace evo

#endif // EVO_I_MEDIA_METADATA_SERVICE_HPP
