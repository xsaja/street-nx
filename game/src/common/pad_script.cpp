// Scripted presses on player 1, added to the real controller, for unattended tests of the same screens on every
// backend (D3D12, Vulkan on the PC, the Switch). Commands, one a line, # comments, run in order:
//   BUTTONS [seconds]  held that long (default 0.15), then 0.15 s released; buttons joined by +:
//                      UP DOWN LEFT RIGHT START BACK A B X Y BLACK WHITE LT RT LS LSD LSL LSR (left stick up, down,
//                      left, right) RS RSD RSL RSR (right stick)
//   WAIT seconds       nothing pressed
//   SHOT               the F12 capture of the next frame (screenshots/ beside the exe)
// From the file STREET_PAD_SCRIPT names at start, and from a file named cmd beside the exe, read and deleted every
// 100 ms while the game runs.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"

namespace street {

namespace {

using Clock = std::chrono::steady_clock;

struct Step {
    std::string what;
    double seconds;
};

std::mutex g_lock;
std::deque<Step> g_queue;
std::string g_held;               // the buttons of the step running
Clock::time_point g_until{};      // the end of the step running
Clock::time_point g_next_read{};  // the next look for the cmd file
bool g_plugged = false;           // a script came: port 0 stays connected from then on, as a real pad would

void Parse(FILE* f, const char* from) {
    char line[160];
    while (std::fgets(line, sizeof line, f)) {
        char what[96] = {};
        double s = -1;
        if (line[0] == '#' || std::sscanf(line, "%95s %lf", what, &s) < 1) continue;
        std::string w = what;
        if (w == "WAIT" && s < 0) s = 1;
        g_queue.push_back({w, s});
        g_plugged = true;
    }
    LOGR("pad script: %s, %zu steps queued", from, g_queue.size());
}

void Apply(const std::string& s, input::Pad& p) {
    size_t at = 0;
    while (at < s.size()) {
        size_t e = s.find('+', at);
        if (e == std::string::npos) e = s.size();
        std::string b = s.substr(at, e - at);
        at = e + 1;
        if (b == "UP") p.buttons |= 1;
        else if (b == "DOWN") p.buttons |= 2;
        else if (b == "LEFT") p.buttons |= 4;
        else if (b == "RIGHT") p.buttons |= 8;
        else if (b == "START") p.buttons |= 16;
        else if (b == "BACK") p.buttons |= 32;
        else if (b == "A") p.analog[0] = 255;
        else if (b == "B") p.analog[1] = 255;
        else if (b == "X") p.analog[2] = 255;
        else if (b == "Y") p.analog[3] = 255;
        else if (b == "BLACK") p.analog[4] = 255;
        else if (b == "WHITE") p.analog[5] = 255;
        else if (b == "LT") p.analog[6] = 255;
        else if (b == "RT") p.analog[7] = 255;
        else if (b == "LS") p.ly = 32767;
        else if (b == "LSD") p.ly = -32767;
        else if (b == "LSL") p.lx = -32767;
        else if (b == "LSR") p.lx = 32767;
        else if (b == "RS") p.ry = 32767;
        else if (b == "RSD") p.ry = -32767;
        else if (b == "RSL") p.rx = -32767;
        else if (b == "RSR") p.rx = 32767;
    }
}

}  // namespace

void ScriptPad(int port, input::Pad& pad) {
    if (port != 0) return;
    static bool loaded = false;
    std::lock_guard lock(g_lock);
    Clock::time_point now = Clock::now();
    if (!loaded) {
        loaded = true;
        if (const char* p = std::getenv("STREET_PAD_SCRIPT"))
            if (FILE* f = std::fopen(p, "r")) {
                Parse(f, p);
                std::fclose(f);
            }
    }
    if (now >= g_next_read) {
        g_next_read = now + std::chrono::milliseconds(100);
        std::string cmd = (app::ExeDir() / "cmd").string();
        if (FILE* f = std::fopen(cmd.c_str(), "r")) {
            Parse(f, "cmd");
            std::fclose(f);
            std::remove(cmd.c_str());
        }
    }
    while (now >= g_until && !g_queue.empty()) {
        Step s = g_queue.front();
        g_queue.pop_front();
        LOGR("pad script: %s %.2f", s.what.c_str(), s.seconds);
        if (s.what == "SHOT") {
            gpu::g_screenshot.store(true);
            g_held.clear();
        } else if (s.what == "WAIT") {
            g_held.clear();
            g_until = now + std::chrono::milliseconds(int64_t(s.seconds * 1000));
        } else {
            // held, then released as long before the next step
            double held = s.seconds > 0 ? s.seconds : 0.15;
            g_held = s.what;
            g_until = now + std::chrono::milliseconds(int64_t(held * 1000));
            g_queue.push_front({"WAIT", 0.15});
        }
    }
    // a pad that only exists while a button is held reads as plugged in and out on every press (FIFA Street then
    // reopens it and drops the press)
    if (g_plugged) pad.connected = true;
    if (now < g_until && !g_held.empty()) Apply(g_held, pad);
}

}  // namespace street
