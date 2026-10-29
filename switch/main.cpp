// The Switch host: the applet's loop, the settings and folders under sdmc:/switch/street-nx, the screen size; the game is
// described by app::kGame (game/src/common/game_info.cpp). BLACK-NX-PORT's switch/main.cpp with MVP's settings
// (xbox_720p) and title mode check.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "build_info.h"
#include "build_stamp.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace {

constexpr const char* kDir = "sdmc:/switch/street-nx";

std::atomic<uint32_t> g_surface_epoch{0};
AppletOperationMode g_op_mode = AppletOperationMode_Handheld;  // the console's real mode
std::atomic<bool> g_docked{false};  // the mode the output follows: the real one, or ReverseNX-RT's when picked
bool g_saltynx = true;              // the saltynx setting

app::Mode ScreenMode() {
    return g_docked ? app::Mode{1920, 1080} : app::Mode{1280, 720};
}

}  // namespace

namespace host {

void* Window() { return nwindowGetDefault(); }
bool Foreground() { return true; }
uint32_t SurfaceEpoch() { return g_surface_epoch; }

void WindowPixels(int& w, int& h) {
    app::Mode m = ScreenMode();
    w = int(m.width);
    h = int(m.height);
}

}  // namespace host

namespace {
// buttons_by_name in the settings file: false (default) maps the face buttons by position, true by their letter
bool g_buttons_by_name = false;
// renderer in the settings file: "deko3d" (the console's own API, the default) or "vulkan" (NVK)
std::string g_renderer = "deko3d";
// deko3d_cached_upload: the upload ring CPU cached and cleaned before each submit, instead of uncached
bool g_dk_cached_upload = true;
// texture_check_frames: a texture nobody locked is hashed whole once in this many frames, sampled in the others
uint32_t g_texture_check_frames = 8;
// profile: the sampling profiler (profiler.cpp) writes logs/prof.bin
bool g_profile = false;
// frame_pacing: "vsync" (default) lets the screen pace the game when vsync is on and the cap is the refresh rate;
// "timer" also sleeps the title thread to its own 60 Hz grid, as before
std::string g_frame_pacing = "vsync";
// smaa: deko3d's anti-aliasing pass over each frame (off by default: about 4.7 ms of GPU at 960x720)
bool g_smaa = false;
}  // namespace

namespace host {
bool ButtonsByName() { return g_buttons_by_name; }
bool UseDeko3d() { return g_renderer == "deko3d"; }
bool DekoCachedUpload() { return g_dk_cached_upload; }
uint32_t TextureCheckFrames() { return g_texture_check_frames; }
bool Smaa() { return g_smaa; }
}  // namespace host

