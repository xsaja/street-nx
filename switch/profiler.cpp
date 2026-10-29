// Sampling profiler (profile = true in the settings), after the one in R-drg's defjam-sw (src/switch_nx.c, MIT).
// The console has no profiler and the emulator's costs are not the console's. A thread above the game's wakes every
// millisecond, pauses each busy registered thread (svcSetThreadActivity), reads its pc, lr and up to five return
// addresses of the x29 chain (svcGetThreadContext3) and lets it run again. "Busy" is decided every 100 ms from the
// threads' tick counts: over 10 % of a core. Samples go to logs/prof.bin every 10 s as 32 byte records (thread slot,
// time, pc, lr, returns, as offsets into the NRO); the log names the slots. tools/prof_report.py turns them into
// tables per function with the ELF: nothing is resolved on the console. A few percent of one core while on.
#include <switch.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "app/app.h"
#include "core/log.h"
#include "host.h"

// the NRO's load address: _start is the first instruction, ELF address 0
extern "C" void _start(void);

namespace {

struct Sample {
    uint16_t slot, time;  // time in 10 ms units since the profiler started (wraps after 655 s)
    uint32_t pc, lr, ret[5];
};
static_assert(sizeof(Sample) == 32);

constexpr int kMaxThreads = 48;
constexpr unsigned kMaxSamples = 60000;  // 10 s at 1 kHz of up to six busy threads

std::mutex g_lock;
Handle g_handles[kMaxThreads];
char g_names[kMaxThreads][24];
int g_count = 0;

Sample g_samples[kMaxSamples];
unsigned g_n = 0;
uint64_t g_t0 = 0;
std::string g_path;
Thread g_thread;

uint32_t Offset(uint64_t a) {
    uint64_t base = reinterpret_cast<uintptr_t>(&_start);
    return a >= base && a - base < 0xFFFFFFFFull ? uint32_t(a - base) : 0xFFFFFFFFu;
}

void Flush() {
    if (!g_n) return;
    if (FILE* f = std::fopen(g_path.c_str(), "ab")) {
        std::fwrite(g_samples, sizeof(Sample), g_n, f);
        std::fclose(f);
    }
    g_n = 0;
}

void LogSlots() {
    std::string line = "prof: slots";
    std::lock_guard lock(g_lock);
    for (int i = 0; i < g_count; i++) line += " " + std::to_string(i) + "=" + g_names[i];
    LOGR("%s", line.c_str());
}

void Loop(void*) {
    Handle h[kMaxThreads];
    uint64_t last[kMaxThreads] = {}, t_list = 0, t_flush = armGetSystemTick();
    bool busy[kMaxThreads] = {};
    int n = 0;
    const uint64_t freq = armGetSystemTickFreq();
    for (;;) {
        svcSleepThread(1000000ull);
        uint64_t now = armGetSystemTick();
        if (now - t_list > freq / 10) {
            {
                std::lock_guard lock(g_lock);
                n = g_count;
                std::memcpy(h, g_handles, sizeof(Handle) * size_t(n));
            }
            uint64_t span = now - t_list;
            for (int i = 0; i < n; i++) {
                uint64_t t = 0;
                if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, h[i], uint64_t(-1)))) t = 0;
                busy[i] = t_list && (t - last[i]) * 10 > span;  // thread ticks run at the system tick's 19.2 MHz
                last[i] = t;
            }
            t_list = now;
        }
        for (int i = 0; i < n && g_n < kMaxSamples; i++) {
            if (!busy[i] || R_FAILED(svcSetThreadActivity(h[i], ThreadActivity_Paused))) continue;
            ThreadContext ctx;
            if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h[i]))) {
                Sample& o = g_samples[g_n++];
                o.slot = uint16_t(i);
                o.time = uint16_t((now - g_t0) / (freq / 100));
                o.pc = Offset(ctx.pc.x);
                o.lr = Offset(ctx.lr);
                std::memset(o.ret, 0xFF, sizeof(o.ret));
                // the x29 chain inside the stack's own mapping: the thread is paused and its stack is ours
                MemoryInfo mi;
                u32 pi;
                if (!(ctx.fp & 7) && R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.fp)) && (mi.perm & Perm_R)) {
                    uint64_t fp = ctx.fp, lo = mi.addr, hi = mi.addr + mi.size;
                    for (int k = 0; k < 5 && fp >= lo && fp + 16 <= hi && !(fp & 7); k++) {
                        const uint64_t* frame = reinterpret_cast<const uint64_t*>(fp);
                        o.ret[k] = Offset(frame[1]);
                        if (frame[0] <= fp) break;
                        fp = frame[0];
                    }
                }
            }
            svcSetThreadActivity(h[i], ThreadActivity_Runnable);
        }
        if (now - t_flush > freq * 10) {
            LogSlots();
            Flush();
            t_flush = armGetSystemTick();
        }
    }
}

}  // namespace

// every named host thread (guest threads, the render thread) offers itself; read only while profiling
extern "C" void street_profile_thread(const char* name) {
    {
        // where Horizon runs it: a thread made without a core mask stays on the core it was made on
        s32 core = -1, prio = -1;
        u64 mask = 0;
        svcGetThreadCoreMask(&core, &mask, CUR_THREAD_HANDLE);
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        LOGR("threads: %s on core %d (mask %llX), priority %X", name, int(core), (unsigned long long)mask, unsigned(prio));
    }
    std::lock_guard lock(g_lock);
    if (g_count >= kMaxThreads) return;
    g_handles[g_count] = threadGetCurHandle();
    std::snprintf(g_names[g_count], sizeof(g_names[0]), "%s", name);
    for (char* c = g_names[g_count]; *c; c++)
        if (*c == ' ' || *c == '=') *c = '_';
    g_count++;
}

namespace host {

void StartProfiler() {
    g_path = (app::ExeDir() / "logs" / "prof.bin").string();
    std::remove(g_path.c_str());
    g_t0 = armGetSystemTick();
    // 0x2A: above every game and host thread, so it samples on time
    if (R_FAILED(threadCreate(&g_thread, Loop, nullptr, nullptr, 0x4000, 0x2A, -2)) || R_FAILED(threadStart(&g_thread))) {
        LOGE("prof: the profiler thread could not start");
        return;
    }
    LOGR("prof: sampling busy threads at 1 kHz into logs/prof.bin (NRO base %p)", reinterpret_cast<void*>(&_start));
}

}  // namespace host
