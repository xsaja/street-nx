// The title's Direct3D 8 calls drawn with deko3d, the Switch's own graphics API (renderer = "deko3d" in the
// settings). The same renderer as vulkan.cpp, call for call: the state, shader and texture logic is gfx_common.h, and
// this file records the GPU work. The SDK's HLSL goes through glslang to SPIR-V as for Vulkan (vk_shader.cpp, the same
// cache), then through SPIRV-Cross to GLSL (dk_glsl.cpp) and uam to DKSH, kept beside it. The core (device, queue,
// memory, work slots, swapchain) is dk_core.cpp, adapted from NFSMW-NX.
//
// Conventions (dk_core.h): origin upper left and depth 0..1, as Direct3D; clip-space y points up, as the SDK's shaders
// write it, so viewports are never flipped. Images are BGRA8 as the console's surfaces are.
//
// Synchronisation is coarse for now (stage D2): a fragment barrier with an image cache invalidation whenever the render
// targets change, and a full barrier after every copy or upload. The work is one deko3d work slot per frame, three in
// flight; a full upload ring ends the slot early.
#include <switch.h>
#include <deko3d.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <dxgiformat.h>

#include "app/app.h"
#include "core/log.h"
#include "dk_core.h"
#include "dk_glsl.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "host.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "uam_api.h"
#include "vk_shader.h"

namespace gpu {

namespace {

#include "gfx_common.h"
#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"

// the gate StopRendering closes at exit: every public entry holds it while it records, and does nothing once closed
std::mutex g_gate;
bool g_stopped = false;
#define DK_GATE()                  \
    std::lock_guard gate_(g_gate); \
    if (g_stopped) return

constexpr uint32_t kSlots = 3;
constexpr uint32_t kUploadMb = 32;   // per slot: vertices, indices, constants, texture staging
constexpr uint32_t kUboRange = 3072;  // b0 and b1: 192 vertex constants, DrawConstants under it
constexpr DkImageFormat kBgra = DkImageFormat_BGRA8_Unorm;
constexpr DkImageFormat kDepthFormat = DkImageFormat_Z24S8;
static_assert(sizeof(DrawConstants) <= kUboRange);
// bumped when the SPIR-V to GLSL conversion or uam changes: older DKSH files are not used
constexpr const char* kDkshTag = "dk1";

// the full screen passes, as vulkan.cpp's (its kBlitHlsl and kDepthHlsl)
const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

// SMAA 1x (third_party/smaa, MIT) over the game's frame before it is scaled to the screen: luma edges, blending
// weights, then the neighborhood blend. Every read linear and clamped, as SMAA asks; the sampler comes with each
// texture's handle, smaa_ls only names one. SMAA hands its textures to functions, which SPIR-V to GLSL cannot tie back
// to a binding: the texture parameters become unused ints and the reads go to the global of the same name
// (smaa_colorTex...), at the register each pass binds it to. The ones only unused functions name are never bound.
const char* kSmaaHlsl = R"(
cbuffer S : register(b1) { float4 smaa_rt; };
#define SMAA_RT_METRICS smaa_rt
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
SamplerState smaa_ls : register(s0);
Texture2D smaa_colorTex : register(t0);
Texture2D smaa_edgesTex : register(t0);
Texture2D smaa_areaTex : register(t1);
Texture2D smaa_searchTex : register(t2);
Texture2D smaa_blendTex : register(t1);
Texture2D smaa_tex : register(t3);
Texture2D smaa_depthTex : register(t3);
Texture2D smaa_predicationTex : register(t3);
Texture2D smaa_velocityTex : register(t3);
Texture2D smaa_currentColorTex : register(t3);
Texture2D smaa_previousColorTex : register(t3);
#define SMAATexture2D(tex) int tex##_unused
#define SMAATexturePass2D(tex) 0
#define SMAASampleLevelZero(tex, coord) smaa_##tex.SampleLevel(smaa_ls, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) smaa_##tex.SampleLevel(smaa_ls, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) smaa_##tex.SampleLevel(smaa_ls, coord, 0, offset)
#define SMAASample(tex, coord) smaa_##tex.SampleLevel(smaa_ls, coord, 0)
#define SMAASamplePoint(tex, coord) smaa_##tex.SampleLevel(smaa_ls, coord, 0)
#define SMAASampleOffset(tex, coord, offset) smaa_##tex.SampleLevel(smaa_ls, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)"
#include "../third_party/smaa/SMAA.hlsl.inc"
R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o0 : TEXCOORD2;
           float4 o1 : TEXCOORD3; float4 o2 : TEXCOORD4; };
V Full(uint id) {
    V o = (V)0;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
V edge_vs(uint id : SV_VertexID) {
    V o = Full(id);
    float4 off[3];
    SMAAEdgeDetectionVS(o.uv, off);
    o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2];
    return o;
}
float4 edge_ps(V i) : SV_Target {
    float4 off[3] = {i.o0, i.o1, i.o2};
    return float4(SMAALumaEdgeDetectionPS(i.uv, off, 0), 0, 0);
}
V weight_vs(uint id : SV_VertexID) {
    V o = Full(id);
    float4 off[3];
    SMAABlendingWeightCalculationVS(o.uv, o.pix, off);
    o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2];
    return o;
}
float4 weight_ps(V i) : SV_Target {
    float4 off[3] = {i.o0, i.o1, i.o2};
    return SMAABlendingWeightCalculationPS(i.uv, i.pix, off, 0, 0, 0, float4(0, 0, 0, 0));
}
V blend_vs(uint id : SV_VertexID) {
    V o = Full(id);
    float4 off;
    SMAANeighborhoodBlendingVS(o.uv, off);
    o.o0 = off;
    return o;
}
float4 blend_ps(V i) : SV_Target { return float4(SMAANeighborhoodBlendingPS(i.uv, i.o0, 0, 0).rgb, 1); }
)";

// ---- the console's values as deko3d's

DkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return DkBlendFactor_Zero;
        case 1: return DkBlendFactor_One;
        case 0x300: return alpha ? DkBlendFactor_SrcAlpha : DkBlendFactor_SrcColor;
        case 0x301: return alpha ? DkBlendFactor_InvSrcAlpha : DkBlendFactor_InvSrcColor;
        case 0x302: return DkBlendFactor_SrcAlpha;
        case 0x303: return DkBlendFactor_InvSrcAlpha;
        case 0x304: return DkBlendFactor_DstAlpha;
        case 0x305: return DkBlendFactor_InvDstAlpha;
        case 0x306: return alpha ? DkBlendFactor_DstAlpha : DkBlendFactor_DstColor;
        case 0x307: return alpha ? DkBlendFactor_InvDstAlpha : DkBlendFactor_InvDstColor;
        case 0x308: return alpha ? DkBlendFactor_One : DkBlendFactor_SrcAlphaSaturate;
        case 0x8001: return DkBlendFactor_ConstColor;
        case 0x8002: return DkBlendFactor_InvConstColor;
        case 0x8003: return DkBlendFactor_ConstAlpha;
        case 0x8004: return DkBlendFactor_InvConstAlpha;
        default: return DkBlendFactor_One;
    }
}

DkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return DkBlendOp_Sub;
        case 0x800B: return DkBlendOp_RevSub;
        case 0x8007: return DkBlendOp_Min;
        case 0x8008: return DkBlendOp_Max;
        default: return DkBlendOp_Add;
    }
}

// deko3d's compare ops are Vulkan's plus one
DkCompareOp Cmp(uint32_t v) { return DkCompareOp(CmpIndex(v) + 1); }

DkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return DkStencilOp_Zero;
        case 0x1E01: return DkStencilOp_Replace;
        case 0x1E02: return DkStencilOp_Incr;
        case 0x1E03: return DkStencilOp_Decr;
        case 0x150A: return DkStencilOp_Invert;
        case 0x8507: return DkStencilOp_IncrWrap;
        case 0x8508: return DkStencilOp_DecrWrap;
        default: return DkStencilOp_Keep;
    }
}

DkWrapMode Wrap(uint32_t v) {
    switch (v) {
        case 2: return DkWrapMode_MirroredRepeat;
        case 3: case 5: return DkWrapMode_ClampToEdge;
        case 4: return DkWrapMode_ClampToBorder;
        default: return DkWrapMode_Repeat;
    }
}

// the input assembler's formats the SDK's vertex.cpp names
void VertexFormat(uint32_t dxgi, DkVtxAttribState& a) {
    a.isBgra = 0;
    switch (dxgi) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: a.size = DkVtxAttribSize_4x32, a.type = DkVtxAttribType_Float; break;
        case DXGI_FORMAT_R32G32B32_FLOAT: a.size = DkVtxAttribSize_3x32, a.type = DkVtxAttribType_Float; break;
        case DXGI_FORMAT_R32G32_FLOAT: a.size = DkVtxAttribSize_2x32, a.type = DkVtxAttribType_Float; break;
        case DXGI_FORMAT_R32_FLOAT: a.size = DkVtxAttribSize_1x32, a.type = DkVtxAttribType_Float; break;
        case DXGI_FORMAT_R32_UINT: a.size = DkVtxAttribSize_1x32, a.type = DkVtxAttribType_Uint; break;
        case DXGI_FORMAT_R16G16B16A16_SNORM: a.size = DkVtxAttribSize_4x16, a.type = DkVtxAttribType_Snorm; break;
        case DXGI_FORMAT_R16G16B16A16_SINT: a.size = DkVtxAttribSize_4x16, a.type = DkVtxAttribType_Sint; break;
        case DXGI_FORMAT_R16G16_SNORM: a.size = DkVtxAttribSize_2x16, a.type = DkVtxAttribType_Snorm; break;
        case DXGI_FORMAT_R16G16_SINT: a.size = DkVtxAttribSize_2x16, a.type = DkVtxAttribType_Sint; break;
        case DXGI_FORMAT_R16_SNORM: a.size = DkVtxAttribSize_1x16, a.type = DkVtxAttribType_Snorm; break;
        case DXGI_FORMAT_R16_SINT: a.size = DkVtxAttribSize_1x16, a.type = DkVtxAttribType_Sint; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: a.size = DkVtxAttribSize_4x8, a.type = DkVtxAttribType_Unorm; break;
        case DXGI_FORMAT_R8G8_UNORM: a.size = DkVtxAttribSize_2x8, a.type = DkVtxAttribType_Unorm; break;
        case DXGI_FORMAT_R8_UNORM: a.size = DkVtxAttribSize_1x8, a.type = DkVtxAttribType_Unorm; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM: a.size = DkVtxAttribSize_4x8, a.type = DkVtxAttribType_Unorm, a.isBgra = 1; break;
        // STREET: the three-component ones vertex.cpp keeps raw for g_packed_strides (w reads as 1)
        case kFormatR16G16B16Snorm: a.size = DkVtxAttribSize_3x16, a.type = DkVtxAttribType_Snorm; break;
        case kFormatR16G16B16Sint: a.size = DkVtxAttribSize_3x16, a.type = DkVtxAttribType_Sint; break;
        case kFormatR8G8B8Unorm: a.size = DkVtxAttribSize_3x8, a.type = DkVtxAttribType_Unorm; break;
        default: a.size = DkVtxAttribSize_4x32, a.type = DkVtxAttribType_Float; break;
    }
}

DkPrimitive Primitive(const Assembled& a) {
    // STREET: what the Xbox drew natively and deko3d does too (vertex.cpp leaves them for g_native_polygons)
    switch (a.native_prim) {
        case 3: return DkPrimitive_LineLoop;
        case 7: return DkPrimitive_TriangleFan;
        case 8: return DkPrimitive_Quads;
        case 9: return DkPrimitive_QuadStrip;
        case 10: return DkPrimitive_Polygon;
        default: break;
    }
    Topology t = a.topology;
    bool strip = a.strip;
    switch (t) {
        case Topology::Points: return DkPrimitive_Points;
        case Topology::Lines: return strip ? DkPrimitive_LineStrip : DkPrimitive_Lines;
        default: return strip ? DkPrimitive_TriangleStrip : DkPrimitive_Triangles;
    }
}

