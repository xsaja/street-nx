// Hooks for default.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }
//
// (2026-10-26: a verbose-only hook on sub_002DAA60, the game's pool free-size walk, found the Switch's oversized
// NtReadFile; see CLAUDE.md and the git history for it.)
#include <chrono>
#include <thread>

#include "core/log.h"
#include "kernel/kernel.h"

namespace {

// EA's sound engine guards itself with a counter: enter by incrementing it and going on only when it is 1, else
// decrement and give up; leave by decrementing it (17 inline decs in 0x220000-0x223443, [[hook]] SoundGuardLeave).
constexpr uint32_t kSoundGuard = 0x2FC184;
constexpr uint32_t kSoundReady = 0x2FC1A8;  // byte, set by the engine's init (0x2206CA)

}  // namespace

// FIFA Street 0x22004B, the guard's enter. The Xbox had one core: nobody else could read the counter between the inc
// and the compare. On the Switch's cores two threads both read 0 and both went in, and the title thread set a sound
// channel's volume while the time critical audio thread was still making the channel's voice: 0x220C48 called
// through a null object (seen in Ryujinx, at 9 s, whenever a thread was held back). Here entering is one compare and
// swap from 0 to 1. A caller that finds it taken waits up to 2 ms for it: on the Xbox the audio thread, of higher
// priority, had always finished by then. Past that it gives up, as the original did.
X86_FUNC(sub_0022004B) {
    auto* count = static_cast<uint32_t*>(kernel::HostPtr(kSoundGuard));
    uint32_t ok = 0;
    if (*static_cast<volatile uint8_t*>(kernel::HostPtr(kSoundReady))) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        for (;;) {
            uint32_t free = 0;
            if (__atomic_compare_exchange_n(count, &free, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                ok = 1;
                break;
            }
            if (std::chrono::steady_clock::now() >= until) {
                LOG_FIRST(8, "sound guard: still taken after 2 ms, the caller skips its work (as on the Xbox)");
                break;
            }
            std::this_thread::yield();
        }
    }
    kernel::Return(ctx, ok, 0);
}

// before each dec that leaves the guard: what the section wrote is seen by the next thread to enter
void SoundGuardLeave(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    (void)base;
    __atomic_thread_fence(__ATOMIC_RELEASE);
}
