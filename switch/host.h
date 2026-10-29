#pragma once
// What the Switch host's parts share: the screen, the applet's focus and player 1's controller for ImGui. Adapted
// from BLACK-NX-PORT's switch/host.h, without Black-Android's touch, gyro and options menu.
#include <cstdint>

namespace host {

// the default NWindow of the applet (vulkan.cpp makes its surface on it)
void* Window();
// the applet has the focus; the renderer waits while it does not
bool Foreground();
// bumped when the screen changes size (docked or handheld): the renderer makes its swapchain again
uint32_t SurfaceEpoch();
// the screen's size in pixels: 1280x720 handheld, 1920x1080 docked
void WindowPixels(int& w, int& h);
// the face buttons by letter (A is the Xbox A) instead of by position (the bottom button is the Xbox A)
bool ButtonsByName();
// the renderer setting: deko3d (dk_*.cpp, deko.cpp) instead of Vulkan on NVK (vulkan.cpp)
bool UseDeko3d();
// deko3d: vertices and constants written to CPU cached memory, cleaned before each submit
bool DekoCachedUpload();
// deko3d: frames between whole checks of an unlocked texture for CPU writes (1 = every frame)
uint32_t TextureCheckFrames();
// deko3d: SMAA 1x over the game's frame before it is scaled to the screen
bool Smaa();
// cpu_mhz, gpu_mhz, mem_mhz from the settings (clocks.cpp): name is "cpu", "gpu" or "mem", 0 the system's clock
void SetClockWanted(const char* name, uint32_t mhz);
uint32_t ClockWanted(const char* name);
void ApplyClocks();    // at start and after each docked/handheld change
void RestoreClocks();  // the system's clocks back
void WatchClocks();    // from the main loop: the asked clocks set again if something changed them
// profile = true in the settings: samples the busy threads into logs/prof.bin (profiler.cpp)
void StartProfiler();
// the console's overlays through SaltyNX (overlays.cpp): FPS and resolution for Status Monitor / Horizon OC Monitor,
// and the handheld/docked mode picked in ReverseNX-RT. All do nothing without SaltyNX
void OverlaysEnable(bool on);                              // the saltynx setting
void OverlaysInit();                                       // main thread, before the game starts
void OverlaysFrame(uint32_t width, uint32_t height);       // each present
void OverlaysSecond(double fps, double fps_avg);           // once a second
bool OverlaysDocked(bool real);                            // the mode to obey

}  // namespace host

// controllers through libnx's HID (input.cpp)
namespace input {
// the first controller's buttons as libnx reports them (HidNpadButton_*), for ImGui's navigation (ui.cpp)
uint64_t NavButtons();
void NavStick(float& x, float& y);
}  // namespace input

// ImGui's frame on the Switch (ui.cpp): render thread, before ImGui::NewFrame
namespace ui {
void NewFrame(float width, float height);
}  // namespace ui