// ---- objects

struct Image {
    DkImage image{};
    dk::ImageMemory mem;
    DkImageFormat fmt = DkImageFormat_None;
    DkImageType type = DkImageType_2D;
    uint32_t w = 1, h = 1, d = 1, layers = 1, levels = 1;
    bool depth = false, stencil = false;
    uint32_t desc = 0;  // its slot in the image descriptor table (sampled: every level), 0 = none
    // hazard tracking against the backend's barrier epoch: last written (by a draw, or by a copy or upload) and last
    // sampled; equal to the current epoch means since the last barrier
    uint64_t drawn = 0, copied = 0, sampled = 0;
    explicit operator bool() const { return mem.block != nullptr; }
};

// a console surface the renderer draws into (vulkan.cpp's Target)
struct Target {
    Image img;
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;
    uint64_t seen = 0;  // DirtyEpochNow at the last check
    bool drawn = false;
    std::vector<uint8_t> read;
    Image fb;  // copy read by draws that sample the target they render to
};

struct Tex {
    Image img;
    TextureDesc d;
    DkImageFormat fmt = DkImageFormat_None;
    bool decode = false;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t sampled = 0;  // MemSigSampled ^ epoch ^ palette as of the last whole check
    uint64_t frame = 0;
    uint64_t palette = 0;
};

struct Shader {
    DkShader shader{};
    bool ok = false;
};

using DrawShaders = DrawShadersT<const DkShader*>;
using DrawTextures = DrawTexturesT<uint32_t, DkImageFormat>;  // views as descriptor slots

struct Upload {
    DkGpuAddr gpu = 0;
    uint8_t* cpu = nullptr;
    explicit operator bool() const { return cpu != nullptr; }
};

// ---- the uam worker: uam is not reentrant and needs a big stack, so every compile runs on this one thread

class Compiler {
public:
    void Start() {
        if (R_FAILED(threadCreate(&thread_, Run, this, nullptr, 8u << 20, 0x3B, -2)) || R_FAILED(threadStart(&thread_)))
            LOGE("deko3d: the shader compile thread could not start");
        else
            started_ = true;
    }
    // GLSL to DKSH on the worker, waiting for it; empty on failure (the reason logged)
    std::vector<uint8_t> Compile(bool vertex, std::string glsl, const char* name) {
        if (!started_) return {};
        std::unique_lock lock(m_);
        Job job{vertex, std::move(glsl), {}, false};
        jobs_.push_back(&job);
        cv_.notify_all();
        done_cv_.wait(lock, [&] { return job.done; });
        if (job.dksh.empty()) LOGE("deko3d: uam failed on %s: %s", name, job.log.c_str());
        return std::move(job.dksh);
    }

private:
    struct Job {
        bool vertex;
        std::string glsl;
        std::vector<uint8_t> dksh;
        bool done;
        std::string log;
    };
    static void Run(void* self) { static_cast<Compiler*>(self)->Loop(); }
    void Loop() {
        uam::init(true);
        for (;;) {
            Job* job;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [&] { return !jobs_.empty(); });
                job = jobs_.front();
                jobs_.pop_front();
            }
            uam::Result r = uam::compile(job->vertex ? uam::Stage::Vertex : uam::Stage::Fragment, job->glsl.c_str());
            std::lock_guard lock(m_);
            job->dksh = r.ok ? std::move(r.dksh) : std::vector<uint8_t>{};
            job->log = std::move(r.log);
            job->done = true;
            done_cv_.notify_all();
        }
    }
    Thread thread_{};
    bool started_ = false;
    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<Job*> jobs_;
};

class DekoBackend final : public Backend {
public:
    ~DekoBackend() override = default;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override {
        DK_GATE();
        Submit(true);
    }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
    }

private:
    struct Late {
        DkMemBlock rb[kSlots] = {};
        Image down;
        uint64_t work[kSlots] = {};
        uint64_t frame[kSlots] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0, gh = 0, next = 0;
    };

    DkCmdBuf Cb() { return core_.cmd(); }

    // memory and objects
    bool CreateImage(Image& im, DkImageFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers, uint32_t levels,
                     bool render, DkImageType type = DkImageType_2D);
    void Retire(Image& im);
    void View(const Image& im, DkImageView& v, bool attach = false) const;
    uint32_t NewDescriptor(const Image& im);
    uint32_t SamplerSlot(const DkSampler& s);
    Upload Reserve(uint32_t size, uint32_t align = 16);
    Upload Constants(const void* data, size_t bytes);
    struct LastConstants {
        uint64_t gen = 0;
        size_t bytes = 0;
        Upload at{};
        std::vector<uint8_t> data;
    } last_vsc_, last_dc_;
    Upload ConstantsReused(LastConstants& last, const void* data, size_t bytes);

    // command recording
    void BeginWork();
    void Submit(bool wait);
    uint64_t Completed();
    void Hazard(bool full);
    void BeginRender(Image* color, Image* depth);
    void SetViewport(float x, float y, float w, float h);
    void SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    void Barrier();
    void Sample(Image& im);
    void BeforeCopy(Image* src, Image* dst);
    void AfterCopy(Image& dst);

    // shaders
    const DkShader* Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash);
    const DkShader* LoadDksh(uint64_t hash, const std::vector<uint32_t>& spirv, vkshader::Stage stage);
    void Precompile();

    // targets
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    void UploadTarget(Target& t, const Surface& s);
    bool PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    void Blit(Image& dst, int32_t x, int32_t y, uint32_t w, uint32_t h, Image& src, const float uvrect[4],
              const BlitOptions& o);
    Image* Feedback(Target& t);
    bool DepthCopy(const State& s);
    bool Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps = 1);
    Image* CopySource(Target& t, Image& down);
    void CopyToBuffer(Image& im, uint32_t w, uint32_t h, DkMemBlock block);
    bool ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

    // draws
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    bool BindStageTextures(const State& s, DrawTextures& stages);
    uint32_t BindTexture(const State& s, int stage, DrawTextures& stages);
    uint32_t BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box, DrawTextures& stages);
    uint32_t BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours);
    uint32_t StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp);
    uint32_t StageSamplerMake(const State& s, int i, const DrawTextures& stages, bool clamp);
    void BindState(const State& s, const Assembled& a, const DrawShaders& sh);
    void ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color,
                     float z, uint32_t stencil);

    // present
    void EnsureSwapchain();
    void DrawFrame(Image* src, int image);
    Image* Smaa(Image& src);
    void SmaaShaders();
    bool SmaaPass(int pass, Image& dst, Image* const* tex, uint32_t n, const float rt[4]);
    void UploadGammaRamp();
    void ReleaseOldTextures();
    void ApplySettings();
    void ReadTiming(uint32_t slot);
    void SaveShot(Target& t, Image* shown);
    void SyncLocked(uint32_t data);
    void LogPerf();

    dk::Device device_;
    dk::Core core_;
    dk::Swapchain swapchain_;
    Compiler compiler_;
    uint32_t swap_w_ = 0, swap_h_ = 0;
    bool vsync_ = true;

    uint64_t work_ = 0;  // the work slot being recorded
    uint64_t completed_ = 0;
    std::vector<Image> retired_[kSlots];
    std::vector<uint32_t> retired_desc_[kSlots];
    uint64_t upload_gen_ = 1;

    // descriptors: image slots handed out and freed a frame late, samplers kept for good
    std::vector<uint32_t> free_desc_;
    uint32_t next_desc_ = 1;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t next_sampler_ = 0;
    bool desc_dirty_ = false;
    // hazard tracking (Hazard, Barrier): the barrier epoch, what the next draw needs, the images it samples
    uint64_t epoch_ = 1;
    bool need_full_ = false, need_frag_ = false;
    Image* draw_samples_[4] = {};
    uint32_t draw_samples_n_ = 0;
    uint64_t barriers_full_ = 0, barriers_frag_ = 0;
    Image* stage_img_[4] = {};  // the images the stages being bound sample
    // the last bind of each stage: the same texture header, targets, frame and images give the same binding
    struct StageMemo {
        bool valid = false;
        uint32_t header = 0, palette = 0, words[5] = {};
        uint64_t frame = 0, dirty = 0, gen = 0;
        const Target *rt = nullptr, *ds = nullptr;
        uint32_t view = 0, levels = 1;
        TexInfo info;
        DkImageFormat fmt{};
        bool is_rt = false, blur = false;
        float k = 1, wide = 0;
        Image* img = nullptr;
    };
    StageMemo stage_memo_[4];
    bool stage_uncached_ = false;  // the bind read a palette or a feedback copy: not remembered
    uint64_t bind_gen_ = 1;        // bumped when an image is made or retired
    struct SamplerMemo {
        bool valid = false, clamp = false;
        std::array<uint32_t, kTssCount> tss{};
        uint32_t levels = 0, func = 0, aniso = 0, slot = 0;
        uint8_t shadow = 0;
    };
    SamplerMemo sampler_memo_[4];
    uint32_t linear_ = 0, point_ = 0;

    // the bound render targets, and whether the recorded state is still there
    Image* rend_color_ = nullptr;
    Image* rend_depth_ = nullptr;
    bool rendering_ = false;

    std::unordered_map<uint64_t, Shader> shaders_;  // by the HLSL's hash
    std::unordered_map<uint64_t, uint64_t> shader_src_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Tex> textures_;
    std::filesystem::path cache_dir_;
    Image lut_;
    Image dummy_[3];  // 2D, 3D, cube
    // SMAA (host::Smaa): its targets at the game frame's scaled size, its two lookup tables, its three shader pairs
    Image smaa_edges_, smaa_blend_, smaa_out_, smaa_area_, smaa_search_;
    const DkShader* smaa_shaders_[6] = {};
    bool smaa_failed_ = false;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* draw_rt_ = nullptr;
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1.0f;
    Assembled asm_;
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    uint64_t presents_ = 0;

    // STREET's perf lines, as vulkan.cpp's
    enum Phase { kPhAssemble, kPhShaders, kPhPipeline, kPhUpload, kPhSets, kPhRecord, kPhAcquire, kPhReset, kPhPresent,
                 kPhTargets, kPhTextures, kPhConstants, kPhases };
    uint64_t phase_ns_[kPhases] = {};
    uint64_t const_reused_ = 0, const_uploaded_ = 0, upload_bytes_ = 0;
    uint64_t stage_memo_hits_ = 0, stage_memo_misses_ = 0;
    uint64_t sig_bytes_ = 0, tex_uploads_ = 0, tex_upload_bytes_ = 0;
    uint32_t check_frames_ = 1;
    uint64_t gpu_ns_ = 0, gpu_works_ = 0;
    // frame pacing: time between presents; with vsync over 18 ms is a missed refresh the player sees as a hitch
    uint64_t last_present_ns_ = 0, pace_max_ns_ = 0, last_acquire_ns_ = 0, last_reset_ns_ = 0;
    uint32_t pace_late_[3] = {};  // over 18, 25 and 34 ms
    bool timed_[kSlots] = {};
    uint64_t compiles_ = 0, compile_ns_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
};

uint64_t TickNs() { return armTicksToNs(armGetSystemTick()); }

// ---- Init

