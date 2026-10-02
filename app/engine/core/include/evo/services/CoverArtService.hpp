#ifndef EVO_COVER_ART_SERVICE_HPP
#define EVO_COVER_ART_SERVICE_HPP

#include "evo/interfaces/ICoverArtService.hpp"
#include <string>
#include <vector>

namespace evo {

class CoverArtService : public ICoverArtService {
public:
    CoverArtService();
    ~CoverArtService() override = default;

    std::string resolveSidecarPath(const std::string& mediaPath, bool isDirectory) override;
    const uint32_t* peekCoverArt(const std::string& mediaPath) const override;
    bool hasTriedCoverArt(const std::string& mediaPath) const override;
    const uint32_t* getCoverArt(const std::string& mediaPath, bool isDirectory) override;

    void ensureHeroArt(const std::string& mediaPath) override;
    const uint32_t* getHeroArtPixels() const override { return m_heroArtValid ? m_heroArtPixels.data() : nullptr; }
    bool isHeroArtValid() const override { return m_heroArtValid; }
    const std::string& getHeroArtPath() const override { return m_heroArtPath; }

    void ensureBrowserPreview(const std::string& mediaPath, bool isDirectory) override;
    const uint32_t* getBrowserPreviewPixels() const override;
    bool isBrowserPreviewValid() const override { return m_browserPreviewValid; }

    bool extractVideoFrame(const std::string& videoPath, uint32_t* outPixels, int targetWidth, int targetHeight) override;
    void boxFilterScaleRgba(const uint8_t* sourceRgba, int srcWidth, int srcHeight,
                            uint32_t* destBgra, int destWidth, int destHeight) override;

    void clearCache() override;

private:
    struct CacheEntry {
        std::string pathKey;
        std::vector<uint32_t> pixels;
        bool valid = false;
        bool tried = false;
    };

    CacheEntry* findOrAllocateSlot(const std::string& key);
    const CacheEntry* findSlot(const std::string& key) const;
    bool isVideoFile(const std::string& path) const;

    std::vector<CacheEntry> m_cache;
    int m_accessClock = 0;

    std::vector<uint32_t> m_heroArtPixels;
    std::string m_heroArtPath;
    bool m_heroArtValid = false;

    std::vector<uint32_t> m_browserPreviewPixels;
    std::string m_browserPreviewPath;
    bool m_browserPreviewValid = false;
    bool m_browserPreviewFailed = false;
};

} // namespace evo

#endif // EVO_COVER_ART_SERVICE_HPP
