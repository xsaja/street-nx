// dk_test.nro: deko3d on the console before the game uses it (stage D1). Modeled on NFSMW-NX's dk_prueba (GPL-3.0).
// Everything goes to sdmc:/switch/street-nx/logs/dk_test.log.
//
//  1. uam on the console: the test's own shaders are compiled at start by the uam library the game will use, on a
//     worker thread as the game's will be (8 MB stack, priority 0x3B). "uam ready" or "SELF-TEST FAILED".
//  2. The core (switch/dk_core): device, queue, work slots, upload ring, image heap, descriptors, swapchain at the
//     console's mode (1280x720 handheld, 1920x1080 docked).
//  3. The game's shaders: every sdmc:/switch/street-nx/shadercache/*.spv (the Vulkan cache the game already made) through
//     switch/dk_glsl and uam, timed, and compared with the DKSH the PC made from the same file (pc_dksh.h).
//  4. The test pattern, which must look like this (Direct3D's conventions, the ones the game's shaders use):
//       - four quadrants: red top left, green top right, blue bottom left, white bottom right;
//       - in the middle three overlapping squares with a depth test: magenta (far), yellow (near) and cyan (the
//         farthest, drawn last): yellow covers magenta, and cyan only shows where it overlaps nothing;
//       - a textured square below the middle: a white mark in its TOP LEFT corner, red growing to the right, green
//         growing downwards;
//       - a bar at the bottom: the progress of the shader checks (yellow), then green (all good) or red.
//  5. The GPU cost of every pixel shader of the game: full-screen passes at the screen's size with dummy textures
//     and constants, timed with GPU timestamps and checked against the wall clock.
// + exits.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dk_core.h"
#include "dk_glsl.h"
#include "pc_dksh.h"
#include "uam_api.h"

namespace fs = std::filesystem;

namespace {

const char* const kDir = "sdmc:/switch/street-nx/";

FILE* g_log = nullptr;
void WriteLine(const char* line) {
    if (!g_log) return;
    std::fputs(line, g_log);
    std::fputc('\n', g_log);
}
void FlushFile() {
    if (g_log) std::fflush(g_log);
}

std::vector<uint8_t> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() <= 0) return {};
    std::vector<uint8_t> d(size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(d.data()), std::streamsize(d.size()));
    return d;
}

uint64_t Fnv(const std::vector<uint8_t>& d) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : d) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

// ---- The test's own shaders

// one quad per draw from gl_VertexID (a triangle strip): its rectangle in clip space (x0, y0, x1, y1, y up as in
// Direct3D), color and depth from the constant buffer; uv (0, 0) is the corner at (x0, y0)
const char kQuadVs[] = R"(#version 460
layout(std140, binding = 0) uniform Quad { vec4 rect; vec4 color; vec4 extra; } q;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
void main() {
    vec2 uv = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(mix(q.rect.xy, q.rect.zw, uv), q.extra.x, 1.0);
    v_color = q.color;
    v_uv = uv;
}
)";
const char kQuadPs[] = R"(#version 460
layout(std140, binding = 0) uniform Quad { vec4 rect; vec4 color; vec4 extra; } q;
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o;
void main() { o = q.extra.y > 0.5 ? texture(tex, v_uv) : v_color; }
)";
// the benchmark's vertex shader: a full-screen quad writing the ten interpolators the game's pixel shaders read
// (the SDK's VSOut: colors, four texture coordinates, fog)
const char kScreenVs[] = R"(#version 460
layout(location = 0) out vec4 o[10];
void main() {
    vec2 uv = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    for (int i = 0; i < 10; ++i) o[i] = vec4(uv, 0.25 + 0.05 * float(i), 1.0);
}
)";

struct Own {
    const char* name;
    const char* glsl;
    bool vertex;
    std::vector<uint8_t> dksh;
};
Own g_own[] = {{"quad_vs", kQuadVs, true, {}}, {"quad_ps", kQuadPs, false, {}}, {"screen_vs", kScreenVs, true, {}}};

// ---- The compile worker: uam is not reentrant and needs a big stack

struct GameShader {
    std::string name;
    bool vertex = false;
    std::vector<uint8_t> dksh;
};