namespace app {

namespace {

std::filesystem::path File() { return ExeDir() / kGame.settings; }

toml::table Table() {
    const Settings& s = Get();
    // display_mode, width and height mean nothing on the Switch: the screen's size is the output
    toml::table t{{"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"xbox_720p", s.xbox_720p},
                  {"language", s.language},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"buttons_by_name", g_buttons_by_name},
                  {"renderer", g_renderer},
                  {"smaa", g_smaa},
                  {"frame_pacing", g_frame_pacing},
                  {"deko3d_cached_upload", g_dk_cached_upload},
                  {"texture_check_frames", int64_t(g_texture_check_frames)},
                  {"state_tracking", gpu::g_track_state},
                  {"render_spin_us", int64_t(gpu::g_render_spin_us.load())},
                  {"draw_copy", gpu::g_draw_copy.load()},
                  {"saltynx", g_saltynx},
                  {"profile", g_profile},
                  {"reuse_constants", gpu::g_reuse_constants.load()},
                  {"cpu_mhz", int64_t(host::ClockWanted("cpu"))},
                  {"gpu_mhz", int64_t(host::ClockWanted("gpu"))},
                  {"mem_mhz", int64_t(host::ClockWanted("mem"))}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

// the Switch starts at the console's own pace: 30 frames a second, vsync on, the frame at the screen's height
Settings DefaultSettings() {
    Settings s;
    s.render_percent = 100;
    s.fps_cap = 60;
    s.vsync = true;
    s.xbox_720p = false;  // FIFA Street was tested on the PC as a stock 480 Xbox (CreateDevice 640x480); 720p untested
    s.aniso = 2;  // the ground and walls seen at an angle stay sharp further away; almost free on the GPU
    gpu::g_draw_copy = true;  // the vertices copied as each draw is made: no per-draw wait (validated on the PC)
    gpu::g_track_state = true;  // the render thread compares only the render states and constants written
    gpu::g_reuse_constants = true;  // a draw with the previous draw's constants reuses their upload
    return s;
}

Settings& Get() {
    static Settings s = DefaultSettings();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.xbox_720p = t["xbox_720p"].value_or(s.xbox_720p);
    s.language = t["language"].value_or(s.language);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    g_buttons_by_name = t["buttons_by_name"].value_or(g_buttons_by_name);
    g_renderer = t["renderer"].value_or(g_renderer);
    if (g_renderer != "vulkan") g_renderer = "deko3d";
    g_dk_cached_upload = t["deko3d_cached_upload"].value_or(g_dk_cached_upload);
    g_smaa = t["smaa"].value_or(g_smaa);
    g_frame_pacing = t["frame_pacing"].value_or(g_frame_pacing);
    if (g_frame_pacing != "timer") g_frame_pacing = "vsync";
    gpu::g_track_state = t["state_tracking"].value_or(gpu::g_track_state);
    g_texture_check_frames = std::clamp<uint32_t>(t["texture_check_frames"].value_or(g_texture_check_frames), 1, 60);
    gpu::g_render_spin_us = std::min<uint32_t>(t["render_spin_us"].value_or(gpu::g_render_spin_us.load()), 2000);
    gpu::g_draw_copy = t["draw_copy"].value_or(gpu::g_draw_copy.load());
    g_saltynx = t["saltynx"].value_or(g_saltynx);
    g_profile = t["profile"].value_or(g_profile);
    gpu::g_reuse_constants = t["reuse_constants"].value_or(gpu::g_reuse_constants.load());
    for (const char* c : {"cpu", "gpu", "mem"})
        host::SetClockWanted(c, t[std::string(c) + "_mhz"].value_or(host::ClockWanted(c)));
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, build %s (%s), Switch", kGame.title, IMAGE_XBE, date, STREET_NX_BUILD, STREET_NX_STAMP);
    LOGR("Path: %s", ExeDir().c_str());
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    LOGR("Applet: %s, memory %llu MB (%llu MB used)",
         appletGetAppletType() == AppletType_Application ? "application (full memory)" : "applet",
         (unsigned long long)(total >> 20), (unsigned long long)(used >> 20));
    Mode desk = Desktop();
    LOGR("Display: %ux%u (%s)", desk.width, desk.height, g_docked ? "docked" : "handheld");
    LogSettings();
}

void ApplyDisplay() {}

Mode Desktop() { return ScreenMode(); }

Mode Output() { return Desktop(); }

std::string SystemLanguage() {
    u64 code = 0;
    if (R_FAILED(setInitialize())) return {};
    Result rc = setGetSystemLanguage(&code);
    setExit();
    if (R_FAILED(rc)) return {};
    char text[9] = {};
    std::memcpy(text, &code, 8);  // "es", "es-419", "en-US"...
    return std::string(text, 2);
}

bool Widescreen() {
    Mode m = Output();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

std::filesystem::path ExeDir() { return std::filesystem::path(kDir); }

int Instance() { return 0; }

void* Window() { return nwindowGetDefault(); }

// no splash and no relaunch into another executable on the Switch
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    // with vsync the screen already paces the game: the render thread waits for the screen's image and the title
    // thread for the frame before. A second clock beside it drifts against the refresh, and its 1 ms sleeps overshoot
    // under load: frames missed the refresh now and then, with no frame queued to hide it
    if (g_frame_pacing == "vsync" && Get().vsync && cap >= 60) cap = 0;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
    gpu::g_perf_idle_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - now).count());
}

}  // namespace app

namespace {

void InitUi() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "switch_libnx";
}

}  // namespace

// libnx calls this for any CPU exception: the report goes to the log, then the process ends, so the log's last lines
// say where. pc - main + main's ELF address (aarch64-none-elf-nm street.elf) is the ELF address.
int main(int, char**);

extern "C" {
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    uintptr_t base = reinterpret_cast<uintptr_t>(kernel::g_base);
    uint64_t far = ctx->far.x;
    LOGE("crash: exception %u at pc %016llX lr %016llX sp %016llX, address %016llX", ctx->error_desc,
         (unsigned long long)ctx->pc.x, (unsigned long long)ctx->lr.x, (unsigned long long)ctx->sp.x,
         (unsigned long long)far);
    LOGE("crash: main at %p; guest base %p%s", reinterpret_cast<void*>(&main), kernel::g_base,
         base && far >= base && far < base + (1ull << 32) ? "" : " (address outside guest memory)");
    if (base && far >= base && far < base + (1ull << 32)) LOGE("crash: guest address %08X", uint32_t(far - base));
    for (int i = 0; i < 29; i += 4)
        LOGE("crash: x%-2d %016llX %016llX %016llX %016llX", i, (unsigned long long)ctx->cpu_gprs[i].x,
             (unsigned long long)ctx->cpu_gprs[i + 1].x, (unsigned long long)ctx->cpu_gprs[i + 2].x,
             (unsigned long long)ctx->cpu_gprs[i + 3].x);
    // on a guest thread: its registers and the return addresses on its stack place the guest code
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("crash: on guest thread %X", t->id);
        kernel::LogGuestStack(*t->ctx, 256);
    }
    logx::Flush();
    svcExitProcess();
}
}

