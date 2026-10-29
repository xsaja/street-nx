// deko3d renderer, the core: device, queue, memory, work slots, shader code and swapchain. Switch only.
// Adapted from NFSMW-NX's nfsmw_dk_nucleo (StevensND and contributors, GPL-3.0), names in English; its notes are
// docs/HowToDeko3d.md of that project (sections 2, 5.8-5.10 and 6).
//
// Device conventions: DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne, as Direct3D: row 0 of an image is
// the top and clip-space z goes from 0 to 1. Clip-space y points UP in deko3d 0.5.0 whatever the origin flag, as in
// Direct3D too, so the SDK's shaders need no flip; no viewport is ever given a negative height (deko3d computes it
// unsigned).
//
// Errors: deko3d's release library ends the process on any failed creation (result 2359-xxxx) without calling
// anything, so the log is flushed before each creation. A queue in an error state (a GPU fault) aborts every later
// submit, flush, acquire and present, so those check it first and stop with the reason in the log.
//
// Threads: one Device (device and queue) is shared, its methods take the queue lock. Each recording thread has its
// own Core: command buffer, work slots, upload ring, image heap, code memory and descriptors.
#pragma once

#include <deko3d.h>

#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

struct NWindow;

namespace dk {

// ---- Log: lines go to the function given (the game's log, or the test's file); without one, to stdout
using LogFn = void (*)(const char* line);
void SetLog(LogFn write, void (*flush)());
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
// before anything that may end the process (a creation, a submit): the lines written must reach the SD card
void FlushLog();
[[noreturn]] void Fatal(const char* format, ...) __attribute__((format(printf, 1, 2)));

// ---- Time
// the CPU counter (19.2 MHz) in nanoseconds
inline uint64_t NowNs() {
    uint64_t ticks;
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks * 625 / 12;
}
// GPU timestamps (dkCmdBufReportCounter DkCounter_Timestamp, the second word of the 16-byte report) count 1.627 ns a
// unit on the Switch: deko3d converts the 19.2 MHz timer as if it ran at 31.25 MHz
inline constexpr double kNsPerGpuUnit = 1.627;

// The device and its one queue. Every queue operation takes the lock.
class Device {
public:
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    // zcull: DkQueueFlags_EnableZcull. A failure ends the process.
    void Create(bool zcull);
    // waits for the GPU and destroys the queue and the device; safe twice
    void Destroy();
    DkDevice device() const { return device_; }

    // memory blocks and swapchains, under their own lock; the log is flushed first
    DkMemBlock CreateBlock(uint32_t bytes, uint32_t flags, const char* name);
    void DestroyBlock(DkMemBlock block);
    DkSwapchain CreateSwapchain(NWindow* window, const DkImage* const images[], uint32_t count);
    void DestroySwapchain(DkSwapchain swapchain);

