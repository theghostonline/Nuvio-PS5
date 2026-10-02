#ifndef EVO_SPEAKER_CALIBRATION_SERVICE_HPP
#define EVO_SPEAKER_CALIBRATION_SERVICE_HPP

/*
 * SpeakerCalibrationService - AUTO CALIBRATION (MIC) in the Surround Sound
 * Studio (#106). Measures every speaker through the DualSense microphone at the
 * listening position and turns the result into per-channel level + delay trims
 * (media/include/evo_speaker_cal.h applies them to playback).
 *
 * How a run works, on one worker thread:
 *   1. Open the user-routed AudioIn (sceAudioInOpen, 16 kHz S16 mono) - the
 *      DualSense microphone unless the user picked another input device.
 *   2. MIC CHECK: 0.8 s of room noise. All-zero PCM = controller muted/off.
 *   3. MEASURING: one continuous 8-channel PCM stream goes out through the
 *      SurroundTestService port - a log sweep per speaker, each in its own
 *      1 s slot, and FRONT LEFT again at the end. Because every chirp sits at
 *      an exact sample offset in ONE output stream and is found in ONE capture
 *      stream, the relative arrival times need no clock timestamps at all; the
 *      repeated FRONT LEFT measures (and removes) the output/mic clock drift.
 *   4. ANALYZING: matched filter per slot -> first strong peak = direct-path
 *      arrival (sub-sample, parabolic), peak height = received level.
 *
 * What it can and cannot know: the output + Bluetooth mic latency is an
 * unknown constant, so distances are RELATIVE (path difference to the nearest
 * speaker) - which is exactly what delay compensation needs. Levels compare
 * the speakers against each other at the seat; the LFE sweep is a different
 * band, and the DualSense mic's low-frequency response is not flat, so treat
 * the subwoofer trim as approximate.
 */

#include "evo/interfaces/ISurroundTestService.hpp"
#include "evo_speaker_cal.h"

#include <pthread.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace evo {

class SpeakerCalibrationService {
public:
    enum class Phase { Idle = 0, MicCheck, Measuring, Analyzing, Complete, Error };

    struct ChannelResult {
        bool  measured = false;   /* part of this layout's run          */
        bool  detected = false;   /* chirp found above the noise floor  */
        float trimDb   = 0.0f;    /* level correction to apply           */
        float levelDb  = 0.0f;    /* received level, dB re full scale    */
        float pathM    = 0.0f;    /* extra path vs the nearest speaker   */
        float delayMs  = 0.0f;    /* compensation delay to apply         */
        float snrDb    = 0.0f;
    };

    struct Snapshot {
        Phase phase = Phase::Idle;
        bool  is51 = false;
        int   step = 0;           /* 1-based speaker being measured       */
        int   total = 0;
        int   channel = -1;       /* AudioOut channel being measured      */
        bool  verifying = false;  /* the closing FRONT LEFT drift check   */
        float noiseDb = -120.0f;  /* room noise floor, dBFS               */
        float micLevel = 0.0f;    /* live input level 0..1                */
        float driftPpm = 0.0f;
        ChannelResult results[EVO_SPEAKER_CAL_CHANNELS];
        char  message[128] = {0};
    };

    SpeakerCalibrationService() = default;
    ~SpeakerCalibrationService();

    /* Starts a run. The output port is borrowed for the run's duration. */
    bool start(ISurroundTestService* output, bool is51);
    void cancel();
    bool isRunning() const { return m_running.load(); }
    Snapshot snapshot() const;
    /* The finished run as a playback profile. false unless Phase::Complete. */
    bool buildProfile(evo_speaker_cal_t* out) const;

private:
    static void* ThreadEntry(void* arg);
    void run();
    void fail(const char* msg);
    void setPhase(Phase p, const char* msg);
    void analyze(size_t seqStart);

    static constexpr int kCaptureRate = 16000;
    static constexpr int kCaptureGrain = 256;
    static constexpr int kOutputRate = 48000;
    static constexpr float kSlotSec = 1.0f;

    ISurroundTestService* m_output = nullptr;
    bool m_is51 = false;
    int  m_order[EVO_SPEAKER_CAL_CHANNELS] = {0};
    int  m_orderCount = 0;

    pthread_t m_thread = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    std::vector<int16_t> m_capture;      /* worker thread only */
    size_t m_noiseBegin = 0, m_noiseEnd = 0;

    mutable std::mutex m_lock;
    Snapshot m_snap;
};

} // namespace evo

#endif // EVO_SPEAKER_CALIBRATION_SERVICE_HPP