// the driver (Mesa) ends the process with abort() or a failed assert, not a CPU exception: say where first
extern "C" [[noreturn]] void abort() {
    LOGE("crash: abort() called from %p (main at %p)", __builtin_return_address(0), reinterpret_cast<void*>(&main));
    logx::Flush();
    std::fflush(stderr);
    svcExitProcess();
    __builtin_unreachable();
}

extern "C" [[noreturn]] void __assert_func(const char* file, int line, const char* func, const char* expr) {
    LOGE("crash: assert %s failed in %s, %s:%d", expr, func ? func : "?", file, line);
    abort();
}

namespace {

// opened from the album (applet mode) the title gets a fraction of the memory: hbmenu must run over a game
bool TitleMode() {
    AppletType t = appletGetAppletType();
    return t == AppletType_Application || t == AppletType_SystemApplication;
}

// a message to the player before the game runs: the console's error applet, then the title ends
void Fatal(const char* text) {
    LOGE("%s", text);
    logx::Flush();
    ErrorApplicationConfig c;
    errorApplicationCreate(&c, text, nullptr);
    errorApplicationShow(&c);
}

}  // namespace

int main(int, char**) {
    // an uncaught C++ exception or std::abort ends the process without a CPU exception: say so in the log first
    std::set_terminate([] {
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
            LOGE("crash: std::terminate (abort, no exception)");
        } catch (const std::system_error& e) {
            LOGE("crash: uncaught std::system_error %d: %s", e.code().value(), e.what());
        } catch (const std::exception& e) {
            LOGE("crash: uncaught exception: %s", e.what());
        } catch (...) {
            LOGE("crash: uncaught exception of an unknown type");
        }
        logx::Flush();
        svcExitProcess();
    });
    appletLockExit();
    g_op_mode = appletGetOperationMode();
    g_docked = g_op_mode == AppletOperationMode_Console;
    std::filesystem::create_directories(std::filesystem::path(kDir) / "logs");
    // what the driver prints goes to stderr: a file beside the logs, written as it comes
    std::freopen((std::string(kDir) + "/logs/stderr.log").c_str(), "w", stderr);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    host::OverlaysEnable(g_saltynx);
    host::OverlaysInit();  // before the game: its sm session can be lent for a moment if the process has no other
    g_docked = host::OverlaysDocked(g_op_mode == AppletOperationMode_Console);
    app::LogSystem();
    host::ApplyClocks();
    if (g_profile) host::StartProfiler();

    if (!TitleMode()) {
        Fatal("FIFA Street necesita el modo titulo. Abre hbmenu manteniendo R mientras inicias cualquier "
              "juego instalado y elige FIFA Street desde ahi (no desde el album).");
        appletUnlockExit();
        return 1;
    }

    app::GameFiles game;
    if (!app::FindGame(game)) {
        Fatal("FIFA Street: no se encontraron los archivos del juego (title ID 45410072). Copia default.xbe y "
              "sus carpetas, o el .iso, en sdmc:/switch/street-nx/game.");
        appletUnlockExit();
        return 1;
    }

    LOGI("app: ImGui");
    InitUi();
    LOGI("app: controllers");
    input::Init();
    LOGI("app: game window hook");
    if (app::kGame.window) app::kGame.window(nwindowGetDefault());
    LOGI("app: booting the image");
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        Fatal("FIFA Street no pudo arrancar. Mira el log en sdmc:/switch/street-nx/logs.");
        appletUnlockExit();
        return 1;
    }

    // the applet's messages (HOME, docking, exit) on this thread; the game runs on its own
    auto alive = std::chrono::steady_clock::now();
    while (appletMainLoop()) {
        // a line every 5 seconds: a log that stops with these still coming is a hang, not an exit
        if (std::chrono::steady_clock::now() - alive > std::chrono::seconds(5)) {
            alive = std::chrono::steady_clock::now();
            LOGI("app: alive");
        }
        AppletOperationMode m = appletGetOperationMode();
        if (m != g_op_mode) {
            g_op_mode = m;
            LOGR("app: console %s", m == AppletOperationMode_Console ? "docked" : "handheld");
            host::ApplyClocks();  // the system set its own for the new mode
        }
        host::WatchClocks();
        // the output follows ReverseNX-RT's pick when there is one (no event for it: asked every 100 ms)
        bool docked = host::OverlaysDocked(m == AppletOperationMode_Console);
        if (docked != g_docked) {
            g_docked = docked;
            g_surface_epoch++;
            LOGR("app: output %s%s", docked ? "docked" : "handheld",
                 docked != (m == AppletOperationMode_Console) ? " (ReverseNX-RT)" : "");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    host::RestoreClocks();
    logx::Flush();
    kernel::Shutdown(0);
    appletUnlockExit();
    return 0;
}