std::atomic<int> g_phase{0};  // 0 own shaders, 1 game's, 2 done
std::atomic<uint32_t> g_done{0}, g_total{1};
std::atomic<bool> g_failed{false};
std::vector<GameShader> g_game;

void Worker(void*) {
    const uint64_t t0 = dk::NowNs();
    uam::init(true);
    dk::Log("[test] uam started in %.1f ms", (dk::NowNs() - t0) / 1e6);
    for (Own& o : g_own) {
        uam::Result r = uam::compile(o.vertex ? uam::Stage::Vertex : uam::Stage::Fragment, o.glsl);
        if (!r.ok) {
            dk::Log("[test] SELF-TEST FAILED: %s does not compile: %s", o.name, r.log.c_str());
            g_failed = true;
        }
        o.dksh = std::move(r.dksh);
    }
    if (!g_failed) dk::Log("[test] uam ready: %zu own shaders", std::size(g_own));
    dk::FlushLog();
    g_phase = 1;

    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(kDir) / "shadercache", ec))
        if (e.path().extension() == ".spv") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    dk::Log("[test] the game's shader cache: %zu SPIR-V modules%s", files.size(),
            files.empty() ? " (play the game once with the Vulkan build to make it)" : "");
    g_total = uint32_t(std::max<size_t>(files.size(), 1));
    size_t same = 0, different = 0, failed = 0, unknown = 0;
    std::vector<double> ms;
    for (const fs::path& p : files) {
        GameShader s;
        s.name = p.stem().string();
        std::vector<uint8_t> bytes = ReadFile(p);
        std::vector<uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), words.size() * 4);
        const uint64_t t = dk::NowNs();
        dkglsl::Glsl g = dkglsl::FromSpirv(words);
        uam::Result r;
        if (!g.text.empty())
            r = uam::compile(g.stage == dkglsl::Stage::Vertex ? uam::Stage::Vertex : uam::Stage::Fragment, g.text.c_str());
        ms.push_back((dk::NowNs() - t) / 1e6);
        g_done++;
        if (!r.ok) {
            if (failed++ < 20) dk::Log("[test] FAILED %s: %s%s", s.name.c_str(), g.error.c_str(), r.log.c_str());
            continue;
        }
        if (!r.log.empty()) dk::Log("[test] uam warning on %s: %s", s.name.c_str(), r.log.c_str());
        const PcDksh* pc = nullptr;
        for (const PcDksh& k : kPcDksh)
            if (s.name == k.name) pc = &k;
        if (!pc) unknown++;
        else if (pc->fnv == Fnv(r.dksh) && pc->bytes == r.dksh.size()) same++;
        else if (different++ < 20)
            dk::Log("[test] DIFFERENT from the PC: %s (%zu bytes here, %u on the PC)", s.name.c_str(), r.dksh.size(),
                    pc->bytes);
        s.vertex = g.stage == dkglsl::Stage::Vertex;
        s.dksh = std::move(r.dksh);
        g_game.push_back(std::move(s));
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    auto pct = [&](double p) { return ms.empty() ? 0.0 : ms[std::min(ms.size() - 1, size_t(p * ms.size()))]; };
    dk::Log("[test] compiled on the console: %zu shaders, %zu failed; mean %.1f ms, p50 %.1f, p90 %.1f, max %.1f; "
            "total %.2f s",
            ms.size(), failed, ms.empty() ? 0.0 : sum / ms.size(), pct(0.5), pct(0.9), ms.empty() ? 0.0 : ms.back(),
            sum / 1000);
    dk::Log("[test] against the PC: %zu identical, %zu different, %zu not in the PC's list", same, different, unknown);
    if (failed || different || files.empty()) g_failed = true;
    dk::FlushLog();
    g_phase = 2;
}

// ---- Rendering

struct Image {
    DkImage image;
    dk::ImageMemory memory;
};

void CreateImage(dk::Core& c, Image& img, DkImageFormat format, uint32_t flags, uint32_t width, uint32_t height,
                 const char* name) {
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, c.device());
    m.flags = flags;
    m.format = format;
    m.dimensions[0] = width;
    m.dimensions[1] = height;
    dk::FixTileSize(m, height);
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    img.memory = c.ReserveImage(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&img.image, &layout, img.memory.block, img.memory.offset);
    dk::Log("[test] image %s: %ux%u, %u KiB", name, width, height, img.memory.bytes >> 10);
}