bool DekoBackend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    dk::SetLog([](const char* line) { LOGR("%s", line); }, [] { logx::Flush(); });
    cache_dir_ = app::ExeDir() / "shadercache";
    vkshader::Init(cache_dir_);
    compiler_.Start();
    gpu::g_native_polygons = true;  // fans, quads and polygons drawn as they are, without a rebuilt triangle list
    gpu::g_packed_strides = true;   // 6 and 3 byte vertices read as they are, not converted on the CPU
    device_.Create(false);  // ZCULL off until checked (NFSMW-NX's item 6)
    dk::CoreConfig config;
    config.slots = kSlots;
    config.upload_mb = kUploadMb;
    config.upload_cached = host::DekoCachedUpload();
    check_frames_ = host::TextureCheckFrames();
    config.command_kb = 2048;
    core_.Create(device_, config);
    free_desc_.clear();
    // the fixed images: the gamma ramp and the black dummies
    if (!CreateImage(lut_, kBgra, 256, 1, 1, 1, 1, false) || !CreateImage(dummy_[0], kBgra, 1, 1, 1, 1, 1, false) ||
        !CreateImage(dummy_[1], kBgra, 1, 1, 1, 1, 1, false, DkImageType_3D) ||
        !CreateImage(dummy_[2], kBgra, 1, 1, 1, 6, 1, false, DkImageType_Cubemap))
        return false;
    DkSampler sc;
    dkSamplerDefaults(&sc);
    sc.minFilter = sc.magFilter = DkFilter_Linear;
    sc.mipFilter = DkMipFilter_Linear;
    sc.wrapMode[0] = sc.wrapMode[1] = sc.wrapMode[2] = DkWrapMode_ClampToEdge;
    linear_ = SamplerSlot(sc);
    sc.minFilter = sc.magFilter = DkFilter_Nearest;
    sc.mipFilter = DkMipFilter_Nearest;
    point_ = SamplerSlot(sc);
    if (!core_.WorkOpen()) BeginWork();
    std::vector<uint8_t> black(6 * 4, 0);
    for (Image* im : {&dummy_[0], &dummy_[1], &dummy_[2]}) {
        Upload u = Reserve(uint32_t(im->layers) * 4, 16);
        std::memset(u.cpu, 0, im->layers * 4);
        DkImageView v;
        View(*im, v);
        if (im->type == DkImageType_Cubemap) v.type = DkImageType_2DArray;
        const DkCopyBuf src = {u.gpu, 0, 0};
        const DkImageRect r = {0, 0, 0, 1, 1, im->layers};
        dkCmdBufCopyBufferToImage(Cb(), &src, &v, &r, 0);
    }
    for (int i = 0; i < 768; i++) ramp_[i] = uint8_t(i & 255);
    UploadGammaRamp();
    Hazard(true);
    Submit(true);
    Precompile();
    if (host::Smaa()) SmaaShaders();
    LOGR("deko3d: ready (guest %ux%u)", guest_w_, guest_h_);
    return true;
}

// the DKSH of every cached SPIR-V module that has none yet, at start: the shaders the game used before, so their first
// draw this session does not wait for uam (31 shaders in about 2.5 s on the console)
void DekoBackend::Precompile() {
    std::error_code ec;
    std::vector<std::filesystem::path> spirv;
    for (const auto& e : std::filesystem::directory_iterator(cache_dir_, ec))
        if (e.path().extension() == ".spv") spirv.push_back(e.path());
    uint32_t made = 0, failed = 0;
    uint64_t t0 = TickNs();
    for (const auto& p : spirv) {
        std::filesystem::path out = p;
        out.replace_extension(std::string(".") + kDkshTag);
        if (std::filesystem::exists(out, ec)) continue;
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        std::vector<uint32_t> words(size_t(f.tellg()) / 4);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(words.data()), std::streamsize(words.size() * 4));
        dkglsl::Glsl g = dkglsl::FromSpirv(words);
        std::vector<uint8_t> dksh;
        if (!g.text.empty()) dksh = compiler_.Compile(g.stage == dkglsl::Stage::Vertex, std::move(g.text), p.stem().string().c_str());
        if (dksh.empty()) {
            failed++;
            continue;
        }
        std::ofstream o(out, std::ios::binary);
        o.write(reinterpret_cast<const char*>(dksh.data()), std::streamsize(dksh.size()));
        made++;
    }
    LOGR("deko3d: %zu cached SPIR-V modules, %u DKSH made at start (%u failed) in %.2f s", spirv.size(), made, failed,
         (TickNs() - t0) / 1e9);
}

// ---- memory and objects

bool DekoBackend::CreateImage(Image& im, DkImageFormat fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t layers,
                              uint32_t levels, bool render, DkImageType type) {
    bind_gen_++;
    im = {};
    im.fmt = fmt;
    im.w = w;
    im.h = h;
    im.d = d;
    im.layers = layers;
    im.levels = levels;
    im.type = type;
    im.depth = fmt == kDepthFormat;
    im.stencil = im.depth;
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, device_.device());
    m.type = type;
    m.flags = render ? DkImageFlags_UsageRender : 0;
    m.format = fmt;
    m.dimensions[0] = w;
    m.dimensions[1] = h;
    m.dimensions[2] = type == DkImageType_3D ? d : type == DkImageType_Cubemap ? 6 : 1;
    m.mipLevels = levels;
    // compressed formats count rows of 4x4 blocks
    bool bc = fmt == DkImageFormat_RGBA_BC1 || fmt == DkImageFormat_RGBA_BC2 || fmt == DkImageFormat_RGBA_BC3;
    dk::FixTileSize(m, bc ? (h + 3) / 4 : h);
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    im.mem = core_.ReserveImage(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    if (!im.mem) return false;
    dkImageInitialize(&im.image, &layout, im.mem.block, im.mem.offset);
    im.desc = NewDescriptor(im);
    return true;
}

void DekoBackend::View(const Image& im, DkImageView& v, bool attach) const {
    dkImageViewDefaults(&v, &im.image);
    if (attach) {
        v.type = DkImageType_2D;
        v.mipLevelCount = 1;
    }
}

// a descriptor slot for its sampled view, written in order with the commands
uint32_t DekoBackend::NewDescriptor(const Image& im) {
    uint32_t slot;
    if (!free_desc_.empty()) {
        slot = free_desc_.back();
        free_desc_.pop_back();
    } else {
        slot = next_desc_++;
    }
    if (slot >= core_.config().image_descriptors) {
        LOGE("deko3d: out of image descriptors");
        return 0;
    }
    DkImageView v;
    View(im, v);
    DkImageDescriptor d;
    dkImageDescriptorInitialize(&d, &v, false, false);
    if (!core_.WorkOpen()) BeginWork();
    dkCmdBufPushData(Cb(), core_.ImageDescriptors() + DkGpuAddr(slot) * sizeof(d), &d, sizeof(d));
    desc_dirty_ = true;
    return slot;
}

uint32_t DekoBackend::SamplerSlot(const DkSampler& s) {
    uint64_t key = Hash(&s, sizeof(s));
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    uint32_t slot = next_sampler_++;
    if (slot >= core_.config().sampler_descriptors) {
        LOGE("deko3d: out of sampler descriptors");
        slot = 0;
    } else {
        DkSamplerDescriptor d;
        dkSamplerDescriptorInitialize(&d, &s);
        if (!core_.WorkOpen()) BeginWork();
        dkCmdBufPushData(Cb(), core_.SamplerDescriptors() + DkGpuAddr(slot) * sizeof(d), &d, sizeof(d));
        desc_dirty_ = true;
    }
    samplers_[key] = slot;
    return slot;
}

// gone once the GPU is past the slot being recorded: its memory and its descriptor
void DekoBackend::Retire(Image& im) {
    if (!im) return;
    bind_gen_++;
    if (rend_color_ == &im || rend_depth_ == &im) rendering_ = false, rend_color_ = rend_depth_ = nullptr;
    core_.ReleaseImageLater(im.mem);
    if (im.desc) retired_desc_[work_ % kSlots].push_back(im.desc);
    im = {};
}

// bytes in this slot's upload ring; a full ring ends the slot and the next one starts
Upload DekoBackend::Reserve(uint32_t size, uint32_t align) {
    if (!core_.WorkOpen()) BeginWork();
    dk::Upload u = core_.Reserve(size, align);
    if (!u) {
        Submit(false);
        u = core_.Reserve(size, align);
        if (!u) {
            LOGE("deko3d: an upload of %u bytes is bigger than the ring", size);
            return {};
        }
    }
    return {u.gpu, u.cpu};
}

Upload DekoBackend::Constants(const void* data, size_t bytes) {
    Upload u = Reserve(kUboRange, DK_UNIFORM_BUF_ALIGNMENT);
    if (u && data) std::memcpy(u.cpu, data, std::min<size_t>(bytes, kUboRange));
    return u;
}

Upload DekoBackend::ConstantsReused(LastConstants& last, const void* data, size_t bytes) {
    if (gpu::g_reuse_constants.load(std::memory_order_relaxed) && last.gen == upload_gen_ && last.bytes == bytes &&
        !std::memcmp(last.data.data(), data, bytes)) {
        const_reused_++;
        return last.at;
    }
    Upload u = Constants(data, bytes);
    const_uploaded_++;
    upload_bytes_ += bytes;
    last.gen = upload_gen_;
    last.bytes = bytes;
    last.at = u;
    last.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
    return u;
}

// ---- command recording

void DekoBackend::BeginWork() {
    work_++;
    uint32_t slot = uint32_t(work_ % kSlots);
    core_.BeginWork(work_);  // waits for the slot's last work
    ReadTiming(slot);
    if (!retired_desc_[slot].empty()) {
        free_desc_.insert(free_desc_.end(), retired_desc_[slot].begin(), retired_desc_[slot].end());
        retired_desc_[slot].clear();
    }
    upload_gen_++;
    rendering_ = false;
    rend_color_ = rend_depth_ = nullptr;
    core_.BindDescriptors();
    // the slot's GPU time: a timestamp at its start, one at its end (Submit)
    dkCmdBufReportCounter(Cb(), DkCounter_Timestamp, core_.QueriesGpu() + slot * 32);
    timed_[slot] = true;
}

void DekoBackend::ReadTiming(uint32_t slot) {
    if (!timed_[slot] || work_ <= kSlots) return;
    timed_[slot] = false;
    uint64_t t0, t1;
    std::memcpy(&t0, core_.QueriesCpu() + slot * 32 + 8, 8);
    std::memcpy(&t1, core_.QueriesCpu() + slot * 32 + 24, 8);
    if (t1 > t0) {
        gpu_ns_ += uint64_t(double(t1 - t0) * dk::kNsPerGpuUnit);
        gpu_works_++;
    }
}

// ends the slot (wait: and waits for the GPU), the next one starts
void DekoBackend::Submit(bool wait) {
    if (!core_.WorkOpen()) BeginWork();
    uint32_t slot = uint32_t(work_ % kSlots);
    dkCmdBufReportCounter(Cb(), DkCounter_Timestamp, core_.QueriesGpu() + slot * 32 + 16);
    core_.EndWork();
    rendering_ = false;
    if (wait) {
        core_.WaitGpu();
        completed_ = work_;
    }
    BeginWork();
}

// the highest work the GPU has finished
uint64_t DekoBackend::Completed() {
    while (completed_ + 1 < work_) {
        uint64_t w = completed_ + 1;
        if (w + kSlots > work_ && !core_.WorkDone(w)) break;
        completed_ = w;
    }
    return completed_;
}

// A barrier now: what was recorded before is done (fragments; full: every engine, the copies too) and the image cache
// dropped, the descriptor cache too when descriptors were written (that needs every engine idle). A new epoch starts.
void DekoBackend::Hazard(bool full) {
    full |= desc_dirty_;
    dkCmdBufBarrier(Cb(), full ? DkBarrier_Full : DkBarrier_Fragments,
                    DkInvalidateFlags_Image | (desc_dirty_ ? DkInvalidateFlags_Descriptors : 0));
    desc_dirty_ = false;
    need_full_ = need_frag_ = false;
    epoch_++;
    (full ? barriers_full_ : barriers_frag_)++;
}

// a draw about to run will sample this image: what wrote it since the last barrier must be done first
void DekoBackend::Sample(Image& im) {
    if (im.copied == epoch_) need_full_ = true;
    else if (im.drawn == epoch_) need_frag_ = true;
    if (draw_samples_n_ < std::size(draw_samples_)) draw_samples_[draw_samples_n_++] = &im;
}

