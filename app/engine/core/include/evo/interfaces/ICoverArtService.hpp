#ifndef EVO_I_COVER_ART_SERVICE_HPP
#define EVO_I_COVER_ART_SERVICE_HPP

#include "evo/Common.hpp"
#include <string>
#include <cstdint>

namespace evo {

/**
 * @brief Interface for artwork discovery, thumbnail frame decoding, and cache management.
 */
class ICoverArtService {
public:
    static constexpr int PosterWidth = 320;
    static constexpr int PosterHeight = 180;
    static constexpr int HeroWidth = 960;
    static constexpr int HeroHeight = 540;
    static constexpr int PreviewWidth = 560;
    static constexpr int PreviewHeight = 315;
    static constexpr int CacheCapacity = 64;

    virtual ~ICoverArtService() = default;

    virtual std::string resolveSidecarPath(const std::string& mediaPath, bool isDirectory) = 0;
    virtual const uint32_t* peekCoverArt(const std::string& mediaPath) const = 0;
    virtual bool hasTriedCoverArt(const std::string& mediaPath) const = 0;
    virtual const uint32_t* getCoverArt(const std::string& mediaPath, bool isDirectory) = 0;
    
    virtual void ensureHeroArt(const std::string& mediaPath) = 0;
    virtual const uint32_t* getHeroArtPixels() const = 0;
    virtual bool isHeroArtValid() const = 0;
    /* Which media the single hero buffer currently holds - callers that follow
     * a moving cursor need to know whether the buffer is still theirs. */
    virtual const std::string& getHeroArtPath() const = 0;

    virtual void ensureBrowserPreview(const std::string& mediaPath, bool isDirectory) = 0;
    virtual const uint32_t* getBrowserPreviewPixels() const = 0;
    virtual bool isBrowserPreviewValid() const = 0;

    virtual bool extractVideoFrame(const std::string& videoPath, uint32_t* outPixels, int targetWidth, int targetHeight) = 0;
    virtual void boxFilterScaleRgba(const uint8_t* sourceRgba, int srcWidth, int srcHeight,
                                    uint32_t* destBgra, int destWidth, int destHeight) = 0;

    virtual void clearCache() = 0;
};

} // namespace evo

#endif // EVO_I_COVER_ART_SERVICE_HPP
