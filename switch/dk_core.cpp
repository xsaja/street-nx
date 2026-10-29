// deko3d renderer, the core (dk_core.h). Adapted from NFSMW-NX's nfsmw_dk_nucleo.cpp (GPL-3.0).
#include "dk_core.h"

#include <switch.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dk {

namespace {

LogFn g_write = nullptr;
void (*g_flush)() = nullptr;

constexpr uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// command memory goes to the command buffer in pieces of this size: the first at the start of each work, the next
// ones as it asks
constexpr uint32_t kCommandPiece = 64u << 10;
// the last bytes of each piece are not given to deko3d: dkCmdBufBindVtxBuffers in 0.5.0 reserves 5 words a buffer
// and writes 6, so near the end of a piece it writes past it; with this gap such words land where nothing is reused
constexpr uint32_t kCommandSlack = 256;

// deko3d's messages; only the debug library (libdeko3dd) calls this
void OnMessage(void*, const char* context, DkResult result, const char* message) {
    if (result != DkResult_Success)
        Fatal("[dk] deko3d error in %s: result %d: %s", context ? context : "?", int(result), message ? message : "");
    static int count = 0;
    if (count < 200) Log("[dk] deko3d %s: %s", context ? context : "?", message ? message : "");
    else if (count == 200) Log("[dk] deko3d: more messages, not logged");
    ++count;
}

void AddCommandMemory(void* core, DkCmdBuf which, size_t minimum) {
    static_cast<Core*>(core)->MoreCommandMemory(which, minimum);
}

}  // namespace

// ---- Log

void SetLog(LogFn write, void (*flush)()) {
    g_write = write;
    g_flush = flush;
}

void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (g_write) g_write(line);
    else std::puts(line);
}

void FlushLog() {
    if (g_flush) g_flush();
    else std::fflush(stdout);
}

void Fatal(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    Log("%s", line);
    FlushLog();
    std::abort();
}

// ---- Device and queue

Device::~Device() { Destroy(); }

void Device::Create(bool zcull) {
    Log("[dk] creating the deko3d device (origin upper left, depth 0..1)");
    FlushLog();
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = OnMessage;
    dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
    device_ = dkDeviceCreate(&dm);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device_);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | (zcull ? DkQueueFlags_EnableZcull : DkQueueFlags_DisableZcull);
    qm.commandMemorySize = 1u << 20;
    qm.flushThreshold = qm.commandMemorySize / 8;
    Log("[dk] creating the queue: graphics, ZCULL %s", zcull ? "on" : "off");
    FlushLog();
    queue_ = dkQueueCreate(&qm);
}

void Device::Destroy() {
    if (!device_) return;
    if (queue_) {
        std::lock_guard lock(lock_);
        if (!dkQueueIsInErrorState(queue_)) dkQueueWaitIdle(queue_);
        dkQueueDestroy(queue_);
        queue_ = nullptr;
    }
    dkDeviceDestroy(device_);
    device_ = nullptr;
}

DkMemBlock Device::CreateBlock(uint32_t bytes, uint32_t flags, const char* name) {
    bytes = Align(bytes, DK_MEMBLOCK_ALIGNMENT);
    std::lock_guard lock(creations_);
    Log("[dk] memory block: %s, %u KiB (flags 0x%X)", name, bytes >> 10, flags);
    FlushLog();  // a failed creation ends the process inside deko3d
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, device_, bytes);
    m.flags = flags;
    return dkMemBlockCreate(&m);
}

void Device::DestroyBlock(DkMemBlock block) {
    if (!block) return;
    std::lock_guard lock(creations_);
    dkMemBlockDestroy(block);
}

DkSwapchain Device::CreateSwapchain(NWindow* window, const DkImage* const images[], uint32_t count) {
    std::lock_guard lock(creations_);
    FlushLog();
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, device_, window ? window : nwindowGetDefault(), images, count);
    return dkSwapchainCreate(&sm);
}

