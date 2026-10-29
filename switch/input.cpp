// controllers through libnx's HID: the handheld Joy-Cons or a pair/Pro Controller as player 1, players 2-4 on their
// own pads. The face buttons by position by default, as an Xbox pad lies under the hand (MVP throws to the bases by
// the diamond they make): bottom B is the Xbox A, right A is B, left Y is X, top X is Y. buttons_by_name = true in the
// settings maps them by letter instead. L and R are the triggers, which the game shows as L and R; ZL and ZR the
// white and black buttons; + is Start and - is Back.
#include "core/input.h"

#include <switch.h>

#include <algorithm>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "host.h"

namespace input {

namespace {

std::mutex g_lock;
PadState g_pads[4];
HidVibrationDeviceHandle g_vib[4][2];
bool g_vib_ok[4];
bool g_stopped = false;  // HID is about to close: no more calls into it

void Read(PadState& s, Pad& p) {
    if (g_stopped) return;
    padUpdate(&s);
    if (!padIsConnected(&s)) return;
    p.connected = true;
    u64 b = padGetButtons(&s);
    // dpad up/down/left/right, start, back, left thumb, right thumb
    constexpr u64 kButtons[8] = {HidNpadButton_Up,   HidNpadButton_Down,  HidNpadButton_Left,     HidNpadButton_Right,
                                 HidNpadButton_Plus, HidNpadButton_Minus, HidNpadButton_StickL, HidNpadButton_StickR};
    for (int i = 0; i < 8; i++)
        if (b & kButtons[i]) p.buttons |= uint16_t(1u << i);
    // A B X Y black white left trigger right trigger
    constexpr u64 kByPosition[8] = {HidNpadButton_B,  HidNpadButton_A,  HidNpadButton_Y, HidNpadButton_X,
                                    HidNpadButton_ZR, HidNpadButton_ZL, HidNpadButton_L, HidNpadButton_R};
    constexpr u64 kByName[8] = {HidNpadButton_A,  HidNpadButton_B,  HidNpadButton_X, HidNpadButton_Y,
                                HidNpadButton_ZR, HidNpadButton_ZL, HidNpadButton_L, HidNpadButton_R};
    const u64* map = host::ButtonsByName() ? kByName : kByPosition;
    for (int i = 0; i < 8; i++) p.analog[i] = (b & map[i]) ? 0xFF : 0;
    // libnx's sticks are +-32767 with up positive, as the console's
    HidAnalogStickState l = padGetStickPos(&s, 0), r = padGetStickPos(&s, 1);
    p.lx = int16_t(std::clamp(l.x, -32767, 32767));
    p.ly = int16_t(std::clamp(l.y, -32767, 32767));
    p.rx = int16_t(std::clamp(r.x, -32767, 32767));
    p.ry = int16_t(std::clamp(r.y, -32767, 32767));
}

}  // namespace

void Init() {
    padConfigureInput(4, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pads[0]);  // player 1 and the handheld Joy-Cons
    padInitialize(&g_pads[1], HidNpadIdType_No2);
    padInitialize(&g_pads[2], HidNpadIdType_No3);
    padInitialize(&g_pads[3], HidNpadIdType_No4);
    const HidNpadIdType ids[4] = {HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4};
    for (int i = 0; i < 4; i++)
        g_vib_ok[i] = R_SUCCEEDED(hidInitializeVibrationDevices(g_vib[i], 2, ids[i], HidNpadStyleTag_NpadFullKey));
    LOGI("input: libnx HID, 4 ports");
}

Pad Poll(int port) {
    Pad p;
    if (port < 0 || port > 3) return p;
    {
        std::lock_guard lock(g_lock);
        Read(g_pads[port], p);
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    if (port < 0 || port > 3 || !g_vib_ok[port]) return;
    std::lock_guard lock(g_lock);
    if (g_stopped) return;
    HidVibrationValue v[2] = {};
    float amp[2] = {left / 65535.0f, right / 65535.0f};
    for (int i = 0; i < 2; i++) {
        v[i].amp_low = amp[i];
        v[i].freq_low = 160.0f;
        v[i].amp_high = amp[i];
        v[i].freq_high = 320.0f;
    }
    hidSendVibrationValues(g_vib[port], v, 2);
}

// the lock waits out a poll in progress, so nothing is inside HID when the caller goes on to close it
void Stop() {
    std::lock_guard lock(g_lock);
    g_stopped = true;
}

uint64_t NavButtons() {
    std::lock_guard lock(g_lock);
    return !g_stopped && padIsConnected(&g_pads[0]) ? padGetButtons(&g_pads[0]) : 0;
}

void NavStick(float& x, float& y) {
    std::lock_guard lock(g_lock);
    HidAnalogStickState l = padGetStickPos(&g_pads[0], 0);
    x = l.x / 32767.0f;
    y = l.y / 32767.0f;
}

}  // namespace input
