// MVP's audio output (audio/audio.h) through libnx's audio renderer (audren): a thread pulls 10 ms blocks of the
// runtime's stereo float mix (xdk/dsound.cpp already folds 4 and 6 channel voices) and queues them as 16 bit wave
// buffers on one voice. Adapted from BLACK-NX-PORT's switch/audio.cpp, which takes a 5.1 mix instead.
#include <malloc.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

constexpr uint32_t kBlock = kRate / 100;  // frames a wave buffer holds: 10 ms
constexpr int kBuffers = 6;               // 60 ms queued at most

AudioDriver g_drv;
AudioDriverWaveBuf g_wave[kBuffers];
int16_t* g_pool = nullptr;  // kBuffers stereo blocks, the renderer's memory pool
std::atomic<bool> g_stop{false};
std::atomic<bool> g_running{false};

void ToPcm(const float* in, int16_t* out) {
    for (uint32_t i = 0; i < kBlock * 2; i++) out[i] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
}

void Run(std::function<void(float*, uint32_t)> render) {
    std::vector<float> mix(size_t(kBlock) * 2);
    while (!g_stop) {
        for (int i = 0; i < kBuffers; i++) {
            AudioDriverWaveBuf& w = g_wave[i];
            if (w.state != AudioDriverWaveBufState_Free && w.state != AudioDriverWaveBufState_Done) continue;
            int16_t* pcm = g_pool + size_t(i) * kBlock * 2;
            std::fill(mix.begin(), mix.end(), 0.0f);
            render(mix.data(), kBlock);
            ToPcm(mix.data(), pcm);
            armDCacheFlush(pcm, size_t(kBlock) * 2 * sizeof(int16_t));
            audrvVoiceAddWaveBuf(&g_drv, 0, &w);
        }
        if (!audrvVoiceIsPlaying(&g_drv, 0)) audrvVoiceStart(&g_drv, 0);
        audrvUpdate(&g_drv);
        audrenWaitFrame();
    }
    g_running = false;
}

}  // namespace

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    static const AudioRendererConfig cfg = {
        .output_rate = AudioRendererOutputRate_48kHz,
        .num_voices = 4,
        .num_effects = 0,
        .num_sinks = 1,
        .num_mix_objs = 1,
        .num_mix_buffers = 2,
    };
    Result rc = audrenInitialize(&cfg);
    if (R_SUCCEEDED(rc)) rc = audrvCreate(&g_drv, &cfg, 2);
    if (R_FAILED(rc)) {
        LOGE("audio: audio renderer unavailable (0x%X), no sound", rc);
        return;
    }
    size_t pool = (size_t(kBlock) * 2 * sizeof(int16_t) * kBuffers + AUDREN_MEMPOOL_ALIGNMENT - 1) &
                  ~size_t(AUDREN_MEMPOOL_ALIGNMENT - 1);
    g_pool = static_cast<int16_t*>(memalign(AUDREN_MEMPOOL_ALIGNMENT, pool));
    std::memset(g_pool, 0, pool);
    armDCacheFlush(g_pool, pool);
    int id = audrvMemPoolAdd(&g_drv, g_pool, pool);
    audrvMemPoolAttach(&g_drv, id);
    static const u8 kSink[2] = {0, 1};
    audrvDeviceSinkAdd(&g_drv, AUDREN_DEFAULT_DEVICE_NAME, 2, kSink);
    audrvUpdate(&g_drv);
    audrenStartAudioRenderer();
    audrvVoiceInit(&g_drv, 0, 2, PcmFormat_Int16, kRate);
    audrvVoiceSetDestinationMix(&g_drv, 0, AUDREN_FINAL_MIX_ID);
    audrvVoiceSetMixFactor(&g_drv, 0, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&g_drv, 0, 0.0f, 0, 1);
    audrvVoiceSetMixFactor(&g_drv, 0, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&g_drv, 0, 1.0f, 1, 1);
    for (int i = 0; i < kBuffers; i++) {
        g_wave[i] = {};
        g_wave[i].data_raw = g_pool;
        g_wave[i].size = pool;
        g_wave[i].start_sample_offset = int32_t(size_t(i) * kBlock);
        g_wave[i].end_sample_offset = int32_t(size_t(i + 1) * kBlock);
    }
    g_running = true;
    std::thread(Run, std::move(render)).detach();
    LOGI("audio: stereo output through the audio renderer, %u frame blocks", kBlock);
}

// bounded, as on Windows: a guest mixer lock may be held by a faulted thread
void StopOutput() {
    if (!g_running) return;
    g_stop = true;
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (g_running && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!g_running) audrvVoiceStop(&g_drv, 0);
}

}  // namespace audio