void Quad(dk::Core& c, float x0, float y0, float x1, float y1, float r, float g, float b, float z, bool textured) {
    const dk::Upload u = c.Reserve(48, DK_UNIFORM_BUF_ALIGNMENT);
    if (!u) return;
    const float d[12] = {x0, y0, x1, y1, r, g, b, 1.0f, z, textured ? 1.0f : 0.0f, 0, 0};
    std::memcpy(u.cpu, d, sizeof(d));
    dkCmdBufBindUniformBuffer(c.cmd(), DkStage_Vertex, 0, u.gpu, 48);
    dkCmdBufBindUniformBuffer(c.cmd(), DkStage_Fragment, 0, u.gpu, 48);
    dkCmdBufDraw(c.cmd(), DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

void BasicState(dk::Core& c, uint32_t width, uint32_t height, bool depth) {
    DkCmdBuf cmd = c.cmd();
    const DkViewport vp = {0, 0, float(width), float(height), 0, 1};
    const DkScissor sc = {0, 0, width, height};
    dkCmdBufSetViewports(cmd, 0, &vp, 1);
    dkCmdBufSetScissors(cmd, 0, &sc, 1);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    rs.provokingVertex = DkProvokingVertex_First;  // Vulkan's and Direct3D's
    DkColorState cs;
    dkColorStateDefaults(&cs);
    DkColorWriteState cws;
    dkColorWriteStateDefaults(&cws);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = depth;
    ds.depthWriteEnable = depth;
    dkCmdBufBindRasterizerState(cmd, &rs);
    dkCmdBufBindColorState(cmd, &cs);
    dkCmdBufBindColorWriteState(cmd, &cws);
    dkCmdBufBindDepthStencilState(cmd, &ds);
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
}

// the pattern (see the top of the file) with the bound targets; progress 0..1 and its color for the bottom bar
void DrawPattern(dk::Core& c, const DkShader* quad[2], uint32_t width, uint32_t height, float progress, float r, float g) {
    DkCmdBuf cmd = c.cmd();
    BasicState(c, width, height, true);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.1f, 0.1f, 0.1f, 1.0f);
    dkCmdBufClearDepthStencil(cmd, true, 1.0f, 0xFF, 0);
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, quad, 2);
    const DkResHandle h = dkMakeTextureHandle(0, 0);
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &h, 1);
    // quadrants (clip-space y up: y = 1 is the top row)
    Quad(c, -1, 1, 0, 0, 1, 0, 0, 0.99f, false);
    Quad(c, 0, 1, 1, 0, 0, 1, 0, 0.99f, false);
    Quad(c, -1, 0, 0, -1, 0, 0, 1, 0.99f, false);
    Quad(c, 0, 0, 1, -1, 1, 1, 1, 0.99f, false);
    // depth: magenta far, yellow near, cyan farthest and drawn last
    Quad(c, -0.30f, 0.45f, 0.10f, -0.05f, 1, 0, 1, 0.6f, false);
    Quad(c, -0.10f, 0.30f, 0.30f, -0.20f, 1, 1, 0, 0.4f, false);
    Quad(c, -0.20f, 0.60f, 0.40f, 0.15f, 0, 1, 1, 0.8f, false);
    // textured square: uv (0, 0) at its top left
    Quad(c, -0.15f, -0.30f, 0.15f, -0.75f, 1, 1, 1, 0.5f, true);
    Quad(c, -1, -0.92f, -1 + 2 * progress, -1, r, g, 0, 0.1f, false);
}

