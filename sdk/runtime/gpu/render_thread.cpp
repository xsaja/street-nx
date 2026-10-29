// The Direct3D 12 backend on a thread of its own. The title's render thread records each call with what the console
// takes at call time (state changes, resource headers, indices, rects) and goes on; vertex, texture and surface memory
// are read when the draw runs, as the console's GPU reads them after the call returns. The XDK's fences, busy checks
// and locks wait for that.
#ifdef __SWITCH__
// the Switch (ARM): NEON in place of SSE, the thread unnamed (Black-Android's src/d3d/render_thread.cpp)
#include <arm_neon.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <smmintrin.h>
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

namespace {

// commands handed over at once, so the render thread keeps close behind and a wait is short
constexpr uint32_t kBatchCommands = 64;

// resource headers the render thread reads: common, data, lock, format, size
constexpr uint32_t kHeaderBytes = 20, kHeaderSlot = 32;
constexpr uint32_t kHeaderRing = 1u << 20;  // guest memory for header copies; a copy is renewed after half a ring

enum class WaitFor : uint8_t { ReadBack, Resource, Fence, Flush, Copy, Frame, Count };

enum class Op : uint8_t { State, Draw, Clear, Copy, Present, Sync, RecordLate, Flush, Gamma, ClearReport, GetReport };

struct Cmd {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    float f = 0;
    uint32_t data = 0, size = 0;  // bytes in Batch::bytes
    int32_t vs = -1, ps = -1;     // index in Batch::vs / ps: the shader changed
    Surface s1, s2;
};

// a draw's vertex copies: each stream's span and where its bytes are in Batch::bytes
struct DrawCopies {
    std::array<StreamSpan, 16> spans;
    std::array<uint32_t, 16> at;
};

// grows without zeroing: every byte added is written at once
template <class T>
struct NoInit : std::allocator<T> {
    template <class U>
    struct rebind {
        using other = NoInit<U>;
    };
    template <class U>
    void construct(U* p) noexcept {
        ::new (static_cast<void*>(p)) U;
    }
    template <class U, class... A>
    void construct(U* p, A&&... a) {
        ::new (static_cast<void*>(p)) U(std::forward<A>(a)...);
    }
};

struct Batch {
    uint64_t id = 0;
    std::vector<Cmd> cmds;
    std::vector<uint8_t, NoInit<uint8_t>> bytes;
    std::vector<DrawCopies> copies;  // Op::Draw with size != 0 uses copies[size - 1]
    std::vector<std::shared_ptr<VertexShader>> vs;
    std::vector<std::shared_ptr<PixelShader>> ps;
};

// the plain data in State, around its shader pointers and the resource headers (sent as copies); of the transforms only
// view, projection, texture and world are drawn with
struct Range {
    size_t off, len;
};
size_t At(const State& s, const void* p) { return size_t(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(&s)); }
std::vector<Range> StateRanges() {
    static const State s;
    return {
        {At(s, &s.simple), At(s, &s.textures) - At(s, &s.simple)},
        {At(s, &s.index_buffer), At(s, &s.vs) - At(s, &s.index_buffer)},
        {At(s, &s.vsc), At(s, &s.rt) - At(s, &s.vsc)},
        {At(s, &s.scissor_count), At(s, &s.transforms) - At(s, &s.scissor_count)},
        {At(s, &s.transforms[2]), 2 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[16]), 4 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[256]), sizeof(s.transforms[0])},
        {At(s, &s.material), sizeof(State) - At(s, &s.material)},
    };
}

// equal bytes, 16 at a time: most of the state is compared on every draw
bool Same(const uint8_t* a, const uint8_t* b, size_t n) {
    size_t i = 0;
#ifdef __SWITCH__
    uint8x16_t diff = vdupq_n_u8(0);
    for (; i + 16 <= n; i += 16) diff = vorrq_u8(diff, veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i)));
    if (vmaxvq_u8(diff)) return false;
#else
    __m128i diff = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16)
        diff = _mm_or_si128(diff, _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
                                                _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i))));
    if (!_mm_testz_si128(diff, diff)) return false;
#endif
    return i == n || !std::memcmp(a + i, b + i, n - i);
}

using Clock = std::chrono::steady_clock;
uint64_t Ns(Clock::time_point t) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count()); }

#ifdef __SWITCH__
extern "C" void street_profile_thread(const char* name) __attribute__((weak));  // switch/profiler.cpp
#endif