    // submits command lists in order and flushes the queue; what names them for the error message
    void Submit(DkCmdList list, const char* what);
    void Submit(DkCmdList first, DkCmdList second, const char* what);
    void WaitIdle();
    int Acquire(DkSwapchain swapchain);
    void Present(DkSwapchain swapchain, int image);
    // stops the process if a GPU fault left the queue in an error state; the caller holds the lock
    void CheckQueue(const char* before) const;

private:
    DkDevice device_ = nullptr;
    DkQueue queue_ = nullptr;
    std::mutex lock_;
    std::mutex creations_;
};

struct CoreConfig {
    uint32_t slots = 3;              // work slots: the CPU records one while the GPU runs the others
    uint32_t upload_mb = 64;         // per slot: vertices, indices, constants, texture uploads
    bool upload_cached = false;      // the ring CPU cached, cleaned to memory before each submit
    uint32_t command_kb = 4096;      // per slot (it grows past this with extra blocks)
    uint32_t pre_kb = 0;             // per slot, the pre-list (Pre()); 0 = none
    uint32_t code_mb = 32;           // shader code (DKSH), bump allocated
    uint32_t image_chunk_mb = 16;    // image heap chunks
    uint32_t image_descriptors = 8192;
    uint32_t sampler_descriptors = 1024;
    uint32_t query_kb = 64;          // GPU reports: timestamps, occlusion
};

// a piece of the current slot's upload ring
struct Upload {
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
    explicit operator bool() const { return cpu != nullptr; }
};

// a piece of the image heap
struct ImageMemory {
    DkMemBlock block = nullptr;
    uint32_t offset = 0;
    uint32_t bytes = 0;
    int32_t chunk = -1;
    explicit operator bool() const { return block != nullptr; }
};

struct CoreStats {
    uint64_t works = 0;            // slots closed since the last call
    uint64_t upload_bytes = 0;     // upload ring used, summed over those slots
    uint64_t upload_full = 0;      // reservations that did not fit
    uint64_t command_bytes_max = 0;
    uint64_t command_overflows = 0;
    uint64_t image_bytes = 0;      // image heap in use (totals, not reset)
    uint64_t image_chunks = 0;
    uint64_t code_bytes = 0;
};

class Core {
public:
    Core() = default;
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    ~Core();

    void Create(Device& device, const CoreConfig& config);
    // waits for the GPU and frees everything; safe twice
    void Destroy();

    DkDevice device() const { return dev_ ? dev_->device() : nullptr; }
    Device& dev() const { return *dev_; }
    DkCmdBuf cmd() const { return cmd_; }
    // the slot's pre-list: runs before everything in cmd() of the same submission, and its end waits for every
    // engine and invalidates the image, descriptor and shader caches (uploads and descriptor writes go here). Only
    // with CoreConfig::pre_kb.
    DkCmdBuf Pre() {
        pre_used_ = true;
        return pre_;
    }
    const CoreConfig& config() const { return config_; }

    // ---- Work slots
    // starts recording work number `work` in slot work % slots: waits for that slot's previous fence, frees the
    // images released while it ran, rewinds its upload ring and command memory
    void BeginWork(uint64_t work);
    // signals the slot's fence after everything recorded, submits and flushes
    void EndWork();
    bool WorkOpen() const { return open_; }
    bool WorkDone(uint64_t work);
    uint64_t FenceWaitNs() const { return fence_wait_ns_; }
    // submits what is recorded so far in the middle of a slot (without its fence) and keeps recording
    void SubmitPartial();
    // a cached ring: what the CPU wrote since the last clean reaches memory
    void FlushUploads();
    void WaitGpu() { dev_->WaitIdle(); }

    // ---- Memory
    // bytes of the current slot's upload ring (CPU-uncached: write once, in order, never read back); empty when full
    Upload Reserve(uint32_t bytes, uint32_t alignment);
    uint32_t UploadFree() const;
    ImageMemory ReserveImage(uint32_t bytes, uint32_t alignment);
    // back to the heap once the GPU has finished the slot being recorded now
    void ReleaseImageLater(const ImageMemory& memory);
    // memory the CPU reads back (CPU-cached, GPU-uncached): dkMemBlockFlushCpuCache before the GPU writes and again
    // before reading
    DkMemBlock CreateReadbackBlock(uint32_t bytes, const char* name);

    // copies a DKSH's code into code memory and initializes `shader`; false (logged) if it does not fit or is not a
    // DKSH. Code loaded in the middle of a frame is seen by the GPU only after InvalidateCodeIfNeeded.
    bool LoadShader(DkShader& shader, std::span<const uint8_t> dksh, const char* name);
    bool code_new() const { return code_new_; }
    void InvalidateCodeIfNeeded();

    // image and sampler descriptor tables, written with dkCmdBufPushData only (ordered with the commands)
    DkGpuAddr ImageDescriptors() const;
    DkGpuAddr SamplerDescriptors() const;
    void BindDescriptors();

