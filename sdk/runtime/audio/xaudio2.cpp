// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <thread>
#include <chrono>
#include <vector>
#include <atomic>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {
namespace {
std::atomic<bool> stopping{false};
std::atomic<bool> started{false};
HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

void StopOutput() {
    stopping = true;
    if (started && finished && WaitForSingleObject(finished, 1500) == WAIT_TIMEOUT)
        LOGR("audio: cleanup timed out (mixer may be blocked); process exit will reclaim resources");
}

void StartOutput(std::function<void(float*, uint32_t)> render) {
    if (stopping || started.exchange(true)) return;
    std::thread([render] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        // Runs before the submitted sample storage goes out of scope.
        auto cleanup = [&] {
            if (voice) { voice->Stop(); voice->DestroyVoice(); voice = nullptr; }
            if (xa) xa->StopEngine();
            if (master) { master->DestroyVoice(); master = nullptr; }
            if (xa) { xa->Release(); xa = nullptr; }
        };
        auto finish = [&] {
            cleanup();
            if (SUCCEEDED(com)) CoUninitialize();
            LOGR("audio: output thread stopped, voices and engine released");
            if (finished) SetEvent(finished);
        };
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        HRESULT hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
        const char* stage = "XAudio2Create";
        if (SUCCEEDED(hr)) { stage = "CreateMasteringVoice"; hr = xa->CreateMasteringVoice(&master); }
        if (SUCCEEDED(hr)) { stage = "CreateSourceVoice"; hr = xa->CreateSourceVoice(&voice, &wf); }
        if (SUCCEEDED(hr)) { stage = "SourceVoice::Start"; hr = voice->Start(); }
        if (FAILED(hr)) {
            LOGE("audio: %s failed HRESULT=%08lX; no sound", stage, static_cast<unsigned long>(hr));
            cleanup();
            // Guest playback clocks and completion callbacks must keep advancing
            // even when the host has no usable audio endpoint.
            LOGR("audio: timed silent output active");
            constexpr uint32_t frames = kRate / 100;
            std::vector<float> discard(frames * 2);
            auto deadline = std::chrono::steady_clock::now();
            while (!stopping) {
                deadline += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(deadline);
                if (stopping) break;
                render(discard.data(), frames);
                // Do not burst through guest audio after a suspended host.
                auto now = std::chrono::steady_clock::now();
                if (now - deadline > std::chrono::milliseconds(100)) deadline = now;
            }
            finish();
            return;
        }
        LOGR("audio: stereo output started successfully");
        constexpr uint32_t kFrames = kRate / 100, kSlots = 4;
        std::vector<float> bufs(kSlots * kFrames * 2);
        for (uint32_t n = 0; !stopping;) {
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued >= 3) {
                Sleep(2);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * 2];
            render(b, kFrames);
            if (stopping) break;
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 8;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            hr = voice->SubmitSourceBuffer(&xb);
            if (FAILED(hr)) {
                LOGE("audio: SubmitSourceBuffer failed HRESULT=%08lX", static_cast<unsigned long>(hr));
                break;
            }
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
        finish();
    }).detach();
}

}  // namespace audio