class Threaded final : public Backend {
public:
    explicit Threaded(std::unique_ptr<Backend> inner) : inner_(std::move(inner)), ranges_(StateRanges()) {
        ring_ = mem::AllocSystem(kHeaderRing, 64);
        if (std::getenv("STREET_TRACK_STATE")) g_track_state = true;  // off the Switch: on by the environment
        TouchAllState();
        cur_ = NewBatch();
        std::thread([this] { Run(); }).detach();
    }
    ~Threaded() override {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock(m_);
        // at exit the worker may be gone mid-batch: then the inner backend is left, not freed under it
        if (!done_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return exited_; })) (void)inner_.release();
    }

    bool Init(uint32_t width, uint32_t height) override { return inner_->Init(width, height); }

    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        auto t = Clock::now();
        g_perf_draws++;
        SendState(s);
        Cmd c{Op::Draw};
        c.a = prim;
        c.b = start;
        c.c = count;
        if (indices) {
            c.d = 1;
            c.data = Put(indices, size_t(count) * 2);
        }
        // STREET: the vertices taken now, so the title may reuse their memory at once (STREET_DRAW_COPY=1 off the Switch)
        static const bool env_copy = [] {
            if (std::getenv("STREET_DRAW_COPY")) g_draw_copy = true;
            return true;
        }();
        (void)env_copy;
        bool copied = g_draw_copy.load(std::memory_order_relaxed) && CopyVertices(s, c, indices);
        cur_->cmds.push_back(c);
        // the batch reads these resources: once per batch for the ones the draw before in it already marked
        std::array<uint32_t, 26> used;
        for (size_t i = 0; i < 4; i++) used[i] = s.textures[i], used[4 + i] = s.palettes[i];
        for (size_t i = 0; i < 16; i++) used[8 + i] = s.streams[i].vb;
        used[24] = s.rt.header;
        used[25] = s.zs.header;
        if (cur_->id != used_batch_ || used != used_) {
            for (uint32_t h : used) Use(h);
            used_ = used;
            used_batch_ = cur_->id;
        }
        Written(s.rt);
        if (cur_->cmds.size() >= kBatchCommands) Submit();
        // Diagnostic only: distinguish reused guest vertex memory from decode/state errors.
        static const bool sync_draws = std::getenv("STREET_SYNC_DRAWS") != nullptr;
        // The validated STREET path is also the default for direct EXE launches.
        static const bool consume_draws = !sync_draws;
        if (consume_draws && !copied) {
            // Wait for the worker to copy guest data into host upload buffers,
            // without forcing GPU completion after every draw.
            static bool reported = false;
            if (!reported) { LOGR("STREET: draw-consumption diagnostic enabled (no per-draw GPU flush)"); reported = true; }
            Wait(Submit(), WaitFor::Resource, "draw consumption");
        } else if (sync_draws) {
            static bool reported = false;
            if (!reported) { LOGR("STREET: synchronous draw diagnostic enabled"); reported = true; }
            Flush();
        }
        g_perf_capture_ns += Ns(t);
    }

    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        auto t = Clock::now();
        SendState(s);
        Cmd c{Op::Clear};
        c.a = count;
        c.b = flags;
        c.c = color;
        c.d = stencil;
        c.f = z;
        if (count && rects) {
            c.data = Put(rects, size_t(count) * 16);
            c.size = count * 16;
        }
        cur_->cmds.push_back(c);
        Use(s.rt.header);
        Use(s.zs.header);
        if (flags & kClearTarget) Written(s.rt);
        g_perf_capture_ns += Ns(t);
    }

    // rects and points are taken now; the copy itself, CPU or GPU, runs in order like the console's
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override {
        auto t = Clock::now();
        Cmd c{Op::Copy};
        c.a = count;
        c.b = count && src_rects ? Scratch(kernel::HostPtr(src_rects), count * 16) : 0;
        c.c = count && dst_points ? Scratch(kernel::HostPtr(dst_points), count * 8) : 0;
        c.s1 = src;
        c.s1.header = Header(src.header);
        c.s2 = dst;
        c.s2.header = Header(dst.header);
        cur_->cmds.push_back(c);
        Use(src.header);
        Use(dst.header);
        Written(dst);
        g_perf_capture_ns += Ns(t);
    }

    void Present(const Surface& front) override {
        auto t = Clock::now();
        Cmd c{Op::Present};
        c.s1 = front;
        c.s1.header = Header(front.header);
        cur_->cmds.push_back(c);
        Use(front.header);
        uint64_t id = Submit();
        g_perf_capture_ns += Ns(t);
        // the title records one frame while the one before it draws
        Wait(last_present_, WaitFor::Frame, nullptr);
        last_present_ = id;
    }

    void Sync(uint32_t data, uint32_t) override { ReadBack(data, false); }

    void SyncLate(uint32_t data) override {
        if (!inner_->DeliverLate(data)) {
            if (diag::Perf()) LOG_FIRST(16, "render thread: no finished copy of %08X for a NOOVERWRITE lock, reading it back now", data);
            written_.insert(data);
            ReadBack(data, true);
        }
        Cmd c{Op::RecordLate};
        c.a = data;
        cur_->cmds.push_back(c);
    }

    void Flush() override {
        cur_->cmds.push_back(Cmd{Op::Flush});
        Wait(Submit(), WaitFor::Flush, "flush");
    }

    void SetGamma(const uint8_t* ramp) override {
        Cmd c{Op::Gamma};
        c.data = Put(ramp, 768);
        cur_->cmds.push_back(c);
    }

    void ClearReport() override { cur_->cmds.push_back(Cmd{Op::ClearReport}); }
    void GetReport(uint32_t report) override {
        Cmd c{Op::GetReport};
        c.a = report;
        cur_->cmds.push_back(c);
    }

    uint32_t Fence() override { return uint32_t(cur_->id); }
    bool FencePending(uint32_t fence) override { return fence > done_.load(); }
    void WaitFence(uint32_t fence) override { Wait(fence, WaitFor::Fence, "fence"); }
    bool Busy(uint32_t resource) override {
        auto it = last_use_.find(resource);
        return it != last_use_.end() && it->second > done_.load();
    }
    void WaitResource(uint32_t resource) override {
        auto it = last_use_.find(resource);
        if (it != last_use_.end()) Wait(it->second, WaitFor::Resource, "resource");
    }

