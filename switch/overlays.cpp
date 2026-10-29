// The console's overlays for a homebrew program: Status Monitor Overlay and its forks (Horizon OC Monitor) show the FPS
// and the resolution from blocks in SaltyNX's shared page, and ReverseNX-RT's overlay switches handheld/docked through
// another. SaltyNX's own plugins write those blocks inside commercial games but never load into a homebrew program, so
// this host writes them itself: it asks SaltyNX for the page once (port "SaltySD": command 7 the page's handle, 6
// reserves room, 0 ends the session), keeps it mapped and writes the blocks like ordinary memory.
// Written for this port from the protocol as SaltyNX defines it (NX-FPS-Common.hpp, ReverseNX.cpp, service.c) and as
// NFSMW-NX documents it (docs/console-overlays.md); none of their code is copied. Without SaltyNX this does nothing.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "host.h"

namespace {

// SaltyNX's NxFpsSharedBlock up to the fields the overlays read (174 bytes); newer SaltyNX appends statistics, so the
// block is reserved with room for them
struct ResolutionCalls {
    uint16_t width, height, calls;
} __attribute__((packed));

struct FpsBlock {
    uint32_t magic;  // 0x465053
    uint8_t fps;
    float fps_avg;
    bool plugin_active;  // the overlay clears it and expects it back within 100 ms
    uint8_t fps_locked, fps_mode, zero_sync, patch_applied;
    uint8_t api;               // 1 NVN, 2 EGL, 3 Vulkan
    uint32_t fps_ticks[10];    // system ticks between the last ten frames
    uint8_t buffers, set_buffers, active_buffers, set_active_buffers;
    uint8_t display_sync;
    ResolutionCalls render_calls[8];
    ResolutionCalls viewport_calls[8];  // calls 0xFFFF is the overlay's question: never written
    bool force_original_refresh_rate, dont_force_60_in_docked, force_suspend;
    uint8_t current_refresh_rate;
    float read_speed_per_second;
    uint8_t fps_locked_docked;
    uint64_t frame_number;
    int8_t expected_set_buffers;
} __attribute__((packed));
static_assert(sizeof(FpsBlock) == 174);

// ReverseNX-RT's Shared block
struct ReverseNxBlock {
    uint32_t magic;  // "NXRT"
    bool is_docked;      // the mode picked in its overlay
    bool def;            // "Controlled by system": the real mode rules
    bool plugin_active;  // the game asked for the mode
    uint8_t res;
    bool was_ddr_used;
} __attribute__((packed));
static_assert(sizeof(ReverseNxBlock) == 9);

constexpr uint32_t kFpsMagic = 0x465053;
constexpr uint32_t kNxrtMagic = 0x5452584E;  // "NXRT"
constexpr size_t kPage = 0x1000;
constexpr uint64_t kFpsReserve = 0x200;  // the block and what newer SaltyNX keeps after it
constexpr uint64_t kNxrtReserve = 12;
constexpr uint8_t kApiVulkan = 3;

std::mutex g_lock;
bool g_enabled = true;
SharedMemory g_shm{};
uint8_t* g_page = nullptr;
volatile FpsBlock* g_fps = nullptr;
volatile ReverseNxBlock* g_nxrt = nullptr;
bool g_logged_failure = false;
uint64_t g_last_tick = 0;
uint32_t g_tick_at = 0;
std::atomic<uint16_t> g_calls{0};

template <typename T>
volatile T* Find(uint32_t magic) {
    if (!g_page) return nullptr;
    for (size_t off = 0; off + sizeof(T) <= kPage; off += 4) {
        uint32_t m;
        std::memcpy(&m, g_page + off, 4);
        if (m == magic) return reinterpret_cast<volatile T*>(g_page + off);
    }
    return nullptr;
}

// the process may have a single session, already sm's: room for one more (resource limit, SVC 0x7E, only when allowed)
void RaiseSessionLimit() {
    u64 handle = 0;
    if (R_FAILED(svcGetInfo(&handle, InfoType_ResourceLimit, INVALID_HANDLE, 0)) || !handle) return;
    Handle rl = Handle(handle);
    s64 used = 0, limit = 0;
    svcGetResourceLimitCurrentValue(&used, rl, LimitableResource_Sessions);
    svcGetResourceLimitLimitValue(&limit, rl, LimitableResource_Sessions);
    if (used + 1 > limit && envIsSyscallHinted(0x7E)) {
        Result rc = svcSetResourceLimitLimitValue(rl, LimitableResource_Sessions, limit + 2);
        LOGR("overlays: sessions %lld/%lld, limit raised: 0x%X", (long long)used, (long long)limit, rc);
    }
    svcCloseHandle(rl);
}

// connects to SaltySD, maps the page, finds or reserves both blocks and ends the session (the port takes one session
// at a time: the overlays need it too). first: the main thread before the game runs, which may release sm for a moment
bool Connect(bool first) {
    Handle h = INVALID_HANDLE;
    Result rc = svcConnectToNamedPort(&h, "SaltySD");
    if (rc == 0x10801) {  // out of sessions
        RaiseSessionLimit();
        rc = svcConnectToNamedPort(&h, "SaltySD");
        if (rc == 0x10801 && first) {
            // nothing else talks to sm before the game starts: give its session back for the few milliseconds this takes
            smExit();
            rc = svcConnectToNamedPort(&h, "SaltySD");
            Result back = smInitialize();
            if (R_FAILED(back)) LOGE("overlays: sm could not be opened again (0x%X)", back);
        }
    }
    if (R_FAILED(rc)) {
        if (!g_logged_failure) {
            LOGR("overlays: no SaltyNX (port SaltySD: 0x%X)%s", rc, rc == 0xF201 ? ", not installed" : "");
            g_logged_failure = true;
        }
        return false;
    }
    Service s;
    serviceCreate(&s, h);
    Handle shm = INVALID_HANDLE;
    if (!g_page) {
        rc = serviceDispatch(&s, 7, .in_send_pid = true, .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
                             .out_handles = &shm);
        if (R_SUCCEEDED(rc)) {
            shmemLoadRemote(&g_shm, shm, kPage, Perm_Rw);
            rc = shmemMap(&g_shm);
            if (R_SUCCEEDED(rc)) g_page = static_cast<uint8_t*>(shmemGetAddr(&g_shm));
        }
        if (!g_page) LOGE("overlays: SaltyNX's page could not be mapped (0x%X)", rc);
    }
    // a block left from an earlier run is reused: the page is only handed out from the start when SaltyNX loads into a
    // game, so reserving on every run would use it up
    auto reserve = [&](uint64_t size) -> uint8_t* {
        uint64_t off = 0;
        Result r = serviceDispatchInOut(&s, 6, size, off, .in_send_pid = true);
        if (R_FAILED(r) || off + size > kPage) {
            LOGE("overlays: no room in SaltyNX's page (0x%X)", r);
            return nullptr;
        }
        return g_page + off;
    };
    if (g_page && !(g_fps = Find<FpsBlock>(kFpsMagic))) {
        if (uint8_t* p = reserve(kFpsReserve)) {
            std::memset(p, 0, kFpsReserve);
            auto* b = reinterpret_cast<volatile FpsBlock*>(p);
            b->api = kApiVulkan;
            for (int i = 0; i < 10; i++) b->fps_ticks[i] = uint32_t(armGetSystemTickFreq() / 30);  // no zero average
            b->magic = kFpsMagic;
            g_fps = b;
        }
    }
    if (g_page && !(g_nxrt = Find<ReverseNxBlock>(kNxrtMagic))) {
        if (uint8_t* p = reserve(kNxrtReserve)) {
            std::memset(p, 0, kNxrtReserve);
            auto* b = reinterpret_cast<volatile ReverseNxBlock*>(p);
            b->def = true;  // the console's mode until the player picks one
            b->magic = kNxrtMagic;
            g_nxrt = b;
        }
    }
    serviceDispatch(&s, 0, .in_send_pid = true);
    serviceClose(&s);
    LOGR("overlays: SaltyNX page %s, FPS block %s, ReverseNX-RT block %s", g_page ? "mapped" : "missing",
         g_fps ? "ready" : "missing", g_nxrt ? "ready" : "missing");
    return g_fps != nullptr;
}

}  // namespace

