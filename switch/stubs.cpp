// What newlib and libnx lack for the SDK's portable runtime. From BLACK-NX-PORT's switch/stubs.cpp, without
// Black-Android's touch, gyro and keyboard answers.
#include <fcntl.h>
#include <unistd.h>

#include <mutex>

// newlib has no pread: the SDK's disc image reader (kernel/disc.cpp) reads at offsets from several threads, so the
// seek and the read go together under one lock
extern "C" ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    static std::mutex lock;
    std::lock_guard<std::mutex> hold(lock);
    if (lseek(fd, offset, SEEK_SET) < 0) return -1;
    return read(fd, buf, count);
}

// libnx gives newlib no thread_detach, so pthread_detach (std::thread::detach) fails with ENOSYS. The runtime's
// detached threads (the kernel clock, rendering, audio, the guest's threads) mostly live as long as the title; one
// that ends keeps its stack and handle until the process exits.
extern "C" int __syscall_thread_detach(struct __pthread_t*) { return 0; }