// The pattern drawn into an image of its own, read back and checked at points whose color is known: orientation,
// depth test and texture upload, without anyone having to look at the screen
bool CheckPattern(dk::Core& c, const DkShader* quad[2], uint32_t width, uint32_t height) {
    Image color, depth;
    CreateImage(c, color, DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender, width, height, "check color");
    CreateImage(c, depth, DkImageFormat_Z24S8, DkImageFlags_UsageRender, width, height, "check depth");
    const uint32_t bytes = width * height * 4;
    DkMemBlock readback = c.CreateReadbackBlock(bytes, "pattern readback");
    dkMemBlockFlushCpuCache(readback, 0, bytes);
    c.BeginWork(900);
    c.BindDescriptors();
    c.InvalidateCodeIfNeeded();
    DkImageView cv, zv;
    dkImageViewDefaults(&cv, &color.image);
    dkImageViewDefaults(&zv, &depth.image);
    dkCmdBufBindRenderTarget(c.cmd(), &cv, &zv);
    DrawPattern(c, quad, width, height, 0.5f, 1, 1);
    dkCmdBufBarrier(c.cmd(), DkBarrier_Fragments, 0);
    const DkImageRect rect = {0, 0, 0, width, height, 1};
    const DkCopyBuf dst = {dkMemBlockGetGpuAddr(readback), width * 4, height};
    dkCmdBufCopyImageToBuffer(c.cmd(), &cv, &rect, &dst, 0);
    c.EndWork();
    c.WaitGpu();
    dkMemBlockFlushCpuCache(readback, 0, bytes);
    const uint8_t* px = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(readback));

    struct Point {
        const char* what;
        float x, y;  // clip space, y up
        int r, g, b;
    };
    const Point points[] = {
        {"top left quadrant red", -0.9f, 0.9f, 255, 0, 0},
        {"top right quadrant green", 0.9f, 0.9f, 0, 255, 0},
        {"bottom left quadrant blue", -0.9f, -0.5f, 0, 0, 255},
        {"bottom right quadrant white", 0.9f, -0.5f, 255, 255, 255},
        {"yellow (near) over magenta", 0.0f, 0.1f, 255, 255, 0},
        {"magenta over cyan (farther, drawn later)", -0.15f, 0.3f, 255, 0, 255},
        {"cyan alone", 0.35f, 0.5f, 0, 255, 255},
        {"texture top left: the white mark", -0.14f, -0.31f, 255, 255, 255},
        {"texture top right: red", 0.14f, -0.31f, 252, 0, 0},
        {"texture bottom left: green", -0.14f, -0.74f, 0, 252, 0},
    };
    int bad = 0;
    for (const Point& p : points) {
        const uint32_t x = std::min(width - 1, uint32_t((p.x + 1) * 0.5f * width));
        const uint32_t y = std::min(height - 1, uint32_t((1 - p.y) * 0.5f * height));
        const uint8_t* q = px + (size_t(y) * width + x) * 4;
        // the texture is filtered and its edges blend: a generous tolerance, the point is the right corner
        const bool ok = std::abs(q[0] - p.r) < 48 && std::abs(q[1] - p.g) < 48 && std::abs(q[2] - p.b) < 48;
        if (!ok) bad++;
        dk::Log("[test] pattern %s: %s at (%u, %u) is %u %u %u, expected %d %d %d", ok ? "ok  " : "BAD ", p.what, x, y,
                q[0], q[1], q[2], p.r, p.g, p.b);
    }
    dk::Log("[test] pattern check: %s (%d of %zu points wrong)", bad ? "FAILED" : "all good", bad, std::size(points));
    dk::FlushLog();
    c.BeginWork(901);
    c.ReleaseImageLater(color.memory);
    c.ReleaseImageLater(depth.memory);
    c.EndWork();
    return bad == 0;
}

