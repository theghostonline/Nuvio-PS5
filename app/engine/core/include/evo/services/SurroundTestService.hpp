#ifndef EVO_SURROUND_TEST_SERVICE_HPP
#define EVO_SURROUND_TEST_SERVICE_HPP

#include "evo/interfaces/ISurroundTestService.hpp"
#include "evo/audio/FieldMusic.hpp"
#include <pthread.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace evo {

class SurroundTestService : public ISurroundTestService {
public:
    SurroundTestService();
    ~SurroundTestService() override;

    bool start() override;
    void stop() override;
    void triggerTone(bool is51Layout, int channelIndex) override;
    bool isActive() const override { return m_active.load(); }
    int getCurrentChannel() const override { return m_currentChannel.load(); }
    bool is51Layout() const override { return m_is51Layout; }
    void set51Layout(bool is51) override { m_is51Layout = is51; }
    bool playPcm(const int16_t* interleaved8, size_t frames) override;
    bool isPcmPlaying() const override { return m_pcmPlaying.load(); }
    bool startField() override;
    void stopField() override;
    bool isFieldPlaying() const override { return m_fieldOn.load(); }
    void setFieldGains(const float gains[8], float master) override;

private:
    static void* AudioThreadEntry(void* arg);
    void AudioLoop();
    void emitTone(int handle, int channelMask, double frequencyHz, int blocks, int grain);
    void emitPcm(int handle);
    void emitFieldBlock(int handle);

    static constexpr int AudioGrain = 512;
    static constexpr int SampleRate = 48000;
    static constexpr int ChannelCount = 8;

    int m_audioHandle = -1;
    pthread_t m_thread = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_active{false};
    std::atomic<int> m_currentChannel{-1};
    std::atomic<int> m_pendingMask{0};
    bool m_is51Layout = false;

    std::mutex m_pcmLock;
    std::vector<int16_t> m_pcm;          /* pending one-shot PCM, 8ch interleaved */
    std::atomic<bool> m_pcmPending{false};
    std::atomic<bool> m_pcmPlaying{false};

    /* 3D SOUND FIELD - targets written by the UI thread, ramped to on the
     * audio thread (m_fieldCur) so gain changes never click */
    std::atomic<bool>  m_fieldOn{false};
    std::atomic<float> m_fieldTarget[ChannelCount];
    float m_fieldCur[ChannelCount] = {0.0f};
    spatial::FieldMusic m_music;
    bool  m_musicUsed = false;      /* audio thread only */
};

} // namespace evo

#endif // EVO_SURROUND_TEST_SERVICE_HPP