// before every draw, clear or blit: the shader caches if code was loaded, one barrier for every hazard noted (what it
// samples written since the last barrier; its targets sampled or copied into since then), then the epochs marked
void DekoBackend::Barrier() {
    core_.InvalidateCodeIfNeeded();
    for (Image* t : {rend_color_, rend_depth_}) {
        if (!t) continue;
        if (t->copied == epoch_) need_full_ = true;
        else if (t->sampled == epoch_) need_frag_ = true;
    }
    if (need_full_ || desc_dirty_) Hazard(true);
    else if (need_frag_) Hazard(false);
    for (uint32_t i = 0; i < draw_samples_n_; i++) draw_samples_[i]->sampled = epoch_;
    draw_samples_n_ = 0;
    if (rend_color_) rend_color_->drawn = epoch_;
    if (rend_depth_) rend_depth_->drawn = epoch_;
}

// around the copy engine (uploads, copies, read-backs): a full barrier first when src was written, or dst written or
// sampled, since the last one; afterwards dst counts as copied into
void DekoBackend::BeforeCopy(Image* src, Image* dst) {
    if (!core_.WorkOpen()) BeginWork();
    bool hazard = src && (src->drawn == epoch_ || src->copied == epoch_);
    hazard |= dst && (dst->drawn == epoch_ || dst->copied == epoch_ || dst->sampled == epoch_);
    if (hazard) Hazard(true);
}

void DekoBackend::AfterCopy(Image& dst) { dst.copied = epoch_; }

// the render targets: binding them is free, Barrier orders what they need
void DekoBackend::BeginRender(Image* color, Image* depth) {
    if (!core_.WorkOpen()) BeginWork();
    if (rendering_ && color == rend_color_ && depth == rend_depth_) return;
    DkImageView cv, dv;
    if (color) View(*color, cv, true);
    if (depth) View(*depth, dv, true);
    const DkImageView* colors[] = {&cv};
    dkCmdBufBindRenderTargets(Cb(), colors, color ? 1 : 0, depth ? &dv : nullptr);
    rendering_ = true;
    rend_color_ = color;
    rend_depth_ = depth;
}

void DekoBackend::SetViewport(float x, float y, float w, float h) {
    const DkViewport vp = {x, y, w, h, 0, 1};
    dkCmdBufSetViewports(Cb(), 0, &vp, 1);
}

void DekoBackend::SetScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    const DkScissor sc = {uint32_t(x0), uint32_t(y0), uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))};
    dkCmdBufSetScissors(Cb(), 0, &sc, 1);
}

// ---- shaders: the HLSL to SPIR-V as Vulkan's (and its cache), then GLSL and DKSH (cached beside the SPIR-V)

const DkShader* DekoBackend::LoadDksh(uint64_t hash, const std::vector<uint32_t>& spirv, vkshader::Stage stage) {
    char name[48];
    std::snprintf(name, sizeof(name), "%016llx_2.%s", (unsigned long long)hash, kDkshTag);
    std::filesystem::path path = cache_dir_ / name;
    std::vector<uint8_t> dksh;
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (f && f.tellg() > 0) {
            dksh.resize(size_t(f.tellg()));
            f.seekg(0);
            f.read(reinterpret_cast<char*>(dksh.data()), std::streamsize(dksh.size()));
        }
    }
    if (dksh.empty()) {
        uint64_t t0 = TickNs();
        dkglsl::Glsl g = dkglsl::FromSpirv(spirv);
        if (g.text.empty()) {
            LOGE("deko3d: %s does not convert to GLSL: %s", name, g.error.c_str());
            return nullptr;
        }
        dksh = compiler_.Compile(stage == vkshader::Stage::Vertex, std::move(g.text), name);
        compiles_++;
        compile_ns_ += TickNs() - t0;
        if (dksh.empty()) return nullptr;
        std::ofstream o(path, std::ios::binary);
        o.write(reinterpret_cast<const char*>(dksh.data()), std::streamsize(dksh.size()));
    }
    Shader& sh = shaders_[hash];
    sh.ok = core_.LoadShader(sh.shader, dksh, name);
    return sh.ok ? &sh.shader : nullptr;
}

const DkShader* DekoBackend::Module(const std::string& src, const char* entry, vkshader::Stage stage, uint64_t& hash) {
    uint64_t seed = Hash(entry, std::strlen(entry), stage == vkshader::Stage::Vertex ? 1 : 2);
    hash = Hash(src.data(), src.size(), seed);
    if (auto it = shaders_.find(hash); it != shaders_.end()) return it->second.ok ? &it->second.shader : nullptr;
    std::vector<uint32_t> spirv;
    if (!vkshader::Compile(src, entry, stage, hash, spirv)) {
        shaders_[hash] = {};
        return nullptr;
    }
    const DkShader* sh = LoadDksh(hash, spirv, stage);
    if (!sh) shaders_[hash] = {};
    return sh;
}

void DekoBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.vkey); it != shader_src_.end()) {
        sh.vh = it->second;
        auto m = shaders_.find(sh.vh);
        sh.vs = m != shaders_.end() && m->second.ok ? &m->second.shader : nullptr;
        return;
    }
    std::string src = VertexHlsl(s, a, sh);
    const char* entry = "main";
    if (a.topology == Topology::Points) {
        std::string points = PointsHlsl(src);
        if (!points.empty()) {
            src = std::move(points);
            entry = "main_points";
        }
    }
    sh.vs = Module(src, entry, vkshader::Stage::Vertex, sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

void DekoBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    if (auto it = shader_src_.find(sh.pkey ^ kPixelKeySalt); it != shader_src_.end()) {
        sh.ph = it->second;
        auto m = shaders_.find(sh.ph);
        sh.ps = m != shaders_.end() && m->second.ok ? &m->second.shader : nullptr;
        return;
    }
    sh.ps = Module(PixelHlsl(pk), "main", vkshader::Stage::Pixel, sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

// ---- samplers

DkSampler BorderSampler(DkSampler s, uint32_t argb) {
    float v = 0, a = 0;
    if ((argb >> 24) >= 128) {
        uint32_t sum = ((argb >> 16) & 255) + ((argb >> 8) & 255) + (argb & 255);
        v = sum >= 384 ? 1.0f : 0.0f;
        a = 1.0f;
    }
    for (int i = 0; i < 3; i++) s.borderColor[i].value_f = v;
    s.borderColor[3].value_f = a;
    return s;
}

uint32_t DekoBackend::StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp) {
    SamplerMemo& m = sampler_memo_[i];
    const auto& tss = s.tss[size_t(i)];
    uint32_t levels = stages.levels[i], func = s.rs[kRsShadowFunc], aniso_set = app::Get().aniso;
    uint8_t shadow = stages.info[i].shadow;
    if (m.valid && m.clamp == clamp && m.levels == levels && m.func == func && m.aniso == aniso_set &&
        m.shadow == shadow && m.tss == tss)
        return m.slot;
    m.valid = true;
    m.clamp = clamp;
    m.levels = levels;
    m.func = func;
    m.aniso = aniso_set;
    m.shadow = shadow;
    m.tss = tss;
    m.slot = StageSamplerMake(s, i, stages, clamp);
    return m.slot;
}

uint32_t DekoBackend::StageSamplerMake(const State& s, int i, const DrawTextures& stages, bool clamp) {
    DkSampler sd;
    dkSamplerDefaults(&sd);
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    sd.minFilter = lin(min) ? DkFilter_Linear : DkFilter_Nearest;
    sd.magFilter = lin(mag) ? DkFilter_Linear : DkFilter_Nearest;
    sd.mipFilter = mip == kTexfLinear ? DkMipFilter_Linear : DkMipFilter_Nearest;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    uint32_t aniso = app::Get().aniso;
    bool mipped = mip && stages.levels[i] > 1;
    if (aniso && lin(min) && mipped && !stages.info[i].shadow) an = aniso;
    sd.maxAnisotropy = float(std::min<uint32_t>(an, 16));
    uint32_t u = t[kTssAddressU], v = t[kTssAddressV];
    sd.wrapMode[0] = Wrap(clamp && u == kTaddressWrap ? kTaddressClamp : u);
    sd.wrapMode[1] = Wrap(clamp && v == kTaddressWrap ? kTaddressClamp : v);
    sd.wrapMode[2] = Wrap(t[kTssAddressW]);
    sd.lodBias = std::clamp(Bits(t[kTssMipLodBias]), -15.0f, 15.0f);
    sd.lodClampMin = std::min(float(t[kTssMaxMipLevel]), float(stages.levels[i] - 1));
    sd.lodClampMax = mip ? 1000.0f : sd.lodClampMin;
    sd = BorderSampler(sd, t[kTssBorderColor]);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, the GPU r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.minFilter = sd.magFilter = (lin(min) || lin(mag)) ? DkFilter_Linear : DkFilter_Nearest;
        sd.mipFilter = DkMipFilter_Nearest;
        sd.compareEnable = true;
        sd.compareOp = DkCompareOp(swap[CmpIndex(s.rs[kRsShadowFunc])] + 1);
        sd.maxAnisotropy = 1;
    }
    return SamplerSlot(sd);
}

// ---- targets

Target* DekoBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    auto it = targets_.find(TargetMapKey(data, depth));
    if (it == targets_.end() || it->second.gw != gw || it->second.gh != gh) return nullptr;
    return &it->second;
}

void DekoBackend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (draw_rt_ == &it->second) draw_rt_ = nullptr;
    if (draw_ds_ == &it->second) draw_ds_ = nullptr;
    Retire(it->second.img);
    Retire(it->second.fb);
    targets_.erase(it);
}

Target* DekoBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint32_t data = mem::VirtualFromPhysical(s.data), gw, gh;
    SurfaceSize(s, gw, gh);
    uint64_t key = TargetMapKey(data, depth);
    auto it = targets_.find(key);
    if (it != targets_.end() && it->second.gw == gw && it->second.gh == gh) return &it->second;
    if (it != targets_.end()) ReleaseTarget(it);
    Target t;
    t.depth = depth;
    t.gw = gw;
    t.gh = gh;
    t.w = std::max(1u, uint32_t(std::lround(gw * scale_)));
    t.h = std::max(1u, uint32_t(std::lround(gh * scale_)));
    if (!CreateImage(t.img, depth ? kDepthFormat : kBgra, t.w, t.h, 1, 1, 1, true)) {
        LOGE("deko3d: no %ux%u target", t.w, t.h);
        return nullptr;
    }
    t.data = data;
    t.bytes = depth ? gw * gh * 4 : GuestPitch(gw) * gh;
    t.epoch = DirtyEpoch(data);
    t.sig = MemSig(data, t.bytes);
    Target& ref = targets_[key];
    ref = std::move(t);
    // a new target starts cleared
    BeginRender(depth ? nullptr : &ref.img, depth ? &ref.img : nullptr);
    Barrier();
    SetScissor(0, 0, int32_t(ref.w), int32_t(ref.h));
    if (depth) dkCmdBufClearDepthStencil(Cb(), true, 1.0f, 0xFF, 0);
    else dkCmdBufClearColorFloat(Cb(), 0, DkColorMask_RGBA, 0, 0, 0, 0);
    return &ref;
}

// BGRA pixels into a new w x h image
bool DekoBackend::PixelImage(Image& im, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    if (!CreateImage(im, kBgra, w, h, 1, 1, 1, false)) return false;
    Upload u = Reserve(w * h * 4, 256);
    if (!u) return false;
    for (uint32_t y = 0; y < h; y++) std::memcpy(u.cpu + size_t(y) * w * 4, &px[size_t(y) * pitch], w * 4);
    BeforeCopy(nullptr, &im);
    DkImageView v;
    View(im, v);
    const DkCopyBuf src = {u.gpu, 0, 0};
    const DkImageRect r = {0, 0, 0, w, h, 1};
    dkCmdBufCopyBufferToImage(Cb(), &src, &v, &r, 0);
    AfterCopy(im);
    return true;
}