// the game's pixel shaders on full-screen passes
void MeasureShaders(dk::Core& c, const DkShader& screen_vs, uint32_t width, uint32_t height) {
    Image target;
    CreateImage(c, target, DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender, width, height, "benchmark target");
    struct Measure {
        size_t i;
        DkShader ps;
        double ms = 0;
    };
    std::vector<Measure> measures;
    for (size_t i = 0; i < g_game.size(); ++i) {
        if (g_game[i].vertex) continue;
        Measure m{i, {}};
        if (c.LoadShader(m.ps, g_game[i].dksh, g_game[i].name.c_str())) measures.push_back(m);
    }
    // constants: every float 0.5 in both buffers (the SDK's VC at 0 and DC at 1), a lit, textured, fogless guess
    static float constants[1024];
    std::fill(std::begin(constants), std::end(constants), 0.5f);
    constexpr uint32_t kPasses = 8;
    constexpr size_t kPerWork = 16;
    uint64_t work = 1000;
    double wall_ns = 0, gpu_ns = 0;
    for (size_t a = 0; a < measures.size(); a += kPerWork) {
        const size_t b = std::min(measures.size(), a + kPerWork);
        c.BeginWork(work++);
        DkCmdBuf cmd = c.cmd();
        c.BindDescriptors();
        c.InvalidateCodeIfNeeded();
        DkImageView view;
        dkImageViewDefaults(&view, &target.image);
        dkCmdBufBindRenderTarget(cmd, &view, nullptr);
        BasicState(c, width, height, false);
        const dk::Upload ubo = c.Reserve(sizeof(constants), DK_UNIFORM_BUF_ALIGNMENT);
        std::memcpy(ubo.cpu, constants, sizeof(constants));
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, ubo.gpu, sizeof(constants));
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, ubo.gpu, sizeof(constants));
        const DkResHandle h = dkMakeTextureHandle(0, 0);
        for (uint32_t t = 0; t < 4; ++t) dkCmdBufBindTextures(cmd, DkStage_Fragment, t, &h, 1);
        for (size_t k = a; k < b; ++k) {
            const DkShader* pair[] = {&screen_vs, &measures[k].ps};
            dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
            dkCmdBufReportCounter(cmd, DkCounter_Timestamp, c.QueriesGpu() + (k - a) * 32);
            for (uint32_t p = 0; p < kPasses; ++p) dkCmdBufDraw(cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
            dkCmdBufReportCounter(cmd, DkCounter_Timestamp, c.QueriesGpu() + (k - a) * 32 + 16);
        }
        const uint64_t t0 = dk::NowNs();
        c.EndWork();
        c.WaitGpu();
        wall_ns += double(dk::NowNs() - t0);
        for (size_t k = a; k < b; ++k) {
            uint64_t begin, end;
            std::memcpy(&begin, c.QueriesCpu() + (k - a) * 32 + 8, 8);
            std::memcpy(&end, c.QueriesCpu() + (k - a) * 32 + 24, 8);
            const double ns = double(end - begin) * dk::kNsPerGpuUnit;
            gpu_ns += ns;
            measures[k].ms = ns / kPasses / 1e6;
        }
    }
    std::sort(measures.begin(), measures.end(), [](const Measure& x, const Measure& y) { return x.ms > y.ms; });
    dk::Log("[test] GPU cost of the game's %zu pixel shaders: full-screen passes of %ux%u, %u each", measures.size(),
            width, height, kPasses);
    for (const Measure& m : measures) dk::Log("[test]   %s  %.3f ms a pass", g_game[m.i].name.c_str(), m.ms);
    dk::Log("[test] GPU (x%.3f) %.1f ms against %.1f ms of wall clock for all of it (submit and wait included): if "
            "the GPU adds up to more than the wall, the factor is wrong",
            dk::kNsPerGpuUnit, gpu_ns / 1e6, wall_ns / 1e6);
    dk::FlushLog();
    c.ReleaseImageLater(target.memory);
}

}  // namespace

