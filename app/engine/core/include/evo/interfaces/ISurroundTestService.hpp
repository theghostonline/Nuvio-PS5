#ifndef EVO_I_SURROUND_TEST_SERVICE_HPP
#define EVO_I_SURROUND_TEST_SERVICE_HPP

#include "evo/Common.hpp"

namespace evo {

/**
 * @brief Interface for multi-channel surround audio diagnostic tests.
 */
class ISurroundTestService {
public:
    virtual ~ISurroundTestService() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void triggerTone(bool is51Layout, int channelIndex) = 0;
    virtual bool isActive() const = 0;
    virtual int getCurrentChannel() const = 0;
    virtual bool is51Layout() const = 0;
    virtual void set51Layout(bool is51) = 0;

    /**
     * Play `frames` of interleaved S16 8-channel PCM (the port's own
     * FL FR FC LFE BL BR SL SR order) once, gap-free, preempting any tone.
     * Used by the Surround Studio's microphone calibration (#106), which needs
     * its whole chirp sequence on one sample-exact timeline. Opens the port if
     * needed; false if it can't.
     */
    virtual bool playPcm(const int16_t* interleaved8, size_t frames) = 0;
    /** True from playPcm() until the last block of that PCM was output. */
    virtual bool isPcmPlaying() const = 0;

    /**
     * 3D SOUND FIELD (#106): continuous music (evo/audio/FieldMusic.hpp)
     * panned across the speakers. `gains` is SpatialField's per-channel gain
     * matrix (LFE entry scales the sub-bass), `master` the overall level; the
     * audio thread ramps to new values over one block, so call it every frame.
     */
    virtual bool startField() = 0;
    virtual void stopField() = 0;
    virtual bool isFieldPlaying() const = 0;
    virtual void setFieldGains(const float gains[8], float master) = 0;
};

} // namespace evo

#endif // EVO_I_SURROUND_TEST_SERVICE_HPP