void DekoBackend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!DescribeSurface(s, d) && !Describe(s.header, d)) return;
    if (Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    Image tex;
    if (!PixelImage(tex, px, pitch, d.width, d.height)) return;
    float uv[4] = {0, 0, 1, 1};
    Blit(t.img, 0, 0, t.w, t.h, tex, uv, {.alpha = true, .over = over});
    Retire(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// src over a rectangle of dst through the blit shader
void DekoBackend::Blit(Image& dst, int32_t x, int32_t y, uint32_t w, uint32_t h, Image& src, const float uvrect[4],
                       const BlitOptions& o) {
    uint64_t hv, hp;
    const DkShader* vs = Module(kBlitHlsl, "vs", vkshader::Stage::Vertex, hv);
    const DkShader* ps = Module(kBlitHlsl, "psmain", vkshader::Stage::Pixel, hp);
    if (!vs || !ps) return;
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), o.mul};
    Upload cb = Constants(c, sizeof(c));
    BeginRender(&dst, nullptr);
    Sample(src);
    Sample(lut_);
    Barrier();
    DkCmdBuf cmd = Cb();
    const DkShader* pair[] = {vs, ps};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 1, cb.gpu, kUboRange);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, cb.gpu, kUboRange);
    const DkResHandle tex[2] = {dkMakeTextureHandle(src.desc, linear_), dkMakeTextureHandle(lut_.desc, point_)};
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, tex, 2);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(cmd, &rs);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = ds.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(cmd, &ds);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = o.over ? 1 : 0;
    dkCmdBufBindColorState(cmd, &cs);
    if (o.over) {
        DkBlendState bs;
        dkBlendStateDefaults(&bs);
        bs.colorBlendOp = bs.alphaBlendOp = DkBlendOp_Add;
        bs.srcColorBlendFactor = DkBlendFactor_One;
        bs.dstColorBlendFactor = DkBlendFactor_InvSrcAlpha;
        bs.srcAlphaBlendFactor = DkBlendFactor_Zero;
        bs.dstAlphaBlendFactor = DkBlendFactor_One;
        dkCmdBufBindBlendState(cmd, 0, &bs);
    }
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(cmd, &cw);
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
    SetViewport(float(x), float(y), float(w), float(h));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
}

Image* DekoBackend::Feedback(Target& t) {
    if (!t.fb && !CreateImage(t.fb, t.img.fmt, t.w, t.h, 1, 1, 1, false)) return nullptr;
    BeforeCopy(&t.img, &t.fb);
    DkImageView sv, dv;
    View(t.img, sv, true);
    View(t.fb, dv, true);
    const DkImageRect r = {0, 0, 0, t.w, t.h, 1};
    dkCmdBufCopyImage(Cb(), &sv, &r, &dv, &r, 0);
    AfterCopy(t.fb);
    return &t.fb;
}

bool DekoBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1)
        return false;
    auto di = targets_.find(TargetMapKey(mem::VirtualFromPhysical(s.rt.data), true));
    auto si = targets_.find(TargetMapKey(d.data, true));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    uint64_t hv, hp;
    const DkShader* vs = Module(kDepthHlsl, "vs", vkshader::Stage::Vertex, hv);
    const DkShader* ps = Module(kDepthHlsl, "psmain", vkshader::Stage::Pixel, hp);
    if (!vs || !ps) return false;
    BeginRender(nullptr, &dst.img);
    Sample(src.img);
    Barrier();
    DkCmdBuf cmd = Cb();
    const DkShader* pair[] = {vs, ps};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
    const DkResHandle tex = dkMakeTextureHandle(src.img.desc, point_);
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &tex, 1);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(cmd, &rs);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = ds.depthWriteEnable = true;
    ds.depthCompareOp = DkCompareOp_Always;
    dkCmdBufBindDepthStencilState(cmd, &ds);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkCmdBufBindColorState(cmd, &cs);
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
    SetViewport(0, 0, float(dst.w), float(dst.h));
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    dst.drawn = true;
    return true;
}

bool DekoBackend::Shrink(Image& src, uint32_t w, uint32_t h, Image& down, uint32_t taps) {
    if (!down || down.w != w || down.h != h) {
        Retire(down);
        if (!CreateImage(down, kBgra, w, h, 1, 1, 1, true)) return false;
    }
    float uv[4] = {0, 0, 1, 1};
    Blit(down, 0, 0, w, h, src, uv, {.alpha = true, .taps = taps});
    return true;
}

Image* DekoBackend::CopySource(Target& t, Image& down) {
    if (t.depth) return nullptr;
    if (t.w != t.gw || t.h != t.gh) {
        uint32_t taps = uint32_t(std::ceil(float(t.w) / t.gw - 1e-3f));
        return Shrink(t.img, t.gw, t.gh, down, std::max(taps, 1u)) ? &down : nullptr;
    }
    return &t.img;
}

// an image's pixels into a CPU-cached block, rows packed (recorded; the caller submits and waits)
void DekoBackend::CopyToBuffer(Image& im, uint32_t w, uint32_t h, DkMemBlock block) {
    BeforeCopy(&im, nullptr);
    DkImageView v;
    View(im, v, true);
    const DkImageRect r = {0, 0, 0, w, h, 1};
    const DkCopyBuf dst = {dkMemBlockGetGpuAddr(block), w * 4, h};
    dkCmdBufCopyImageToBuffer(Cb(), &v, &r, &dst, 0);
}

bool DekoBackend::ReadBack(Image& im, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    uint32_t bytes = w * h * 4;
    DkMemBlock block = device_.CreateBlock(bytes, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached, "read-back");
    dkMemBlockFlushCpuCache(block, 0, dkMemBlockGetSize(block));
    CopyToBuffer(im, w, h, block);
    Submit(true);
    dkMemBlockFlushCpuCache(block, 0, dkMemBlockGetSize(block));
    const uint8_t* p = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(block));
    out.assign(p, p + bytes);
    device_.DestroyBlock(block);
    return true;
}

// ---- draws

constexpr float kUpscaledMin = 1.01f;

uint32_t DekoBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                        DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return 0;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    Target* rt = draw_rt_;
    float k = float(t.w) / (t.gw * stages.k[stage]);
    bool blur = rt && t.gw < guest_w_ && rt->gw < guest_w_;
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (blur || t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        stages.blur[stage] = blur;
        info.flags |= kTexWide;
    }
    stages.fmt[stage] = t.img.fmt;
    if (feedback) {
        stage_uncached_ = true;
        Image* fb = Feedback(t);
        stage_img_[stage] = fb;
        return fb ? fb->desc : 0;
    }
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    stage_img_[stage] = &t.img;
    return t.img.desc;
}

bool DekoBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    Retire(tx.img);
    tx = Tex{};
    tx.d = d;
    bool bc = fi.compressed;
    switch (fi.host) {
        case Host::BC1: tx.fmt = DkImageFormat_RGBA_BC1; break;
        case Host::BC2: tx.fmt = DkImageFormat_RGBA_BC2; break;
        case Host::BC3: tx.fmt = DkImageFormat_RGBA_BC3; break;
        case Host::R32F: tx.fmt = DkImageFormat_R32_Float; break;
        default: tx.fmt = kBgra; break;
    }
    uint32_t w = d.width, h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    DkImageType type = d.cube ? DkImageType_Cubemap : d.depth > 1 ? DkImageType_3D : DkImageType_2D;
    if (!CreateImage(tx.img, tx.fmt, w, h, d.cube ? 1 : d.depth, d.cube ? 6 : 1, tx.levels, false, type)) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void DekoBackend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                const uint32_t* colours) {
    BeforeCopy(nullptr, &tx.img);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    bool bc = fi.compressed;
    uint32_t block = fi.host == Host::BC1 ? 8 : 16;
    for (uint32_t f = 0; f < faces; f++)
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            uint32_t mw = std::max(tx.img.w >> l, 1u), mh = std::max(tx.img.h >> l, 1u), md = std::max(tx.img.d >> l, 1u);
            // rows packed: block rows for DXT, texel rows else
            uint32_t row = bc ? std::max(1u, (mw + 3) / 4) * block : mw * 4;
            uint32_t rows = bc ? std::max(1u, (mh + 3) / 4) : mh;
            size_t src_rows = pitch ? px.size() / pitch / md : 0;
            Upload u = Reserve(row * rows * md, 256);
            if (!u) continue;
            for (uint32_t z = 0; z < md; z++)
                for (uint32_t y = 0; y < rows; y++) {
                    uint8_t* dst = u.cpu + (size_t(z) * rows + y) * row;
                    if (y < src_rows) std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min(pitch, row));
                    else std::memset(dst, 0, row);
                }
            DkImageView v;
            View(tx.img, v);
            if (d.cube) v.type = DkImageType_2DArray;
            v.mipLevelOffset = uint8_t(l);
            v.mipLevelCount = 1;
            if (d.cube) {
                v.layerOffset = uint16_t(f);
                v.layerCount = 1;
            }
            const DkCopyBuf src = {u.gpu, 0, 0};
            const DkImageRect r = {0, 0, 0, mw, mh, md};
            dkCmdBufCopyBufferToImage(Cb(), &src, &v, &r, 0);
        }
    AfterCopy(tx.img);
}

uint32_t DekoBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                       DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    if (palette.va) stage_uncached_ = true;
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    Tex& tx = textures_[key];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return 0;
    Tex& t = textures_[key];
    if (t.frame != presents_ + 1 || !same || t.palette != palette.hash) {
        t.frame = presents_ + 1;
        t.palette = palette.hash;
        const uint32_t total = TotalSize(d);
        const uint64_t mix = DirtyEpoch(d.data) ^ palette.hash;
        const uint32_t every = check_frames_;
        // between whole checks a few samples: a lock, a new palette or changed samples check it whole at once
        bool whole = !same || every <= 1 || (presents_ + (key >> 6)) % every == 0;
        uint64_t sampled = MemSigSampled(d.data, total) ^ mix;
        sig_bytes_ += std::min<uint32_t>(total, 1024);
        if (!whole && sampled != t.sampled) whole = true;
        uint64_t sig = t.sig;
        if (whole) {
            sig = MemSig(d.data, total) ^ mix;
            sig_bytes_ += total <= 65536 ? total : 4096;
            t.sampled = sampled;
        }
        if (sig != t.sig || !same) {
            t.sig = sig;
            tex_uploads_++;
            tex_upload_bytes_ += total;
            UploadTexture(t, d, fi, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), palette.va ? palette.colours : nullptr);
        }
    }
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    stages.fmt[stage] = t.fmt;
    stages.levels[stage] = t.levels;
    stage_img_[stage] = &t.img;
    return t.img.desc;
}

uint32_t DekoBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stage_img_[stage] = nullptr;
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.fmt[stage] = kBgra;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return 0;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    auto tit = targets_.find(TargetMapKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed)
        return BindTargetTexture(s, stage, d, tit->second, true, stages);
    return BindGuestTexture(s, stage, d, fi, stages);
}

bool DekoBackend::BindStageTextures(const State& s, DrawTextures& stages) {
    const uint64_t dirty = DirtyEpochNow();
    for (int i = 0; i < 4; i++) {
        StageMemo& m = stage_memo_[i];
        uint32_t header = s.textures[size_t(i)], words[5] = {};
        if (header) std::memcpy(words, kernel::HostPtr(header), sizeof(words));
        if (m.valid && m.header == header && m.palette == s.palettes[size_t(i)] && m.frame == presents_ &&
            m.dirty == dirty && m.gen == bind_gen_ && m.rt == draw_rt_ && m.ds == draw_ds_ &&
            !std::memcmp(m.words, words, sizeof(words))) {
            stages.view[i] = m.view;
            stages.info[i] = m.info;
            stages.fmt[i] = m.fmt;
            stages.levels[i] = m.levels;
            stages.rt[i] = m.is_rt;
            stages.k[i] = m.k;
            stages.wide[i] = m.wide;
            stages.blur[i] = m.blur;
            stage_img_[i] = m.img;
            stage_memo_hits_++;
            continue;
        }
        stage_uncached_ = false;
        stages.view[i] = BindTexture(s, i, stages);
        m.valid = !stage_uncached_ && dirty == DirtyEpochNow();
        m.header = header;
        m.palette = s.palettes[size_t(i)];
        std::memcpy(m.words, words, sizeof(words));
        m.frame = presents_;
        m.dirty = dirty;
        m.gen = bind_gen_;
        m.rt = draw_rt_;
        m.ds = draw_ds_;
        m.view = stages.view[i];
        m.info = stages.info[i];
        m.fmt = stages.fmt[i];
        m.levels = stages.levels[i];
        m.is_rt = stages.rt[i];
        m.k = stages.k[i];
        m.wide = stages.wide[i];
        m.blur = stages.blur[i];
        m.img = stage_img_[i];
        stage_memo_misses_++;
    }
    for (int i = 0; i < 4; i++) {
        if (stages.view[i]) continue;
        uint8_t kind = stages.info[i].kind;
        stages.view[i] = dummy_[kind == kTex3D ? 1 : kind == kTexCube ? 2 : 0].desc;
        if (stages.info[i].shadow) stages.info[i].shadow = 0;
    }
    return true;
}