    // query_kb of CPU-uncached memory for dkCmdBufReportCounter (16 bytes a report)
    uint8_t* QueriesCpu() const { return queries_cpu_; }
    DkGpuAddr QueriesGpu() const { return queries_gpu_; }

    CoreStats TakeStats();
    // deko3d's command buffers call this when they run out of memory (cbAddMem)
    void MoreCommandMemory(DkCmdBuf which, size_t minimum);

private:
    struct SlotMemory {
        uint32_t used = 0;
        std::vector<std::pair<DkMemBlock, uint32_t>> overflow;  // blocks made when the slice ran out, kept
        size_t overflow_used = 0;
    };
    struct Slot {
        DkFence fence{};
        bool fenced = false;
        uint32_t upload_used = 0;
        uint32_t upload_flushed = 0;  // cleaned up to here
        SlotMemory commands, pre;
        std::vector<ImageMemory> to_free;
    };
    struct Chunk {
        DkMemBlock block = nullptr;
        uint32_t bytes = 0;
        std::vector<std::pair<uint32_t, uint32_t>> free;  // offset, bytes; sorted by offset
    };

    DkMemBlock CreateBlock(uint32_t bytes, uint32_t flags, const char* name);
    DkCmdList ClosePre();
    void AddChunk(uint32_t bytes);
    static void ReturnToChunk(Chunk& c, uint32_t offset, uint32_t bytes);

    CoreConfig config_;
    Device* dev_ = nullptr;
    DkCmdBuf cmd_ = nullptr;
    DkCmdBuf pre_ = nullptr;
    bool pre_used_ = false;
    uint64_t fence_wait_ns_ = 0;
    DkMemBlock pre_memory_ = nullptr;
    DkMemBlock upload_ = nullptr, command_memory_ = nullptr, code_ = nullptr, descriptors_ = nullptr,
               queries_ = nullptr;
    std::vector<DkMemBlock> readback_blocks_;
    uint8_t* upload_cpu_ = nullptr;
    DkGpuAddr upload_gpu_ = 0;
    uint8_t* queries_cpu_ = nullptr;
    DkGpuAddr queries_gpu_ = 0;
    std::vector<Slot> slots_;
    uint32_t slot_ = 0;
    bool open_ = false;
    uint32_t code_used_ = 0;
    bool code_new_ = false;
    std::vector<Chunk> chunks_;
    uint64_t commands_this_work_ = 0;
    CoreStats stats_;
};

// ---- Images
// Before dkImageLayoutInitialize of a block-linear 2D image whose level 0 is `rows` tall (compressed formats: rows
// of blocks): deko3d 0.5.0 gives images of 6 to 8 rows of 8-row blocks a tile height that its layout and its
// descriptor disagree on, and the second column of blocks of level 0 lands on mip 1. This sets the tile its level 0
// really uses (SwitchWakerHD's image_tile_size_fix).
void FixTileSize(DkImageLayoutMaker& m, uint32_t rows);

// ---- Presentation
class Swapchain {
public:
    static constexpr uint32_t kImages = 3;
    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    ~Swapchain();

    // kImages RGBA8 images of that size on `window` (nullptr: the default window), in their own memory block.
    // interval 0 presents at once, 1 waits for vsync
    void Create(Device& dev, uint32_t width, uint32_t height, uint32_t interval, NWindow* window = nullptr);
    void Destroy();
    int Acquire();
    void Present(int image);
    DkImage& Image(int i) { return images_[i]; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool created() const { return swapchain_ != nullptr; }

private:
    Device* dev_ = nullptr;
    DkSwapchain swapchain_ = nullptr;
    DkMemBlock memory_ = nullptr;
    DkImage images_[kImages];
    uint32_t width_ = 0, height_ = 0;
};

// 1920x1080 docked, 1280x720 handheld
void ScreenSize(uint32_t& width, uint32_t& height);

}  // namespace dk
