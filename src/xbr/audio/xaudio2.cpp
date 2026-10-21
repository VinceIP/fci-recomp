// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

// FCI: silence while a notice holds the game (fci_manual.c); the mixer is not run, so nothing advances
std::atomic<bool> g_held{false};

namespace {
// FCI: a critical error (the output device removed or changed) stops the engine for good; the output is made
// again on the new default device
struct EngineWatch final : IXAudio2EngineCallback {
    std::atomic<bool> failed{false};
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT) override { failed = true; }
};

struct Output {
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;

    bool Open(EngineWatch& watch) {
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        watch.failed = false;
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xa->RegisterForCallbacks(&watch)) ||
            FAILED(xa->CreateMasteringVoice(&master)) || FAILED(xa->CreateSourceVoice(&voice, &wf)) ||
            FAILED(voice->Start())) {
            Close();
            return false;
        }
        return true;
    }
    void Close() {
        if (voice) voice->DestroyVoice();
        if (master) master->DestroyVoice();
        if (xa) xa->Release();
        voice = nullptr;
        master = nullptr;
        xa = nullptr;
    }
};
}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        EngineWatch watch;
        Output out;
        bool warned = false;
        constexpr uint32_t kFrames = kRate / 100, kSlots = 4;
        std::vector<float> bufs(kSlots * kFrames * 2);
        ULONGLONG retry = 0;
        for (uint32_t n = 0;;) {
            if (out.voice && watch.failed) {
                LOGW("audio: output device lost, reopening");
                out.Close();
            }
            if (!out.voice && GetTickCount64() >= retry) {
                if (!out.Open(watch)) {
                    if (!warned) LOGE("audio: XAudio2 unavailable, no sound (retrying)");
                    warned = true;
                    retry = GetTickCount64() + 1000;
                }
            }
            float* b = &bufs[(n % kSlots) * kFrames * 2];
            if (!out.voice) {  // no device: the mixer still runs at real time, its output dropped
                render(b, kFrames);
                Sleep(10);
                continue;
            }
            XAUDIO2_VOICE_STATE st;
            out.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued >= 3) {
                Sleep(2);
                continue;
            }
            if (g_held)
                std::fill(b, b + kFrames * 2, 0.0f);
            else
                render(b, kFrames);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 8;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            if (FAILED(out.voice->SubmitSourceBuffer(&xb))) {
                out.Close();
                continue;
            }
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio

extern "C" void fci_audio_hold(int on) { audio::g_held = on != 0; }