void Device::DestroySwapchain(DkSwapchain swapchain) {
    if (!swapchain) return;
    std::lock_guard lock(creations_);
    dkSwapchainDestroy(swapchain);
}

void Device::CheckQueue(const char* before) const {
    if (dkQueueIsInErrorState(queue_)) Fatal("[dk] the deko3d queue is in an error state (a GPU fault) before %s", before);
}

void Device::Submit(DkCmdList list, const char* what) {
    std::lock_guard lock(lock_);
    CheckQueue(what);
    dkQueueSubmitCommands(queue_, list);
    dkQueueFlush(queue_);
}

void Device::Submit(DkCmdList first, DkCmdList second, const char* what) {
    std::lock_guard lock(lock_);
    CheckQueue(what);
    dkQueueSubmitCommands(queue_, first);
    dkQueueSubmitCommands(queue_, second);
    dkQueueFlush(queue_);
}

void Device::WaitIdle() {
    std::lock_guard lock(lock_);
    CheckQueue("waiting for the GPU");
    dkQueueWaitIdle(queue_);
}

int Device::Acquire(DkSwapchain swapchain) {
    // the acquire may wait for the window to give an image back: outside the queue lock, only the GPU wait for that
    // image goes into the queue
    int image = 0;
    DkFence fence;
    dkSwapchainAcquireImage(swapchain, &image, &fence);
    std::lock_guard lock(lock_);
    CheckQueue("waiting for a swapchain image");
    dkQueueWaitFence(queue_, &fence);
    return image;
}

void Device::Present(DkSwapchain swapchain, int image) {
    std::lock_guard lock(lock_);
    CheckQueue("presenting");
    dkQueuePresentImage(queue_, swapchain, image);
}

// ---- One recording thread's command buffer and memory

Core::~Core() { Destroy(); }

DkMemBlock Core::CreateBlock(uint32_t bytes, uint32_t flags, const char* name) { return dev_->CreateBlock(bytes, flags, name); }

void Core::Create(Device& dev, const CoreConfig& config) {
    dev_ = &dev;
    config_ = config;
    config_.slots = std::max<uint32_t>(config_.slots, 1);
    config_.command_kb = std::max<uint32_t>(Align(config_.command_kb, kCommandPiece >> 10), kCommandPiece >> 10);

    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, device());
    cm.userData = this;
    cm.cbAddMem = AddCommandMemory;
    cmd_ = dkCmdBufCreate(&cm);
    if (config_.pre_kb) {
        config_.pre_kb = std::max<uint32_t>(Align(config_.pre_kb, kCommandPiece >> 10), kCommandPiece >> 10);
        pre_ = dkCmdBufCreate(&cm);
    }

    constexpr uint32_t kUncached = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    const uint32_t n = config_.slots;
    command_memory_ = CreateBlock(n * (config_.command_kb << 10), kUncached, "command memory (per slot)");
    if (pre_) pre_memory_ = CreateBlock(n * (config_.pre_kb << 10), kUncached, "pre-list memory (per slot)");
    upload_ = CreateBlock(n * (config_.upload_mb << 20),
                          config_.upload_cached ? DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached : kUncached,
                          config_.upload_cached ? "upload ring (per slot, CPU cached)" : "upload ring (per slot)");
    upload_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(upload_));
    upload_gpu_ = dkMemBlockGetGpuAddr(upload_);
    code_ = CreateBlock(config_.code_mb << 20, kUncached | DkMemBlockFlags_Code, "shader code");
    descriptors_ = CreateBlock((config_.image_descriptors + config_.sampler_descriptors) * 32, kUncached,
                               "image and sampler descriptors");
    queries_ = CreateBlock(config_.query_kb << 10, kUncached, "GPU queries");
    queries_cpu_ = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(queries_));
    queries_gpu_ = dkMemBlockGetGpuAddr(queries_);
    std::memset(queries_cpu_, 0, config_.query_kb << 10);
    slots_.assign(n, Slot{});
    AddChunk(config_.image_chunk_mb << 20);
    Log("[dk] core ready: %u slots, %u MiB upload, %u KiB commands and %u KiB pre-list per slot, %u MiB code", n,
        config_.upload_mb, config_.command_kb, pre_ ? config_.pre_kb : 0, config_.code_mb);
}

