#pragma once
/*
 * The Nuvio app's stand-in for EVO Player's Application singleton.
 *
 * The vendored PlaybackController asks EVO's app for its settings, chapter
 * metadata and file browser. The Nuvio Player has none of those: every call
 * site already handles a missing service (default decoder choice, 30 s chapter
 * jumps, no folder auto-advance), so this answers "none" for each.
 */
#include "evo/interfaces/IFileSystemBrowser.hpp"
#include "evo/interfaces/IPlaybackController.hpp"
#include "evo/interfaces/IMediaMetadataService.hpp"
#include "evo/interfaces/ISettingsService.hpp"

namespace evo {

class Application {
public:
    static Application& getInstance() {
        static Application app;
        return app;
    }
    ISettingsService* getSettingsService() const { return nullptr; }
    IMediaMetadataService* getMediaMetadataService() const { return nullptr; }
    IFileSystemBrowser* getFileSystemBrowser() const { return nullptr; }

    /* The Nuvio Player's controller (player.cpp), for engine helpers that
     * restart playback, e.g. on a subtitle track switch. */
    IPlaybackController* getPlaybackController() const { return m_playback; }
    void setPlaybackController(IPlaybackController* pb) { m_playback = pb; }

private:
    IPlaybackController* m_playback = nullptr;
};

} // namespace evo