constexpr float kDepth24Max = 16777215.0f;

// what Vulkan's pipeline key holds, as deko3d's state objects
void DekoBackend::BindState(const State& s, const Assembled& a, const DrawShaders& sh) {
    DkCmdBuf cmd = Cb();
    PsoKey k;
    std::memset(&k, 0, sizeof(k));
    KeyBlend(s, k);
    KeyRasterizer(s, a, k);
    // vertex input: the attributes at their register's location, as vk_shader.cpp numbers the inputs
    DkVtxAttribState attribs[16];
    uint32_t na = 0;
    for (uint32_t r = 0; r < 16; r++) {
        DkVtxAttribState& at = attribs[r];
        std::memset(&at, 0, sizeof(at));
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[r];
            at.bufferId = e.buffer;
            at.offset = e.offset;
            VertexFormat(e.format, at);
            na = r + 1;
        } else {
            at.isFixed = 1;
            at.size = DkVtxAttribSize_1x32;
            at.type = DkVtxAttribType_Float;
        }
    }
    dkCmdBufBindVtxAttribState(cmd, attribs, na);
    DkVtxBufferState vbs[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) vbs[i] = {a.buffers[i].stride, 0};
    dkCmdBufBindVtxBufferState(cmd, vbs, a.buffer_count);

    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    constexpr uint32_t kFillWireframe = 0x1B01;
    rs.polygonModeFront = rs.polygonModeBack = k.fill == kFillWireframe ? DkPolygonMode_Line : DkPolygonMode_Fill;
    rs.cullMode = k.cull ? DkFace_Back : DkFace_None;
    rs.frontFace = k.ccw ? DkFrontFace_CCW : DkFrontFace_CW;
    rs.provokingVertex = DkProvokingVertex_First;
    if (draw_ds_ && a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
        float bias = float(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        float slope = Bits(SimpleState(s, kMSlopeScale)) * float(draw_ds_->w) / draw_ds_->gw;
        if (bias != 0 || slope != 0) {
            rs.depthBiasEnableMask = DkPolygonFlag_All;
            dkCmdBufSetDepthBias(cmd, bias, 0, slope);
        }
    }
    dkCmdBufBindRasterizerState(cmd, &rs);

    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = ds.depthWriteEnable = false;
    ds.stencilTestEnable = false;
    if (draw_ds_) {
        ds.depthTestEnable = s.rs[kRsZEnable] != 0;
        if (ds.depthTestEnable) {
            ds.depthWriteEnable = SimpleState(s, kMZWriteEnable) != 0;
            ds.depthCompareOp = Cmp(SimpleState(s, kMZFunc));
        }
        ds.stencilTestEnable = s.rs[kRsStencilEnable] != 0;
        if (ds.stencilTestEnable) {
            ds.stencilFrontFailOp = ds.stencilBackFailOp = StencilOp(s.rs[kRsStencilFail]);
            ds.stencilFrontDepthFailOp = ds.stencilBackDepthFailOp = StencilOp(SimpleState(s, kMStencilZFail));
            ds.stencilFrontPassOp = ds.stencilBackPassOp = StencilOp(SimpleState(s, kMStencilPass));
            ds.stencilFrontCompareOp = ds.stencilBackCompareOp = Cmp(SimpleState(s, kMStencilFunc));
            dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, uint8_t(SimpleState(s, kMStencilWriteMask)),
                               uint8_t(SimpleState(s, kMStencilRef)), uint8_t(SimpleState(s, kMStencilMask)));
        }
    }
    dkCmdBufBindDepthStencilState(cmd, &ds);

    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = k.blend ? 1 : 0;
    dkCmdBufBindColorState(cmd, &cs);
    if (k.blend) {
        DkBlendState bs;
        bs.colorBlendOp = bs.alphaBlendOp = BlendOp(k.op);
        bs.srcColorBlendFactor = Blend(k.src, false);
        bs.dstColorBlendFactor = Blend(k.dst, false);
        bs.srcAlphaBlendFactor = Blend(k.src, true);
        bs.dstAlphaBlendFactor = Blend(k.dst, true);
        dkCmdBufBindBlendState(cmd, 0, &bs);
        float bf[4];
        Color(SimpleState(s, kMBlendColor), bf);
        dkCmdBufSetBlendConst(cmd, bf[0], bf[1], bf[2], bf[3]);
    }
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkColorWriteStateSetMask(&cw, 0, k.wmask);
    dkCmdBufBindColorWriteState(cmd, &cw);
}

void DekoBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    DK_GATE();
    if (DepthCopy(s)) return;
    uint64_t t = TickNs(), t2;
    auto phase = [&](Phase p) {
        t2 = TickNs();
        phase_ns_[p] += t2 - t;
        t = t2;
    };
    Assembled& a = asm_;
    if (!Assemble(s, prim, start, count, indices, a)) return;
    phase(kPhAssemble);

    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return;
    if (rt && LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
    draw_rt_ = rt;
    draw_ds_ = ds;
    phase(kPhTargets);

    DrawShaders sh = ClassifyShaders<DrawShaders>(s, a);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    phase(kPhShaders);
    DrawTextures stages;
    if (!BindStageTextures(s, stages) || !draw_rt_ && !draw_ds_) return;
    phase(kPhTextures);
    PixelKey pk = MakePixelKey(s, stages.info);
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs || !sh.ps) return;
    phase(kPhShaders);

    GuestExtent size;
    SurfaceSize(draw_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    FillSurfaceConstants(s, size, c);
    if (a.topology == Topology::Points) {
        const Target* pt = draw_rt_ ? draw_rt_ : draw_ds_;
        c.surf[3] = pt && pt->gw ? float(pt->w) / float(pt->gw) : 1.0f;
    }
    FillFogConstants(s, c);
    FillCombinerConstants(s, c);
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    phase(kPhConstants);
    // everything uploaded before the targets are bound: a full ring ends the slot, which drops the bound state
    uint32_t need = 2 * kUboRange + 1024 + a.index_count * 4 + 16;
    for (uint32_t i = 0; i < a.buffer_count; i++) need += a.buffers[i].bytes + 16;
    if (core_.UploadFree() < need) Submit(false);
    Upload vc = ConstantsReused(last_vsc_, s.vsc.data(), sizeof(s.vsc));
    Upload dc = ConstantsReused(last_dc_, &c, sizeof(c));
    DkGpuAddr vbuf[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Upload v = Reserve(std::max<uint32_t>(b.bytes, 4), 16);
        if (b.src && b.bytes) std::memcpy(v.cpu, b.src, b.bytes);
        upload_bytes_ += b.bytes;
        vbuf[i] = v.gpu;
    }
    Upload ib{};
    uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
    if (a.index_count) {
        ib = Reserve(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        upload_bytes_ += ibytes;
    }
    phase(kPhUpload);
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    DkResHandle tex[4];
    for (int i = 0; i < 4; i++) tex[i] = dkMakeTextureHandle(stages.view[i], StageSampler(s, i, stages, clamp));
    phase(kPhSets);

    BeginRender(draw_rt_ ? &draw_rt_->img : nullptr, draw_ds_ ? &draw_ds_->img : nullptr);
    for (Image* im : stage_img_)
        if (im) Sample(*im);
    Barrier();
    if (draw_rt_) draw_rt_->drawn = true;
    if (draw_ds_) draw_ds_->drawn = true;
    DkCmdBuf cmd = Cb();
    const DkShader* pair[] = {sh.vs, sh.ps};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
    for (DkStage st : {DkStage_Vertex, DkStage_Fragment}) {
        dkCmdBufBindUniformBuffer(cmd, st, 0, vc.gpu, kUboRange);
        dkCmdBufBindUniformBuffer(cmd, st, 1, dc.gpu, kUboRange);
    }
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, tex, 4);
    phase(kPhPipeline);
    BindState(s, a, sh);
    Target& bt = draw_rt_ ? *draw_rt_ : *draw_ds_;
    float W = float(bt.w), H = float(bt.h);
    SetViewport(0, 0, W, H);
    // STREET: host pixel centres inside each scaled console rectangle, X and Y scaled apart (as d3d12.cpp)
    float sx = W / size.w, sy = H / size.h;
    int32_t x0 = PixelBoundary(float(s.viewport.x), sx), y0 = PixelBoundary(float(s.viewport.y), sy);
    int32_t x1 = PixelBoundary(float(s.viewport.x + s.viewport.w), sx);
    int32_t y1 = PixelBoundary(float(s.viewport.y + s.viewport.h), sy);
    if (s.scissor_count) {
        x0 = std::max(x0, PixelBoundary(float(s.scissors[0]), sx));
        y0 = std::max(y0, PixelBoundary(float(s.scissors[1]), sy));
        x1 = std::min(x1, PixelBoundary(float(s.scissors[2]), sx));
        y1 = std::min(y1, PixelBoundary(float(s.scissors[3]), sy));
    }
    SetScissor(std::clamp(x0, 0, int32_t(W)), std::clamp(y0, 0, int32_t(H)), std::clamp(x1, 0, int32_t(W)),
               std::clamp(y1, 0, int32_t(H)));
    // one buffer per call: dkCmdBufBindVtxBuffers writes past what it reserves with several (deko3d 0.5.0)
    for (uint32_t i = 0; i < a.buffer_count; i++)
        dkCmdBufBindVtxBuffer(cmd, i, vbuf[i], std::max<uint32_t>(a.buffers[i].bytes, 4));
    DkPrimitive prim_dk = Primitive(a);
    if (a.index_count) {
        dkCmdBufBindIdxBuffer(cmd, a.index16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32, ib.gpu);
        dkCmdBufDrawIndexed(cmd, prim_dk, a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        dkCmdBufDraw(cmd, prim_dk, a.draw_count, 1, 0, 0);
    }
    phase(kPhRecord);
}

void DekoBackend::ClearTarget(Target& t, const State& s, uint32_t count, const uint8_t* rects, uint32_t flags,
                              uint32_t color, float z, uint32_t stencil) {
    struct R {
        int32_t l, t, r, b;
    };
    std::vector<R> rs;
    auto add = [&](int x0, int y0, int x1, int y1) {
        float sx = float(t.w) / t.gw, sy = float(t.h) / t.gh;
        R r{std::clamp(PixelBoundary(float(x0), sx), 0, int32_t(t.w)), std::clamp(PixelBoundary(float(y0), sy), 0, int32_t(t.h)),
            std::clamp(PixelBoundary(float(x1), sx), 0, int32_t(t.w)), std::clamp(PixelBoundary(float(y1), sy), 0, int32_t(t.h))};
        if (r.r > r.l && r.b > r.t) rs.push_back(r);
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; i++) {
            int32_t r[4];
            std::memcpy(r, rects + size_t(i) * sizeof(r), sizeof(r));
            add(r[0], r[1], r[2], r[3]);
        }
    } else {
        const auto& v = s.viewport;
        add(int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h));
    }
    t.drawn = true;
    if (rs.empty()) return;
    if (t.depth) {
        bool cz = flags & kClearZ, cs = flags & kClearStencil;
        if (!cz && !cs) return;
        BeginRender(nullptr, &t.img);
        Barrier();
        for (const R& r : rs) {
            SetScissor(r.l, r.t, r.r, r.b);
            dkCmdBufClearDepthStencil(Cb(), cz, std::clamp(z, 0.0f, 1.0f), cs ? 0xFF : 0, uint8_t(stencil));
        }
    } else {
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        float c[4];
        Color(color, c);
        BeginRender(&t.img, nullptr);
        Barrier();
        for (const R& r : rs) {
            SetScissor(r.l, r.t, r.r, r.b);
            dkCmdBufClearColorFloat(Cb(), 0, DkColorMask_RGBA, c[0], c[1], c[2], c[3]);
        }
    }
}

void DekoBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                        uint32_t stencil) {
    DK_GATE();
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    for (Target* t : {rt, ds})
        if (t) ClearTarget(*t, s, count, rects, flags, color, z, stencil);
}

void DekoBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) {
    DK_GATE();
    Target* src_t = FindTarget(src, false);
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "deko3d: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Target* dst_t = EnsureTarget(dst, false);
    src_t = FindTarget(src, false);
    if (!dst_t || !src_t || src_t == dst_t) return;
    BeforeCopy(&src_t->img, &dst_t->img);
    DkImageView sv, dv;
    View(src_t->img, sv, true);
    View(dst_t->img, dv, true);
    float dst_scale = float(dst_t->w) / dst_t->gw;
    bool listed = count && src_rects;
    uint32_t n = listed ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t l = 0, t = 0, r = std::min(src_t->w, dst_t->w), b = std::min(src_t->h, dst_t->h), dx = 0, dy = 0;
        if (listed) {
            GuestRect g = ReadGuestRect(src_rects, i);
            l = ScaleUnsigned(g.left, dst_scale);
            t = ScaleUnsigned(g.top, dst_scale);
            r = std::min(ScaleUnsigned(g.right, dst_scale), src_t->w);
            b = std::min(ScaleUnsigned(g.bottom, dst_scale), src_t->h);
            dx = l;
            dy = t;
            if (dst_points) {
                int32_t px, py;
                ReadGuestPoint(dst_points, i, px, py);
                dx = ScaleUnsigned(px, dst_scale);
                dy = ScaleUnsigned(py, dst_scale);
            }
        }
        if (r <= l || b <= t || dx >= dst_t->w || dy >= dst_t->h) continue;
        r = std::min(r, l + dst_t->w - dx);
        b = std::min(b, t + dst_t->h - dy);
        const DkImageRect sr = {l, t, 0, r - l, b - t, 1};
        const DkImageRect dr = {dx, dy, 0, r - l, b - t, 1};
        dkCmdBufCopyImage(Cb(), &sv, &sr, &dv, &dr, 0);
    }
    AfterCopy(dst_t->img);
    dst_t->drawn = true;
}

// ---- read-backs

void DekoBackend::Sync(uint32_t data, uint32_t) {
    DK_GATE();
    SyncLocked(data);
}

void DekoBackend::SyncLocked(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    Target& t = it->second;
    Image down;
    Image* src = CopySource(t, down);
    std::vector<uint8_t> px;
    bool ok = src && ReadBack(*src, t.gw, t.gh, px);
    Retire(down);
    if (!ok) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void DekoBackend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool DekoBackend::DeliverLate(uint32_t data) {
    std::lock_guard gate(g_gate);
    if (g_stopped) return false;
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = Completed(), now = presents_;
    int best = -1;
    for (uint32_t i = 0; i < kSlots; i++) {
        bool finished = l.work[i] && l.work[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kSlots) + 1;
        if (finished && recent && (best < 0 || l.work[i] > l.work[best])) best = int(i);
    }
    if (best < 0) return false;
    if (l.work[best] > l.delivered) {
        DkMemBlock b = l.rb[best];
        dkMemBlockFlushCpuCache(b, 0, dkMemBlockGetSize(b));
        const uint8_t* p = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(b));
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.gw * 4, l.gw * 4);
        l.delivered = l.work[best];
    }
    return true;
}

void DekoBackend::RecordLate(uint32_t data) {
    DK_GATE();
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        core_.WaitGpu();
        for (DkMemBlock& b : l.rb)
            if (b) device_.DestroyBlock(b), b = nullptr;
        Retire(l.down);
        l = Late{};
        l.gw = t.gw;
        l.gh = t.gh;
    }
    uint32_t slot = l.next;
    if (l.work[slot] > Completed()) return;
    Image* src = CopySource(t, l.down);
    if (!src) return;
    if (!l.rb[slot]) {
        l.rb[slot] = device_.CreateBlock(t.gw * t.gh * 4, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached,
                                         "late read-back");
        dkMemBlockFlushCpuCache(l.rb[slot], 0, dkMemBlockGetSize(l.rb[slot]));
    }
    CopyToBuffer(*src, t.gw, t.gh, l.rb[slot]);
    rendering_ = false;  // the copy engine ran between draws: bind the targets again before the next
    l.work[slot] = work_;  // the work this copy is in
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kSlots;
}

// ---- present

// the swapchain at the screen's size, made again when the console's mode or vsync changes
void DekoBackend::EnsureSwapchain() {
    int w, h;
    host::WindowPixels(w, h);
    bool vsync = app::Get().vsync;
    if (swapchain_.created() && uint32_t(w) == swap_w_ && uint32_t(h) == swap_h_ && vsync == vsync_) return;
    if (core_.WorkOpen()) Submit(true);
    swapchain_.Create(device_, uint32_t(w), uint32_t(h), vsync ? 1 : 0);
    swap_w_ = uint32_t(w);
    swap_h_ = uint32_t(h);
    vsync_ = vsync;
    LOGR("deko3d: swapchain %ux%u, vsync %s", swap_w_, swap_h_, vsync ? "on" : "off");
}

void DekoBackend::DrawFrame(Image* src, int image) {
    // the frame letterboxed to the game's aspect over black
    DkImage& sw = swapchain_.Image(image);
    rendering_ = false;
    rend_color_ = rend_depth_ = nullptr;
    if (src) Sample(*src);
    Barrier();
    DkImageView v;
    dkImageViewDefaults(&v, &sw);
    dkCmdBufBindRenderTarget(Cb(), &v, nullptr);
    rendering_ = false;
    SetScissor(0, 0, int32_t(swap_w_), int32_t(swap_h_));
    dkCmdBufClearColorFloat(Cb(), 0, DkColorMask_RGBA, 0, 0, 0, 1);
    if (!src) return;
    float aspect = app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;  // STREET: as d3d12.cpp
    float w = float(swap_w_), h = float(swap_h_);
    float vw = std::min(w, h * aspect), vh = vw / aspect;
    uint64_t hv, hp;
    const DkShader* vs = Module(kBlitHlsl, "vs", vkshader::Stage::Vertex, hv);
    const DkShader* ps = Module(kBlitHlsl, "psmain", vkshader::Stage::Pixel, hp);
    if (!vs || !ps) return;
    float c[8] = {0, 0, 1, 1, gamma_ ? 1.0f : 0.0f, 0, 1, 1};
    Upload cb = Constants(c, sizeof(c));
    // the upload may have ended the slot: the swapchain image bound again
    dkCmdBufBindRenderTarget(Cb(), &v, nullptr);
    Barrier();
    DkCmdBuf cmd = Cb();
    const DkShader* pair[] = {vs, ps};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 1, cb.gpu, kUboRange);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, cb.gpu, kUboRange);
    const DkResHandle tex[2] = {dkMakeTextureHandle(src->desc, linear_), dkMakeTextureHandle(lut_.desc, point_)};
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, tex, 2);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(cmd, &rs);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = ds.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(cmd, &ds);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkCmdBufBindColorState(cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(cmd, &cw);
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
    SetViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
    SetScissor(0, 0, int32_t(w), int32_t(h));
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
}

// SMAA over src (the front target at its scaled size) into smaa_out_; null when it cannot run, and the frame is shown
// as it is. Any shader that does not compile turns it off for the session.
Image* DekoBackend::Smaa(Image& src) {
    if (smaa_failed_) return nullptr;
    if (!smaa_area_) {
        std::vector<uint8_t> area(size_t(AREATEX_WIDTH) * AREATEX_HEIGHT * 4), search(size_t(SEARCHTEX_SIZE) * 4);
        for (size_t i = 0; i < size_t(AREATEX_WIDTH) * AREATEX_HEIGHT; i++) {
            area[i * 4 + 2] = areaTexBytes[i * 2];  // BGRA: the table's R and G read back as .r and .g
            area[i * 4 + 1] = areaTexBytes[i * 2 + 1];
            area[i * 4 + 3] = 255;
        }
        for (size_t i = 0; i < SEARCHTEX_SIZE; i++) {
            search[i * 4 + 2] = searchTexBytes[i];
            search[i * 4 + 3] = 255;
        }
        if (!PixelImage(smaa_area_, area, AREATEX_WIDTH * 4, AREATEX_WIDTH, AREATEX_HEIGHT) ||
            !PixelImage(smaa_search_, search, SEARCHTEX_WIDTH * 4, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT)) {
            LOGE("deko3d: SMAA tables could not be made: SMAA off");
            smaa_failed_ = true;
            return nullptr;
        }
    }
    if (!smaa_out_ || smaa_out_.w != src.w || smaa_out_.h != src.h) {
        for (Image* im : {&smaa_edges_, &smaa_blend_, &smaa_out_}) {
            Retire(*im);
            if (!CreateImage(*im, kBgra, src.w, src.h, 1, 1, 1, true)) {
                LOGE("deko3d: SMAA targets could not be made: SMAA off");
                smaa_failed_ = true;
                return nullptr;
            }
        }
        LOGR("deko3d: SMAA at %ux%u", src.w, src.h);
    }
    const float rt[4] = {1.0f / float(src.w), 1.0f / float(src.h), float(src.w), float(src.h)};
    Image* const edge_in[] = {&src};
    Image* const weight_in[] = {&smaa_edges_, &smaa_area_, &smaa_search_};
    Image* const blend_in[] = {&src, &smaa_blend_};
    if (!SmaaPass(0, smaa_edges_, edge_in, 1, rt) || !SmaaPass(1, smaa_blend_, weight_in, 3, rt) ||
        !SmaaPass(2, smaa_out_, blend_in, 2, rt))
        return nullptr;
    return &smaa_out_;
}

// SMAA's six shaders, from Init: the first start compiles them (uam takes seconds on the console, and a frame that long
// let a game thread run ahead into an object not made yet), later starts load them from the shader cache
void DekoBackend::SmaaShaders() {
    static const char* const kEntries[6] = {"edge_vs", "edge_ps", "weight_vs", "weight_ps", "blend_vs", "blend_ps"};
    const std::string src = kSmaaHlsl;
    uint64_t t0 = TickNs();
    for (int i = 0; i < 6; i++) {
        uint64_t h;
        smaa_shaders_[i] = Module(src, kEntries[i], i % 2 ? vkshader::Stage::Pixel : vkshader::Stage::Vertex, h);
        if (!smaa_shaders_[i]) {
            LOGE("deko3d: SMAA %s does not compile: SMAA off", kEntries[i]);
            smaa_failed_ = true;
            return;
        }
    }
    LOGR("deko3d: SMAA shaders ready in %.2f s", (TickNs() - t0) / 1e9);
}

