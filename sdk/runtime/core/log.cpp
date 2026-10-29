#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace logx {

static std::mutex g_mutex;  // the file and the writes to it, in order
static FILE* g_file = nullptr;
static std::string g_path;
static auto g_start = std::chrono::steady_clock::now();

// STREET: a line is only queued; a writer thread puts the queue in the files every 100 ms. Written at once, every
// line waited for the SD card (twice: the log and the unbuffered stderr), under the lock every thread takes: the render
// thread's 10 s perf lines made a 35 ms frame, and a slow card write stalled whoever logged next for 100-300 ms.
static std::mutex g_queue_mutex;
static std::condition_variable g_queue_cv;
static std::string g_queue;
static bool g_writer = false;
constexpr size_t kQueueMax = 4 << 20;  // past this, lines are dropped and counted
static size_t g_dropped = 0;

// the queue into the files; g_mutex held
static void WriteQueued(std::string& batch) {
    if (batch.empty()) return;
    if (g_file) {
        std::fwrite(batch.data(), 1, batch.size(), g_file);
        std::fflush(g_file);
    }
    std::fwrite(batch.data(), 1, batch.size(), stderr);
    batch.clear();
}

static void Drain() {
    std::lock_guard<std::mutex> io(g_mutex);
    std::string batch;
    {
        std::lock_guard<std::mutex> q(g_queue_mutex);
        batch.swap(g_queue);
        if (g_dropped) {
            char note[80];
            std::snprintf(note, sizeof(note), "[log: %zu lines dropped, the queue was full]\n", g_dropped);
            batch += note;
            g_dropped = 0;
        }
    }
    WriteQueued(batch);
}

static void StartWriter() {
    if (g_writer) return;
    g_writer = true;
    std::thread([] {
        for (;;) {
            {
                std::unique_lock<std::mutex> q(g_queue_mutex);
                g_queue_cv.wait_for(q, std::chrono::milliseconds(100));
            }
            Drain();
        }
    }).detach();
}

constexpr int kKeep = 30;

// the run number of prefix_NNN.log, 0 for another name
static int RunNumber(const fs::path& file, const std::string& prefix) {
    std::string name = file.filename().string();
    if (file.extension() != ".log" || name.size() <= prefix.size() + 1 || name.compare(0, prefix.size() + 1, prefix + "_"))
        return 0;
    return std::atoi(name.c_str() + prefix.size() + 1);
}

void Init(const std::string& dir, const std::string& prefix, bool append) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!kVerbose) {
        g_path = (fs::path(dir) / (prefix + ".log")).string();
        g_file = std::fopen(g_path.c_str(), append ? "a" : "w");
        StartWriter();
        return;
    }
    std::vector<std::pair<int, fs::path>> runs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (int n = RunNumber(e.path(), prefix)) runs.emplace_back(n, e.path());
    std::sort(runs.begin(), runs.end());
    for (size_t i = 0; i + kKeep <= runs.size(); i++) fs::remove(runs[i].second, ec);
    char name[256];
    std::snprintf(name, sizeof(name), "%s_%03d.log", prefix.c_str(), runs.empty() ? 1 : runs.back().first + 1);
    g_path = (fs::path(dir) / name).string();
    g_file = std::fopen(g_path.c_str(), "w");
    StartWriter();
}

const std::string& Path() { return g_path; }

void Write(Level lv, const char* fmt, ...) {
    if (!kVerbose && lv < Level::Error) return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    static const char* names[] = {"debug", "info", "warn", "error", "info"};
    auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF;
    char line[4300];
    int n = kVerbose ? std::snprintf(line, sizeof(line), "[%9.3f] [%s] [t%04zx] %s\n", t, names[int(lv)], size_t(tid), msg)
                     : std::snprintf(line, sizeof(line), "[%9.3f] %s%s\n", t, lv == Level::Error ? "error: " : "", msg);
    n = std::min(n, int(sizeof(line)) - 1);
    if (!g_writer) {  // before Init: straight to the files
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file) std::fwrite(line, 1, size_t(n), g_file);
        std::fwrite(line, 1, size_t(n), stderr);
        return;
    }
    {
        std::lock_guard<std::mutex> q(g_queue_mutex);
        if (g_queue.size() + size_t(n) > kQueueMax) {
            g_dropped++;
            return;
        }
        g_queue.append(line, size_t(n));
    }
    if (lv >= Level::Warn) g_queue_cv.notify_one();  // warnings and errors reach the card without waiting the 100 ms
}

// everything queued into the files now: before exiting, from crash handlers
void Flush() {
    Drain();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fflush(g_file);
}

}  // namespace logx