int main(int, char**) {
    std::error_code ec;
    fs::create_directories(fs::path(kDir) / "logs", ec);
    g_log = std::fopen((std::string(kDir) + "logs/dk_test.log").c_str(), "w");
    dk::SetLog(WriteLine, FlushFile);
    dk::Log("[test] mvp-nx deko3d test (stage D1): the core, uam and the game's shaders on the console");

    // uam on its own thread, as the game's compile worker will be
    Thread worker;
    if (R_FAILED(threadCreate(&worker, Worker, nullptr, nullptr, 8u << 20, 0x3B, -2)) || R_FAILED(threadStart(&worker)))
        dk::Fatal("[test] the compile thread could not be created");

    uint32_t width, height;
    dk::ScreenSize(width, height);
    dk::Device device;
    device.Create(true);
    dk::Core c;
    dk::CoreConfig config;
    config.upload_mb = 16;
    c.Create(device, config);
    dk::Swapchain swapchain;
    swapchain.Create(device, width, height, 1);
    Image depth;
    CreateImage(c, depth, DkImageFormat_Z24S8, DkImageFlags_UsageRender, width, height, "depth");

    while (g_phase.load() < 1) svcSleepThread(1'000'000);
    DkShader own[std::size(g_own)];
    bool ready = !g_failed;
    for (size_t i = 0; ready && i < std::size(g_own); ++i) ready = c.LoadShader(own[i], g_own[i].dksh, g_own[i].name);

    // a 64x64 gradient with a white mark at its top left (the orientation check); descriptor 0, sampler 0 (linear,
    // repeat)
    Image tex;
    CreateImage(c, tex, DkImageFormat_RGBA8_Unorm, 0, 64, 64, "texture");
    c.BeginWork(0);
    {
        std::vector<uint32_t> texels(64 * 64);
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x)
                texels[y * 64 + x] = (x < 8 && y < 8) ? 0xFFFFFFFFu : 0xFF000000u | (y * 4) << 8 | (x * 4);
        const dk::Upload u = c.Reserve(64 * 64 * 4, 256);
        std::memcpy(u.cpu, texels.data(), 64 * 64 * 4);
        DkImageView view;
        dkImageViewDefaults(&view, &tex.image);
        const DkCopyBuf src = {u.gpu, 0, 0};
        const DkImageRect rect = {0, 0, 0, 64, 64, 1};
        dkCmdBufCopyBufferToImage(c.cmd(), &src, &view, &rect, 0);
        DkImageDescriptor d;
        dkImageDescriptorInitialize(&d, &view, false, false);
        dkCmdBufPushData(c.cmd(), c.ImageDescriptors(), &d, 32);
        DkSampler s;
        dkSamplerDefaults(&s);
        s.minFilter = s.magFilter = DkFilter_Linear;
        s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_Repeat;
        DkSamplerDescriptor sd;
        dkSamplerDescriptorInitialize(&sd, &s);
        dkCmdBufPushData(c.cmd(), c.SamplerDescriptors(), &sd, 32);
        // the copy and the descriptors before anything samples them
        dkCmdBufBarrier(c.cmd(), DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    }
    c.EndWork();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    const DkShader* quad[] = {&own[0], &own[1]};
    const bool pattern_ok = ready && CheckPattern(c, quad, width, height);
    bool measured = false;
    uint64_t work = 1, frames = 0;
    const uint64_t start = dk::NowNs();
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        if (!ready) {
            svcSleepThread(16'000'000);
            continue;
        }
        if (!measured && g_phase.load() == 2) {
            measured = true;
            if (!g_game.empty()) MeasureShaders(c, own[2], width, height);
        }

        const int image = swapchain.Acquire();
        c.BeginWork(work++);
        DkCmdBuf cmd = c.cmd();
        c.BindDescriptors();
        c.InvalidateCodeIfNeeded();
        DkImageView color, z;
        dkImageViewDefaults(&color, &swapchain.Image(image));
        dkImageViewDefaults(&z, &depth.image);
        dkCmdBufBindRenderTarget(cmd, &color, &z);
        const bool done = g_phase.load() == 2;
        const bool failed = g_failed || !pattern_ok;
        const float progress = done ? 1.0f : float(g_done.load()) / float(std::max(1u, g_total.load()));
        DrawPattern(c, quad, width, height, progress, done && failed ? 1.0f : done ? 0.0f : 1.0f, done && failed ? 0.0f : 1.0f);
        c.EndWork();
        swapchain.Present(image);
        if (++frames % 600 == 0) {
            const dk::CoreStats st = c.TakeStats();
            dk::Log("[test] %llu frames in %.1f s; upload %llu KiB a work, commands max %llu KiB, overflows %llu",
                    (unsigned long long)frames, (dk::NowNs() - start) / 1e9,
                    (unsigned long long)(st.works ? st.upload_bytes / st.works >> 10 : 0),
                    (unsigned long long)(st.command_bytes_max >> 10), (unsigned long long)st.command_overflows);
            dk::FlushLog();
        }
    }

    threadWaitForExit(&worker);
    threadClose(&worker);
    swapchain.Destroy();
    c.Destroy();
    device.Destroy();
    dk::Log("[test] end");
    FlushFile();
    std::fclose(g_log);
    return 0;
}
