#ifndef EVO_I_APPLICATION_HPP
#define EVO_I_APPLICATION_HPP

#include "evo/Common.hpp"
#include "evo/interfaces/IStatefulFeature.hpp"

namespace evo {

class ISettingsService;
class IMediaMetadataService;
class ICoverArtService;
class ISoundEffectEngine;
class ISurroundTestService;
class IFileSystemBrowser;
class IPlaybackController;
class ScreenManager;

/**
 * @brief Discrete lifecycle states for the overall EVO Player application.
 */
enum class ApplicationState : int {
    Uninitialized = 0,
    InitializingHardware = 1,
    InitializingServices = 2,
    InitializingScreens = 3,
    Running = 4,
    Exiting = 5,
    Terminated = 6
};

/**
 * @brief Discrete lifecycle events for the application engine.
 */
enum class ApplicationEvent : int {
    Boot = 0,
    HardwareReady = 1,
    ServicesReady = 2,
    ScreensReady = 3,
    StartLoop = 4,
    RequestExit = 5,
    ShutdownComplete = 6
};

/**
 * @brief Interface for EVO Player application runtime and service container.
 *
 * Inherits IStatefulFeature to enforce structured application lifecycle management.
 */
class IApplication : public IStatefulFeature {
public:
    virtual ~IApplication() = default;

    virtual bool initialize(int argc, char** argv) = 0;
    virtual int run() = 0;
    virtual void shutdown() = 0;
    virtual void requestExit() = 0;

    virtual ApplicationState getApplicationState() const = 0;

    virtual ISettingsService* getSettingsService() const = 0;
    virtual IMediaMetadataService* getMediaMetadataService() const = 0;
    virtual ICoverArtService* getCoverArtService() const = 0;
    virtual ISoundEffectEngine* getSoundEffectEngine() const = 0;
    virtual ISurroundTestService* getSurroundTestService() const = 0;
    virtual IFileSystemBrowser* getFileSystemBrowser() const = 0;
    virtual IPlaybackController* getPlaybackController() const = 0;
    virtual ScreenManager* getScreenManager() const = 0;
};

} // namespace evo

#endif // EVO_I_APPLICATION_HPP