void Core::Destroy() {
    if (!dev_) return;
    dev_->WaitIdle();
    if (cmd_) dkCmdBufDestroy(cmd_);
    if (pre_) dkCmdBufDestroy(pre_);
    for (Slot& s : slots_)
        for (SlotMemory* m : {&s.commands, &s.pre})
            for (auto& [block, bytes] : m->overflow) dev_->DestroyBlock(block);
    slots_.clear();
    for (Chunk& c : chunks_) dev_->DestroyBlock(c.block);
    chunks_.clear();
    for (DkMemBlock b : readback_blocks_) dev_->DestroyBlock(b);
    readback_blocks_.clear();
    for (DkMemBlock b : {command_memory_, pre_memory_, upload_, code_, descriptors_, queries_})
        if (b) dev_->DestroyBlock(b);
    dev_ = nullptr;
    cmd_ = pre_ = nullptr;
    command_memory_ = pre_memory_ = upload_ = code_ = descriptors_ = queries_ = nullptr;
}

// ---- Work slots

void Core::BeginWork(uint64_t work) {
    slot_ = uint32_t(work % config_.slots);
    Slot& s = slots_[slot_];
    if (s.fenced) {
        const uint64_t before = NowNs();
        for (int wait = 1;; ++wait) {
            if (dkFenceWait(&s.fence, 2'000'000'000ll) == DkResult_Success) break;
            dev_->CheckQueue("waiting for a slot's fence");
            Log("[dk] work %llu: the GPU has not finished slot %u after %d s", (unsigned long long)work, slot_, wait * 2);
            FlushLog();
        }
        fence_wait_ns_ += NowNs() - before;
    }
    for (const ImageMemory& m : s.to_free) ReturnToChunk(chunks_[size_t(m.chunk)], m.offset, m.bytes);
    s.to_free.clear();
    s.upload_used = s.upload_flushed = 0;
    // dkCmdBufClear rewinds to the start of the last piece given (deko3d 0.5.0), which belongs to another slot: the
    // slot's first piece is given again, as deko_examples' CCmdMemRing does
    dkCmdBufClear(cmd_);
    dkCmdBufAddMemory(cmd_, command_memory_, slot_ * (config_.command_kb << 10), kCommandPiece - kCommandSlack);
    s.commands.used = kCommandPiece;
    s.commands.overflow_used = 0;
    if (pre_) {
        dkCmdBufClear(pre_);
        dkCmdBufAddMemory(pre_, pre_memory_, slot_ * (config_.pre_kb << 10), kCommandPiece - kCommandSlack);
        s.pre.used = kCommandPiece;
        s.pre.overflow_used = 0;
    }
    pre_used_ = false;
    commands_this_work_ = kCommandPiece;
    open_ = true;
}

void Core::MoreCommandMemory(DkCmdBuf which, size_t minimum) {
    Slot& s = slots_[slot_];
    const bool pre = pre_ && which == pre_;
    SlotMemory& m = pre ? s.pre : s.commands;
    DkMemBlock memory = pre ? pre_memory_ : command_memory_;
    const uint32_t want = Align(uint32_t(std::max<size_t>(minimum + kCommandSlack, kCommandPiece)), DK_MEMBLOCK_ALIGNMENT);
    const uint32_t slice = (pre ? config_.pre_kb : config_.command_kb) << 10;
    if (m.used + want <= slice) {
        dkCmdBufAddMemory(which, memory, slot_ * slice + m.used, want - kCommandSlack);
        m.used += want;
        commands_this_work_ += want;
        return;
    }
    if (m.overflow_used == 0) ++stats_.command_overflows;
    while (m.overflow_used < m.overflow.size()) {
        const auto [block, bytes] = m.overflow[m.overflow_used++];
        if (bytes >= want) {
            dkCmdBufAddMemory(which, block, 0, bytes - kCommandSlack);
            commands_this_work_ += bytes;
            return;
        }
    }
    const uint32_t bytes = std::max<uint32_t>(want, 1u << 20);
    DkMemBlock block = CreateBlock(bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                   pre ? "pre-list overflow" : "command memory overflow");
    m.overflow.push_back({block, bytes});
    m.overflow_used = m.overflow.size();
    dkCmdBufAddMemory(which, block, 0, bytes - kCommandSlack);
    commands_this_work_ += bytes;
}

DkCmdList Core::ClosePre() {
    if (!pre_ || !pre_used_) return 0;
    // uploads (copy engine), descriptor writes and code are done and visible before cmd() runs
    dkCmdBufBarrier(pre_, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
    pre_used_ = false;
    return dkCmdBufFinishList(pre_);
}

void Core::FlushUploads() {
    Slot& s = slots_[slot_];
    if (!config_.upload_cached || s.upload_used <= s.upload_flushed) return;
    const uint32_t base = slot_ * (config_.upload_mb << 20);
    dkMemBlockFlushCpuCache(upload_, base + s.upload_flushed, s.upload_used - s.upload_flushed);
    s.upload_flushed = s.upload_used;
}

void Core::EndWork() {
    FlushUploads();
    Slot& s = slots_[slot_];
    const DkCmdList pre = ClosePre();
    dkCmdBufSignalFence(cmd_, &s.fence, true);
    s.fenced = true;
    if (pre) dev_->Submit(pre, dkCmdBufFinishList(cmd_), "submitting a work");
    else dev_->Submit(dkCmdBufFinishList(cmd_), "submitting a work");
    open_ = false;
    ++stats_.works;
    stats_.upload_bytes += s.upload_used;
    stats_.command_bytes_max = std::max(stats_.command_bytes_max, commands_this_work_);
}

void Core::SubmitPartial() {
    FlushUploads();
    const DkCmdList pre = ClosePre();
    if (pre) dev_->Submit(pre, dkCmdBufFinishList(cmd_), "a partial submit");
    else dev_->Submit(dkCmdBufFinishList(cmd_), "a partial submit");
}

bool Core::WorkDone(uint64_t work) {
    Slot& s = slots_[work % config_.slots];
    return !s.fenced || dkFenceWait(&s.fence, 0) == DkResult_Success;
}

Upload Core::Reserve(uint32_t bytes, uint32_t alignment) {
    Slot& s = slots_[slot_];
    const uint32_t slice = config_.upload_mb << 20;
    const uint32_t offset = Align(s.upload_used, std::max<uint32_t>(alignment, 4));
    if (!open_ || uint64_t(offset) + bytes > slice) {
        if (stats_.upload_full++ < 20)
            Log("[dk] upload: %u bytes do not fit in slot %u (%u of %u MiB used)%s", bytes, slot_, s.upload_used >> 20,
                config_.upload_mb, open_ ? "" : " (outside a work)");
        return {};
    }
    s.upload_used = offset + bytes;
    const uint32_t at = slot_ * slice + offset;
    return {upload_cpu_ + at, upload_gpu_ + at};
}

uint32_t Core::UploadFree() const {
    if (!open_) return 0;
    const uint32_t slice = config_.upload_mb << 20, used = slots_[slot_].upload_used;
    return used < slice ? slice - used : 0;
}

DkMemBlock Core::CreateReadbackBlock(uint32_t bytes, const char* name) {
    DkMemBlock b = CreateBlock(bytes, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached, name);
    readback_blocks_.push_back(b);
    return b;
}

// ---- Image heap: first fit in chunks, frees delayed until the slot's fence

void Core::AddChunk(uint32_t bytes) {
    char name[48];
    std::snprintf(name, sizeof(name), "image chunk %zu", chunks_.size());
    bytes = Align(bytes, DK_MEMBLOCK_ALIGNMENT);
    Chunk c;
    c.block = CreateBlock(bytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, name);
    c.bytes = bytes;
    c.free.push_back({0, bytes});
    chunks_.push_back(std::move(c));
    stats_.image_chunks = chunks_.size();
}

void Core::ReturnToChunk(Chunk& c, uint32_t offset, uint32_t bytes) {
    auto it = std::lower_bound(c.free.begin(), c.free.end(), std::make_pair(offset, 0u));
    it = c.free.insert(it, {offset, bytes});
    if (it + 1 != c.free.end() && it->first + it->second == (it + 1)->first) {
        it->second += (it + 1)->second;
        c.free.erase(it + 1);
    }
    if (it != c.free.begin() && (it - 1)->first + (it - 1)->second == it->first) {
        (it - 1)->second += it->second;
        c.free.erase(it);
    }
}

ImageMemory Core::ReserveImage(uint32_t bytes, uint32_t alignment) {
    alignment = std::max<uint32_t>(alignment, 256);
    bytes = Align(bytes, 256);
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < chunks_.size(); ++i) {
            Chunk& c = chunks_[i];
            for (size_t k = 0; k < c.free.size(); ++k) {
                const auto [start, size] = c.free[k];
                const uint32_t at = Align(start, alignment);
                if (uint64_t(at - start) + bytes > size) continue;
                c.free.erase(c.free.begin() + std::ptrdiff_t(k));
                if (at > start) ReturnToChunk(c, start, at - start);
                if (at + bytes < start + size) ReturnToChunk(c, at + bytes, start + size - at - bytes);
                stats_.image_bytes += bytes;
                return {c.block, at, bytes, int32_t(i)};
            }
        }
        AddChunk(std::max(config_.image_chunk_mb << 20, Align(bytes + alignment, DK_MEMBLOCK_ALIGNMENT)));
    }
    Fatal("[dk] the image heap cannot place %u bytes", bytes);
}

void Core::ReleaseImageLater(const ImageMemory& memory) {
    if (memory.chunk < 0) return;
    stats_.image_bytes -= memory.bytes;
    slots_[slot_].to_free.push_back(memory);
}

// ---- Shader code

bool Core::LoadShader(DkShader& shader, std::span<const uint8_t> dksh, const char* name) {
    // the DKSH header (uam's dksh.h): the control section, which starts with this header and stays in CPU memory,
    // then the code, which goes to code memory
    struct Header {
        uint32_t magic, header_bytes, control_bytes, code_bytes, programs_offset, programs;
    } h;
    if (dksh.size() < sizeof(h)) {
        Log("[dk] shader %s: %zu bytes, not a DKSH", name, dksh.size());
        return false;
    }
    std::memcpy(&h, dksh.data(), sizeof(h));
    if (h.magic != 0x48534B44u || h.programs != 1 || uint64_t(h.control_bytes) + h.code_bytes > dksh.size()) {
        Log("[dk] shader %s: not a one-program DKSH (magic 0x%08X, %u programs)", name, h.magic, h.programs);
        return false;
    }
    const uint32_t at = Align(code_used_, DK_SHADER_CODE_ALIGNMENT);
    if (uint64_t(at) + h.code_bytes > (config_.code_mb << 20) - DK_SHADER_CODE_UNUSABLE_SIZE) {
        Log("[dk] shader code memory full: %s (%u bytes) not loaded", name, h.code_bytes);
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(dkMemBlockGetCpuAddr(code_)) + at, dksh.data() + h.control_bytes, h.code_bytes);
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, code_, at);
    m.control = dksh.data();
    m.programId = 0;
    dkShaderInitialize(&shader, &m);
    if (!dkShaderIsValid(&shader)) {
        Log("[dk] shader %s: deko3d does not accept it", name);
        return false;
    }
    code_used_ = at + h.code_bytes;
    stats_.code_bytes = code_used_;
    code_new_ = true;
    return true;
}