namespace host {

void OverlaysEnable(bool on) { g_enabled = on; }

void OverlaysInit() {
    if (!g_enabled) return;
    std::lock_guard lock(g_lock);
    Connect(true);
}

// each present: the overlay's "alive?" answer, the frame interval and the resolution, a few stores into mapped memory
void OverlaysFrame(uint32_t width, uint32_t height) {
    volatile FpsBlock* b = g_fps;
    if (!b) return;
    uint64_t now = armGetSystemTick();
    if (g_last_tick) b->fps_ticks[g_tick_at++ % 10] = uint32_t(std::min<uint64_t>(now - g_last_tick, 0xFFFFFFFFu));
    g_last_tick = now;
    uint16_t calls = std::min<uint16_t>(g_calls.load(std::memory_order_relaxed), 0xFFFE);
    b->render_calls[0].width = uint16_t(width);
    b->render_calls[0].height = uint16_t(height);
    b->render_calls[0].calls = calls;
    b->viewport_calls[0].width = uint16_t(width);
    b->viewport_calls[0].height = uint16_t(height);
    b->viewport_calls[0].calls = calls;
    b->frame_number = b->frame_number + 1;
    b->api = kApiVulkan;
    b->plugin_active = true;
}

// once a second: the counts, and the blocks found again if the page was rewritten (or SaltyNX came up late)
void OverlaysSecond(double fps, double fps_avg) {
    if (!g_enabled) return;
    std::lock_guard lock(g_lock);
    g_calls = uint16_t(std::clamp(fps, 0.0, 255.0));
    bool lost = g_fps && g_fps->magic != kFpsMagic;
    if (!g_fps || lost) {
        static uint32_t tries = 0;
        if (lost || tries++ % 5 == 0) {  // a missing SaltyNX is asked again every 5 s
            g_fps = nullptr;
            g_nxrt = nullptr;
            Connect(false);
        }
        if (!g_fps) return;
    }
    g_fps->fps = uint8_t(std::clamp(fps + 0.5, 0.0, 255.0));
    g_fps->fps_avg = float(fps_avg);
}

// the mode to obey: ReverseNX-RT's when the player picked one in its overlay, else the console's
bool OverlaysDocked(bool real) {
    volatile ReverseNxBlock* b = g_nxrt;
    if (!b || b->magic != kNxrtMagic) return real;
    b->plugin_active = true;
    if (b->def) {
        b->is_docked = real;  // the overlay shows the real mode while the system rules
        return real;
    }
    return b->is_docked;
}

}  // namespace host
