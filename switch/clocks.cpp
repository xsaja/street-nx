// CPU, GPU and memory clocks from the settings file (cpu_mhz, gpu_mhz, mem_mhz; 0 leaves the system's), set through
// clkrst (pcv before 8.0.0) and put back as they were when the title exits. Capped at the console's own top rates:
// a rate above them needs an overclocking sysmodule, which this does not ask for.
#include <switch.h>

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "host.h"

namespace {

struct Domain {
    PcvModuleId id;   // clkrst, 8.0.0 on
    PcvModule old;    // pcv, before
    const char* name;
    uint32_t max_mhz;
    uint32_t want_mhz = 0;     // 0: the system's
    uint32_t original_hz = 0;  // read before the first change, put back at exit
};

Domain g_domains[3] = {
    {PcvModuleId_CpuBus, PcvModule_CpuBus, "cpu", 1785},
    {PcvModuleId_GPU, PcvModule_GPU, "gpu", 921},
    {PcvModuleId_EMC, PcvModule_EMC, "mem", 1600},
};

bool UseClkrst() { return hosversionAtLeast(8, 0, 0); }

uint32_t GetHz(const Domain& d) {
    uint32_t hz = 0;
    if (UseClkrst()) {
        ClkrstSession s;
        if (R_SUCCEEDED(clkrstOpenSession(&s, d.id, 3))) {
            clkrstGetClockRate(&s, &hz);
            clkrstCloseSession(&s);
        }
    } else {
        pcvGetClockRate(d.old, &hz);
    }
    return hz;
}

bool SetHz(const Domain& d, uint32_t hz) {
    Result rc;
    if (UseClkrst()) {
        ClkrstSession s;
        rc = clkrstOpenSession(&s, d.id, 3);
        if (R_SUCCEEDED(rc)) {
            rc = clkrstSetClockRate(&s, hz);
            clkrstCloseSession(&s);
        }
    } else {
        rc = pcvSetClockRate(d.old, hz);
    }
    return R_SUCCEEDED(rc);
}

bool g_open = false;

bool Open() {
    if (g_open) return true;
    Result rc = UseClkrst() ? clkrstInitialize() : pcvInitialize();
    if (R_FAILED(rc)) {
        LOGE("clocks: %s unavailable (0x%X)", UseClkrst() ? "clkrst" : "pcv", rc);
        return false;
    }
    g_open = true;
    return true;
}

}  // namespace

namespace host {

void SetClockWanted(const char* name, uint32_t mhz) {
    for (Domain& d : g_domains)
        if (!std::strcmp(d.name, name)) d.want_mhz = mhz;
}

uint32_t ClockWanted(const char* name) {
    for (const Domain& d : g_domains)
        if (!std::strcmp(d.name, name)) return d.want_mhz;
    return 0;
}

// at start and after each docked/handheld change, which puts the system's back
void ApplyClocks() {
    bool any = false;
    for (const Domain& d : g_domains) any |= d.want_mhz != 0;
    if (!any || !Open()) return;
    for (Domain& d : g_domains) {
        if (!d.want_mhz) continue;
        if (!d.original_hz) d.original_hz = GetHz(d);
        uint32_t mhz = std::min(d.want_mhz, d.max_mhz);
        bool ok = SetHz(d, mhz * 1000000u);
        LOGR("clocks: %s %u MHz asked%s, now %u MHz (was %u MHz)", d.name, d.want_mhz,
             mhz != d.want_mhz ? ", capped" : "", GetHz(d) / 1000000u, d.original_hz / 1000000u);
        if (!ok) LOGE("clocks: %s could not be set", d.name);
    }
}

// once a second: the system or a clock sysmodule (sys-clk, Horizon OC) may set its own rate after ours; it is logged
// and ours set again, so a log shows whether the asked clock held
void WatchClocks() {
    static uint64_t next = 0;
    static uint32_t resets = 0;
    if (!g_open) return;
    uint64_t now = armTicksToNs(armGetSystemTick());
    if (now < next) return;
    next = now + 1000000000ull;
    for (Domain& d : g_domains) {
        if (!d.want_mhz) continue;
        uint32_t want = std::min(d.want_mhz, d.max_mhz), is = GetHz(d) / 1000000u;
        if (is == want) continue;
        ++resets;
        if (resets <= 10 || resets % 60 == 0)
            LOGR("clocks: %s at %u MHz instead of %u (set by the system or a clock sysmodule; %u times), set again", d.name,
                 is, want, resets);
        SetHz(d, want * 1000000u);
    }
}

void RestoreClocks() {
    if (!g_open) return;
    for (Domain& d : g_domains)
        if (d.original_hz) {
            SetHz(d, d.original_hz);
            d.original_hz = 0;
        }
    if (UseClkrst()) clkrstExit();
    else pcvExit();
    g_open = false;
}

}  // namespace host

// libnx calls this on the way out of a normal exit: the system's clocks back
extern "C" void userAppExit(void) { host::RestoreClocks(); }