private:
    Batch* NewBatch() {
        Batch* b;
        {
            std::lock_guard lock(m_);
            if (free_.empty()) {
                b = new Batch;
            } else {
                b = free_.back();
                free_.pop_back();
            }
        }
        b->id = ++next_id_;
        return b;
    }

    // the draw's vertex spans into the batch; false (nothing taken) when one is too big or not in guest memory
    bool CopyVertices(const State& s, Cmd& c, const uint8_t* indices) {
        constexpr uint32_t kMaxCopy = 1u << 20;
        DrawCopies dc;
        if (!DrawStreamSpans(s, c.a, c.b, c.c, indices, dc.spans)) return false;
        uint64_t total = 0;
        for (const StreamSpan& sp : dc.spans) {
            if (!sp.bytes) continue;
            total += sp.bytes;
            if (total > kMaxCopy || uint64_t(sp.from) + sp.bytes > (1ull << 32) || !mem::IsCommitted(sp.from) ||
                !mem::IsCommitted(sp.from + sp.bytes - 1))
                return false;
        }
        for (size_t i = 0; i < 16; i++)
            dc.at[i] = dc.spans[i].bytes ? Put(kernel::HostPtr(dc.spans[i].from), dc.spans[i].bytes) : 0;
        cur_->copies.push_back(dc);
        c.size = uint32_t(cur_->copies.size());
        g_perf_copied++;
        g_perf_copied_bytes += total;
        return true;
    }

    uint32_t Put(const void* p, size_t n) {
        size_t at = (cur_->bytes.size() + 3) & ~size_t(3);
        cur_->bytes.resize(at + n);
        std::memcpy(cur_->bytes.data() + at, p, n);
        return uint32_t(at);
    }

    void ReadBack(uint32_t data, bool late) {
        // only surfaces the GPU wrote since the last read-back hold anything newer than guest memory
        if (data == last_written_) last_written_ = 0;
        if (!written_.erase(data)) return;
        (late ? g_perf_sync_late : g_perf_sync_block)++;
        if (diag::Perf() && logged_.insert(uint64_t(data) << 1 | (late ? 1 : 0)).second)
            LOGI("render thread: read-back of %08X for %s", data, late ? "a NOOVERWRITE lock without a finished copy" : "a lock / BlockOnResource");
        Cmd c{Op::Sync};
        c.a = data;
        cur_->cmds.push_back(c);
        Wait(Submit(), WaitFor::ReadBack, "read-back");
    }

    // bytes the render thread reads through a guest address, in the header ring
    uint32_t Scratch(const void* p, uint32_t n) {
        uint32_t size = (n + kHeaderSlot - 1) & ~(kHeaderSlot - 1);
        if (ring_pos_ % kHeaderRing + size > kHeaderRing) ring_pos_ += kHeaderRing - ring_pos_ % kHeaderRing;
        uint32_t at = ring_ + uint32_t(ring_pos_ % kHeaderRing);
        ring_pos_ += size;
        std::memcpy(kernel::HostPtr(at), p, n);
        return at;
    }

    // A header as it is now. The console's push buffer takes a texture's format and address, and a surface's or vertex
    // buffer's address, when they are set; the title points the same header elsewhere before the GPU gets there.
    uint32_t Header(uint32_t header) {
        if (!header || !ring_) return header;
        // vertex buffer, index buffer and palette headers are 12 bytes: reading 20 can run off the end of the heap
        const uint32_t type = kernel::R32(header) & 0x70000;  // 0 vertex, 0x10000 index, 0x30000 palette
        uint8_t now[kHeaderBytes] = {};
        std::memcpy(now, kernel::HostPtr(header), type < 0x20000 || type == 0x30000 ? 12 : kHeaderBytes);
        Recent<HeaderCopy>& r = recent_headers_[RecentSlot(header)];
        HeaderCopy& c = r.key == header ? *r.value : headers_[header];
        r = {header, &c};
        if (c.made && !std::memcmp(c.bytes, now, kHeaderBytes) && ring_pos_ - c.made < kHeaderRing / 2) return c.at;
        c.at = Scratch(now, kHeaderBytes);
        c.made = ring_pos_;
        std::memcpy(c.bytes, now, kHeaderBytes);
        return c.at;
    }

    // a field of the render thread's State, when it differs from what was sent
    void Delta(const void* field, const void* value, size_t n) {
        uint8_t* sent = reinterpret_cast<uint8_t*>(&sent_) + At(sent_, field);
        if (Same(sent, static_cast<const uint8_t*>(value), n)) return;
        std::memcpy(sent, value, n);
        uint32_t head[2] = {uint32_t(At(sent_, field)), uint32_t(n)};
        size_t at = cur_->bytes.size();
        cur_->bytes.resize(at + 8 + n);
        std::memcpy(cur_->bytes.data() + at, head, 8);
        std::memcpy(cur_->bytes.data() + at + 8, value, n);
    }

    // what changed since the last command, in 64 byte pieces
    void SendState(const State& s) {
        const uint8_t* now = reinterpret_cast<const uint8_t*>(&s);
        Cmd c{Op::State};
        size_t first = cur_->bytes.size();
        // STREET: blocks of simple and vsc nobody wrote since the last send are equal to what was sent
        const bool track = g_track_state;
        const uint8_t* sent = reinterpret_cast<const uint8_t*>(&sent_);
        if (track) {
            // straight to the written blocks, each cut to the ranges it overlaps
            for (size_t w = 0; w < std::size(g_state_dirty); w++)
                for (uint64_t bits = g_state_dirty[w]; bits; bits &= bits - 1) {
                    size_t b0 = (w * 64 + size_t(std::countr_zero(bits))) * 64, b1 = b0 + 64;
                    for (const Range& r : ranges_) {
                        size_t lo = std::max(b0, r.off), hi = std::min(b1, r.off + r.len);
                        if (lo < hi) Delta(sent + lo, now + lo, hi - lo);
                    }
                }
        } else {
            for (const Range& r : ranges_)
                for (size_t off = r.off, end = r.off + r.len; off < end; off += 64)
                    Delta(sent + off, now + off, std::min<size_t>(64, end - off));
        }
        // STREET: with state tracking a group of headers is read again only when the title set it since the last send,
        // as the console takes a header's fields when it is set (and its copies before half the ring has gone by):
        // reading each bound header from guest memory on every draw was ~4.5 % of the title thread on the Switch
        auto stale = [&](int g, const void* field, size_t n) {
            if (!track || StateDirty(At(s, field), n) || ring_pos_ - groups_read_[g] >= kHeaderRing / 4) {
                groups_read_[g] = ring_pos_;
                return true;
            }
            return false;
        };
        const bool read_textures = stale(0, &s.textures, sizeof(s.textures));
        const bool read_palettes = stale(1, &s.palettes, sizeof(s.palettes));
        const bool read_streams = stale(2, &s.streams, sizeof(s.streams));
        const bool read_targets = stale(3, &s.rt, sizeof(s.rt)) | stale(4, &s.zs, sizeof(s.zs));
        CleanState();
        if (read_textures) {
            std::array<uint32_t, 4> textures;
            for (size_t i = 0; i < 4; i++) textures[i] = Header(s.textures[i]);
            Delta(&sent_.textures, &textures, sizeof(textures));
        }
        if (read_palettes) {
            std::array<uint32_t, 4> palettes;
            for (size_t i = 0; i < 4; i++) palettes[i] = Header(s.palettes[i]);
            Delta(&sent_.palettes, &palettes, sizeof(palettes));
        }
        if (read_streams) {
            auto streams = s.streams;
            for (auto& st : streams) st.vb = Header(st.vb);
            Delta(&sent_.streams, &streams, sizeof(streams));
        }
        if (read_targets) {
            Surface rt = s.rt, zs = s.zs;
            rt.header = Header(rt.header);
            zs.header = Header(zs.header);
            Delta(&sent_.rt, &rt, sizeof(rt));
            Delta(&sent_.zs, &zs, sizeof(zs));
        }
        if (s.vs.get() != sent_.vs.get()) {
            sent_.vs = s.vs;
            c.vs = int32_t(cur_->vs.size());
            cur_->vs.push_back(s.vs);
        }
        if (s.ps.get() != sent_.ps.get()) {
            sent_.ps = s.ps;
            c.ps = int32_t(cur_->ps.size());
            cur_->ps.push_back(s.ps);
        }
        if (cur_->bytes.size() == first && c.vs < 0 && c.ps < 0) return;
        c.data = uint32_t(first);
        c.size = uint32_t(cur_->bytes.size() - first);
        cur_->cmds.push_back(c);
    }

    // resources by guest header: the batch that last read them
    void Use(uint32_t header) {
        if (!header) return;
        Recent<uint64_t>& r = recent_uses_[RecentSlot(header)];
        if (r.key != header) r = {header, &last_use_[header]};
        *r.value = cur_->id;
    }
    void Written(const Surface& s) {
        if (!s.header) return;
        uint32_t data = mem::VirtualFromPhysical(s.data);
        if (data == last_written_) return;  // draws into the same target, one after another
        written_.insert(data);
        last_written_ = data;
    }

    uint64_t Submit() {
        uint64_t id = cur_->id;
        {
            std::lock_guard lock(m_);
            queue_.push_back(cur_);
        }
        cv_.notify_one();
        cur_ = NewBatch();
        return id;
    }

    void Wait(uint64_t id, WaitFor kind, const char* why) {
        if (!id || done_.load() >= id) return;
        if (id >= cur_->id) id = Submit();
        auto t = Clock::now();
        // g_render_spin_us: poll a while first, the worker is often a few microseconds from done
        bool done = false;
        if (uint32_t spin = g_render_spin_us.load(std::memory_order_relaxed)) {
            auto until = t + std::chrono::microseconds(spin);
            while (!(done = done_.load() >= id) && Clock::now() < until) std::this_thread::yield();
        }
        if (!done) {
            std::unique_lock lock(m_);
            done_cv_.wait(lock, [&] { return done_.load() >= id; });
        }
        uint64_t ns = Ns(t);
        g_perf_wait_ns += ns;
        g_perf_wait_kind_ns[size_t(kind)] += ns;
        if (why) {
            g_perf_waits++;
            if (diag::Perf()) LOG_FIRST(32, "render thread: the title waited %.2f ms for a %s", ns / 1e6, why);
        }
    }

    void Run() {
#ifndef __SWITCH__
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto set = reinterpret_cast<SetDescription>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
            set(GetCurrentThread(), L"render");
#else
        if (street_profile_thread) street_profile_thread("render");
#endif
        for (;;) {
            Batch* b;
            // g_render_spin_us: poll a while for the next batch before sleeping
            if (uint32_t spin = g_render_spin_us.load(std::memory_order_relaxed)) {
                auto until = Clock::now() + std::chrono::microseconds(spin);
                while (Clock::now() < until) {
                    {
                        std::lock_guard lock(m_);
                        if (stop_ || !queue_.empty()) break;
                    }
                    std::this_thread::yield();
                }
            }
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) break;
                b = queue_.front();
                queue_.pop_front();
            }
            auto t = Clock::now();
            for (const Cmd& c : b->cmds) Execute(*b, c);
            g_perf_render_ns += Ns(t);
            uint64_t id = b->id;
            b->cmds.clear();
            b->bytes.clear();
            b->copies.clear();
            b->vs.clear();
            b->ps.clear();
            {
                std::lock_guard lock(m_);
                free_.push_back(b);
                done_.store(id);
            }
            done_cv_.notify_all();
        }
        std::lock_guard lock(m_);
        exited_ = true;
        done_cv_.notify_all();
    }

    void Execute(const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.data() + c.data;
        switch (c.op) {
            case Op::State: {
                uint8_t* s = reinterpret_cast<uint8_t*>(&state_);
                for (const uint8_t* p = data; p < data + c.size;) {
                    uint32_t head[2];
                    std::memcpy(head, p, 8);
                    std::memcpy(s + head[0], p + 8, head[1]);
                    p += 8 + head[1];
                }
                if (c.vs >= 0) state_.vs = b.vs[size_t(c.vs)];
                if (c.ps >= 0) state_.ps = b.ps[size_t(c.ps)];
                break;
            }
            case Op::Draw:
                if (c.size) {
                    // the vertices as they were when the draw was made
                    const DrawCopies& dc = b.copies[c.size - 1];
                    std::array<StreamCopy, 16> copies;
                    for (size_t i = 0; i < 16; i++)
                        copies[i] = {dc.spans[i], dc.spans[i].bytes ? b.bytes.data() + dc.at[i] : nullptr};
                    UseStreamCopies(&copies);
                    inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr);
                    UseStreamCopies(nullptr);
                } else {
                    inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr);
                }
                break;
            case Op::Clear: inner_->Clear(state_, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d); break;
            case Op::Copy: inner_->Copy(c.s1, c.s2, c.a, c.b, c.c); break;
            case Op::Present: inner_->Present(c.s1); break;
            case Op::Sync: inner_->Sync(c.a, 0); break;
            case Op::RecordLate: inner_->RecordLate(c.a); break;
            case Op::Flush: inner_->Flush(); break;
            case Op::Gamma: inner_->SetGamma(data); break;
            case Op::ClearReport: inner_->ClearReport(); break;
            case Op::GetReport: inner_->GetReport(c.a); break;
        }
    }

    std::unique_ptr<Backend> inner_;
    std::vector<Range> ranges_;

    // the title's thread
    struct HeaderCopy {
        uint32_t at = 0;
        uint64_t made = 0;  // ring_pos_ after it was written
        uint8_t bytes[kHeaderBytes];
    };
    std::unordered_map<uint32_t, HeaderCopy> headers_;
    // the last few lookups, in front of the maps (their elements stay put)
    template <typename T>
    struct Recent {
        uint32_t key = 0;
        T* value = nullptr;
    };
    // STREET: 1024 slots by a multiplicative hash; (header >> 3) & 63 put the headers of one pool in a few slots
    static size_t RecentSlot(uint32_t header) { return (header * 2654435761u) >> 22; }
    std::array<Recent<HeaderCopy>, 1024> recent_headers_{};
    std::array<Recent<uint64_t>, 1024> recent_uses_{};
    uint32_t ring_ = 0;
    uint64_t ring_pos_ = 0;
    Batch* cur_ = nullptr;
    State sent_;  // the state as the render thread will have it
    uint64_t groups_read_[5] = {};  // ring_pos_ when textures, palettes, streams, rt, zs headers were last read
    std::array<uint32_t, 26> used_{};  // the resources the last draw marked used, in batch used_batch_
    uint64_t used_batch_ = 0;
    std::unordered_map<uint32_t, uint64_t> last_use_;
    std::unordered_set<uint32_t> written_;  // surfaces drawn since their last read-back, by data address
    uint32_t last_written_ = 0;             // in written_ already
    std::unordered_set<uint64_t> logged_;   // perf diagnostic: read-backs named once, by address and kind
    uint64_t next_id_ = 0, last_present_ = 0;

    // the render thread
    State state_;

    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<Batch*> queue_;
    std::vector<Batch*> free_;
    std::atomic<uint64_t> done_{0};
    bool stop_ = false, exited_ = false;
};

}  // namespace

std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) { return std::make_unique<Threaded>(std::move(inner)); }

}  // namespace gpu
