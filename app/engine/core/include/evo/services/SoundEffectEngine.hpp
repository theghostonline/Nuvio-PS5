#ifndef EVO_SOUND_EFFECT_ENGINE_HPP
#define EVO_SOUND_EFFECT_ENGINE_HPP

#include "evo/interfaces/ISoundEffectEngine.hpp"
#include <pthread.h>
#include <atomic>

namespace evo {

class SoundEffectEngine : public ISoundEffectEngine {
public:
    SoundEffectEngine();
    ~SoundEffectEngine() override;

    bool initialize() override;
    void shutdown() override;
    void playSound(SoundEffect effect) override;
    void setEnabled(bool enabled) override { m_enabled = enabled; }
    bool isEnabled() const override { return m_enabled; }

private:
    static void* WorkerThreadEntry(void* arg);
    void WorkerLoop();
    void renderTone(int handle, double startFreq, double endFreq, int durationMs, double gain);

    static constexpr int AudioGrain = 256;
    static constexpr int SampleRate = 48000;

    int m_audioHandle = -1;
    pthread_t m_thread = 0;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_pendingEffect{0};
    bool m_enabled = true;
};

} // namespace evo

#endif // EVO_SOUND_EFFECT_ENGINE_HPP