void Core::InvalidateCodeIfNeeded() {
    if (!code_new_) return;
    dkCmdBufBarrier(cmd_, DkBarrier_None, DkInvalidateFlags_Shader);
    code_new_ = false;
}

DkGpuAddr Core::ImageDescriptors() const { return dkMemBlockGetGpuAddr(descriptors_); }
DkGpuAddr Core::SamplerDescriptors() const {
    return dkMemBlockGetGpuAddr(descriptors_) + DkGpuAddr(config_.image_descriptors) * 32;
}
void Core::BindDescriptors() {
    dkCmdBufBindImageDescriptorSet(cmd_, ImageDescriptors(), config_.image_descriptors);
    dkCmdBufBindSamplerDescriptorSet(cmd_, SamplerDescriptors(), config_.sampler_descriptors);
}

CoreStats Core::TakeStats() {
    const CoreStats s = stats_;
    stats_.works = stats_.upload_bytes = stats_.upload_full = stats_.command_bytes_max = stats_.command_overflows = 0;
    return s;
}

// ---- Images

void FixTileSize(DkImageLayoutMaker& m, uint32_t rows) {
    if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo)) return;
    if (m.type == DkImageType_1D || m.type == DkImageType_1DArray || m.type == DkImageType_3D || m.type == DkImageType_Buffer)
        return;  // 3D: deko3d never shrinks the depth tile at level 0
    if ((m.type == DkImageType_2DMS || m.type == DkImageType_2DMSArray) && (m.msMode == DkMsMode_4x || m.msMode == DkMsMode_8x))
        rows *= 2;  // the multisampled height
    // deko3d's choice (pickTileSize of 1.5 times the height in 8-row GOBs) ...
    const uint32_t gobs = (rows + rows / 2 + 7) / 8;
    const uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
    // ... and the shrink its layout applies to level 0 (calcLevelOffset), which the descriptor and engines do not
    uint32_t shrunk = tile;
    while (shrunk && (8u << (shrunk - 1)) >= rows) --shrunk;
    if (shrunk == tile) return;
    m.flags |= DkImageFlags_CustomTileSize;
    m.tileSize = DkTileSize(shrunk);
}