// one SMAA pass, a full screen triangle into dst: 0 edges, 1 blending weights (both cleared first), 2 the blend
bool DekoBackend::SmaaPass(int pass, Image& dst, Image* const* tex, uint32_t n, const float rt[4]) {
    const DkShader* vs = smaa_shaders_[pass * 2];
    const DkShader* ps = smaa_shaders_[pass * 2 + 1];
    if (!vs || !ps) {
        smaa_failed_ = true;
        return false;
    }
    Upload cb = Constants(rt, sizeof(float) * 4);
    BeginRender(&dst, nullptr);
    for (uint32_t i = 0; i < n; i++) Sample(*tex[i]);
    Barrier();
    DkCmdBuf cmd = Cb();
    SetScissor(0, 0, int32_t(dst.w), int32_t(dst.h));
    if (pass < 2) dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0, 0, 0, 0);
    const DkShader* pair[] = {vs, ps};
    dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, pair, 2);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 1, cb.gpu, kUboRange);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 1, cb.gpu, kUboRange);
    DkResHandle handles[3];
    for (uint32_t i = 0; i < n; i++) handles[i] = dkMakeTextureHandle(tex[i]->desc, linear_);
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, handles, n);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(cmd, &rs);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = ds.depthWriteEnable = false;
    dkCmdBufBindDepthStencilState(cmd, &ds);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    dkCmdBufBindColorState(cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkCmdBufBindColorWriteState(cmd, &cw);
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
    SetViewport(0, 0, float(dst.w), float(dst.h));
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    return true;
}

void DekoBackend::UploadGammaRamp() {
    ramp_dirty_ = false;
    Upload u = Reserve(256 * 4, 256);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = u.cpu + i * 4;
        p[0] = ramp_[512 + i];
        p[1] = ramp_[256 + i];
        p[2] = ramp_[i];
        p[3] = 255;
        identity &= ramp_[i] == i && ramp_[256 + i] == i && ramp_[512 + i] == i;
    }
    gamma_ = !identity;
    DkImageView v;
    View(lut_, v);
    BeforeCopy(nullptr, &lut_);
    const DkCopyBuf src = {u.gpu, 0, 0};
    const DkImageRect r = {0, 0, 0, 256, 1, 1};
    dkCmdBufCopyBufferToImage(Cb(), &src, &v, &r, 0);
    AfterCopy(lut_);
}

void DekoBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 1024;
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            Retire(it->second.img);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void DekoBackend::ApplySettings() {
    float want = app::RenderScale(guest_h_);
    if (std::fabs(want - scale_) <= 1e-4f) return;
    for (auto& [key, t] : targets_) {
        Retire(t.img);
        Retire(t.fb);
    }
    targets_.clear();
    draw_rt_ = draw_ds_ = nullptr;
    scale_ = want;
    LOGI("deko3d: render scale %.3f", scale_);
}

// STREET: the F12 capture (the pad script's SHOT): the frame at the game's size, as vulkan.cpp's but without the gamma
// ramp, read back from the front target; with SMAA, its result at the scaled size
void DekoBackend::SaveShot(Target& t, Image* shown) {
    Image down;
    bool smaa = shown && shown != &t.img;
    Image* src = smaa ? shown : CopySource(t, down);
    uint32_t w = smaa ? shown->w : t.gw, h = smaa ? shown->h : t.gh;
    std::vector<uint8_t> px;
    bool ok = src && ReadBack(*src, w, h, px);
    Retire(down);
    if (!ok) return;
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; i++) {
        rgb[i * 3] = px[i * 4 + 2];
        rgb[i * 3 + 1] = px[i * 4 + 1];
        rgb[i * 3 + 2] = px[i * 4];
    }
    std::error_code ec;
    std::filesystem::path dir = app::ExeDir() / "screenshots";
    std::filesystem::create_directories(dir, ec);
    static uint32_t serial = 0;
    char name[48];
    std::snprintf(name, sizeof(name), "STREET_dk_%llu_%u.png", (unsigned long long)presents_, ++serial);
    std::vector<uint8_t> png = EncodePng(rgb, w, h);
    std::ofstream f(dir / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    LOGR("screenshot saved: %s", (dir / name).string().c_str());
}

void DekoBackend::Present(const Surface& front) {
    DK_GATE();
    uint64_t now_ns = TickNs();
    if (last_present_ns_) {
        uint64_t gap = now_ns - last_present_ns_;
        pace_max_ns_ = std::max(pace_max_ns_, gap);
        pace_late_[0] += gap > 18000000;
        pace_late_[1] += gap > 25000000;
        pace_late_[2] += gap > 34000000;
        // a hitch, with where the render thread waited in the frame before: the screen's image, or the GPU's slot
        static unsigned reported = 0;
        if (gap > 50000000 && presents_ > 2 && reported < 400) {
            reported++;
            LOGR("pacing: render thread present gap of %.0f ms (frame %llu; before it acquire %.1f ms, slot wait %.1f ms)",
                 gap / 1e6, (unsigned long long)presents_, last_acquire_ns_ / 1e6, last_reset_ns_ / 1e6);
        }
    }
    last_present_ns_ = now_ns;
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    Image* shown = t ? &t->img : nullptr;
    if (t && host::Smaa())
        if (Image* aa = Smaa(t->img)) shown = aa;
    if (t && gpu::g_screenshot.exchange(false)) SaveShot(*t, shown);
    if (ramp_dirty_) UploadGammaRamp();
    EnsureSwapchain();
    // the frame's draws go to the GPU before the acquire, which makes the queue wait for the screen's image: the GPU
    // works on them meanwhile. The slot's GPU time ends here, without that wait.
    uint32_t slot = uint32_t(work_ % kSlots);
    dkCmdBufReportCounter(Cb(), DkCounter_Timestamp, core_.QueriesGpu() + slot * 32 + 16);
    core_.SubmitPartial();
    core_.BindDescriptors();
    uint64_t ta = TickNs();
    int image = swapchain_.Acquire();
    last_acquire_ns_ = TickNs() - ta;
    phase_ns_[kPhAcquire] += last_acquire_ns_;
    DrawFrame(shown, image);
    uint64_t tp = TickNs();
    core_.EndWork();
    swapchain_.Present(image);
    phase_ns_[kPhPresent] += TickNs() - tp;
    host::OverlaysFrame(swap_w_, swap_h_);  // STREET: Status Monitor's FPS and RES
    presents_++;
    uint64_t tr = TickNs();
    BeginWork();  // waits for the slot three frames back
    last_reset_ns_ = TickNs() - tr;
    phase_ns_[kPhReset] += last_reset_ns_;
    ReleaseOldTextures();
    LogPerf();
    ApplySettings();
}

// STREET's perf lines, as vulkan.cpp's: every 10 s the frame rate and where the time went; and the overlays' FPS
void DekoBackend::LogPerf() {
    auto now = std::chrono::steady_clock::now();
    static auto overlay_t = now;
    static uint64_t overlay_frames = 0;
    static double overlay_avg[10] = {};
    static int overlay_at = 0;
    double overlay_el = std::chrono::duration<double>(now - overlay_t).count();
    if (overlay_el >= 1.0) {
        double fps1 = double(presents_ - overlay_frames) / overlay_el;
        overlay_avg[overlay_at++ % 10] = fps1;
        int n10 = std::min(overlay_at, 10);
        double sum = 0;
        for (int i = 0; i < n10; i++) sum += overlay_avg[i];
        host::OverlaysSecond(fps1, sum / n10);
        overlay_t = now;
        overlay_frames = presents_;
    }
    static uint64_t frames_logged = 0;
    if (now - fps_t_ < std::chrono::seconds(10)) return;
    double secs = std::chrono::duration<double>(now - fps_t_).count();
    double n = double(std::max<uint64_t>(presents_ - frames_logged, 1));
    fps_t_ = now;
    frames_logged = presents_;
    auto ms = [&](std::atomic<uint64_t>& a) { return double(a.exchange(0)) / 1e6 / n; };
    double rec = ms(gpu::g_perf_capture_ns), wait = ms(gpu::g_perf_wait_ns), rt = ms(gpu::g_perf_render_ns);
    double idle = ms(gpu::g_perf_idle_ns);
    double kinds[6];
    for (int i = 0; i < 6; i++) kinds[i] = ms(gpu::g_perf_wait_kind_ns[i]);
    double gpu_ms = gpu_works_ ? double(gpu_ns_) / 1e6 / n : -1;
    double draws = double(gpu::g_perf_draws.exchange(0)) / n, waits = double(gpu::g_perf_waits.exchange(0)) / n;
    gpu_ns_ = gpu_works_ = 0;
    LOGR("perf: %.1f fps, screen %ux%u, guest %ux%u, render scale %.3f (deko3d)", n / secs, swap_w_, swap_h_, guest_w_,
         guest_h_, scale_);
    LOGR("perf: pacing: %u of %.0f frames over 18 ms, %u over 25 ms, %u over 34 ms, longest %.1f ms", pace_late_[0], n,
         pace_late_[1], pace_late_[2], double(pace_max_ns_) / 1e6);
    pace_late_[0] = pace_late_[1] = pace_late_[2] = 0;
    pace_max_ns_ = 0;
    LOGR("perf: ms a frame: rec %.2f, wait %.2f (%.2f/%.2f/%.2f/%.2f/%.2f/%.2f), rt %.2f, gpu %.2f, idle %.2f; "
         "%.0f draws, %.0f waits a frame",
         rec, wait, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], rt, gpu_ms, idle, draws, waits);
    double ph[kPhases];
    for (int i = 0; i < kPhases; i++) {
        ph[i] = double(phase_ns_[i]) / 1e6 / n;
        phase_ns_[i] = 0;
    }
    double reused = const_reused_ + const_uploaded_ ? 100.0 * double(const_reused_) / double(const_reused_ + const_uploaded_) : 0;
    const_reused_ = const_uploaded_ = 0;
    LOGR("perf: rt ms a frame: assemble %.2f, targets %.2f, shaders %.2f, textures %.2f, constants %.2f, upload %.2f (%.0f KB), bind %.2f, samplers %.2f, record %.2f; "
         "acquire %.2f, next slot %.2f, present %.2f; constants reused %.0f%%; %llu shaders compiled (%.0f ms); "
         "barriers a frame: %.1f full, %.1f fragment",
         ph[kPhAssemble], ph[kPhTargets], ph[kPhShaders], ph[kPhTextures], ph[kPhConstants], ph[kPhUpload],
         double(upload_bytes_) / 1024 / n, ph[kPhPipeline], ph[kPhSets], ph[kPhRecord], ph[kPhAcquire],
         ph[kPhReset], ph[kPhPresent], reused, (unsigned long long)compiles_, compile_ns_ / 1e6, barriers_full_ / n,
         barriers_frag_ / n);
    compiles_ = compile_ns_ = 0;
    upload_bytes_ = 0;
    LOGR("perf: texture stages remembered %.0f%%, upload ring %s",
         stage_memo_hits_ + stage_memo_misses_ ? 100.0 * double(stage_memo_hits_) / double(stage_memo_hits_ + stage_memo_misses_) : 0.0,
         core_.config().upload_cached ? "cached" : "uncached");
    stage_memo_hits_ = stage_memo_misses_ = 0;
    LOGR("perf: textures a frame: %.0f KB checked, %.1f uploaded (%.0f KB); vertices copied by the title thread %.0f KB",
         double(sig_bytes_) / 1024 / n, double(tex_uploads_) / n, double(tex_upload_bytes_) / 1024 / n,
         double(gpu::g_perf_copied_bytes.exchange(0)) / 1024 / n);
    sig_bytes_ = tex_uploads_ = tex_upload_bytes_ = 0;
    barriers_full_ = barriers_frag_ = 0;
    auto per = [&](std::atomic<uint64_t>& a) { return double(a.exchange(0)) / n; };
    double raw = per(gpu::g_perf_asm_raw), conv = per(gpu::g_perf_asm_converted), listed = per(gpu::g_perf_asm_listed);
    double verts = per(gpu::g_perf_asm_vertices);
    LOGR("perf: draws a frame assembled: %.0f raw, %.0f converted on the CPU (%.0f vertices), %.0f with a rebuilt index list",
         raw, conv, verts, listed);
}

}  // namespace

std::unique_ptr<Backend> CreateDekoBackend() { return std::make_unique<DekoBackend>(); }

void StopRendering() {
    std::lock_guard lock(g_gate);
    g_stopped = true;
}

}  // namespace gpu
