#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // visibility tests: draws count passing pixels while simple[kMCountEnable / 4] is set, ClearReport zeroes the
    // count, GetReport writes it to a 16 byte report once the GPU is past those draws; without them it stays pending
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}

private:
    uint32_t fence_ = 0;
};

std::unique_ptr<Backend> CreateBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);
#ifdef __SWITCH__
// before the process ends: libnx closes the display and the GPU on exit while the render thread may still present,
// and deko3d ends the process on a failed call (the console's "closed because of an error"). Waits out the call under
// way; every later one returns at once
void StopRendering();
#endif

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause
// the render thread executing batches (present included), and the fps cap sleeping (app::Pace)
inline std::atomic<uint64_t> g_perf_render_ns{0}, g_perf_idle_ns{0};
inline std::atomic<uint64_t> g_perf_draws{0};  // draws the title made
// microseconds the title's thread and the render thread poll for each other before sleeping on a condition variable:
// the same work in the same order, with less wake-up latency on each hand-over (0: sleep at once, as before)
inline std::atomic<uint32_t> g_render_spin_us{0};
// STREET (performance): copy each draw's vertices when it is recorded and skip the per-draw wait for the render thread
// (draws too big to copy, or on memory that is not there, still wait). Off: the wait on every draw, as validated on
// Windows. The Switch reads draw_copy from its settings; elsewhere STREET_DRAW_COPY=1 turns it on.
inline std::atomic<bool> g_draw_copy{false};
inline std::atomic<uint64_t> g_perf_copied{0}, g_perf_copied_bytes{0};

// STREET (performance): a draw whose constants equal the previous draw's in the same upload ring reuses that upload
inline std::atomic<bool> g_reuse_constants{false};

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};
// F12 composed PNG screenshot; F3 diagnostic capture remains separate.
inline std::atomic<bool> g_screenshot{false};
// after each swap the new back buffer holds a copy of the frame just shown, as D3DSWAPEFFECT_COPY would: for titles
// that draw over the previous frame although they asked for flips (FIFA Street's loading bar). Set by GameInfo::init.
inline bool g_swap_keeps_back_buffer = false;

}  // namespace gpu