// ---- Presentation

Swapchain::~Swapchain() { Destroy(); }

void Swapchain::Create(Device& dev, uint32_t width, uint32_t height, uint32_t interval, NWindow* window) {
    Destroy();
    dev_ = &dev;
    width_ = width;
    height_ = height;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, dev.device());
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = width;
    lm.dimensions[1] = height;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    const uint32_t alignment = dkImageLayoutGetAlignment(&layout);
    const uint32_t bytes = Align(uint32_t(dkImageLayoutGetSize(&layout)), alignment);
    Log("[dk] swapchain: %u RGBA8 images of %ux%u", kImages, width, height);
    // nwindowSetDimensions is refused while buffers are registered: the window takes the size before the swapchain
    nwindowSetDimensions(window ? window : nwindowGetDefault(), width, height);
    memory_ = dev.CreateBlock(kImages * bytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "swapchain images");
    const DkImage* images[kImages];
    for (uint32_t i = 0; i < kImages; ++i) {
        dkImageInitialize(&images_[i], &layout, memory_, i * bytes);
        images[i] = &images_[i];
    }
    swapchain_ = dev.CreateSwapchain(window, images, kImages);
    dkSwapchainSetSwapInterval(swapchain_, interval);
}

void Swapchain::Destroy() {
    if (!swapchain_) return;
    dev_->WaitIdle();
    dev_->DestroySwapchain(swapchain_);
    dev_->DestroyBlock(memory_);
    swapchain_ = nullptr;
    memory_ = nullptr;
}

int Swapchain::Acquire() { return dev_->Acquire(swapchain_); }

void Swapchain::Present(int image) { dev_->Present(swapchain_, image); }

void ScreenSize(uint32_t& width, uint32_t& height) {
    const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    width = docked ? 1920 : 1280;
    height = docked ? 1080 : 720;
}

}  // namespace dk
