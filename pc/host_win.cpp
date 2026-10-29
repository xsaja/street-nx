// The host functions switch/vulkan.cpp asks for, answered by MVP's Windows host (sdk/runtime/app): its game window
// and its ImGui Win32 backend. Only for the PC check of the Vulkan backend (Phase 2), never in the Switch build.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "host.h"

namespace host {

void* Window() { return app::Window(); }
// a minimized window has no size to draw at: the renderer waits as for the Switch's HOME menu
bool Foreground() { return !IsIconic(static_cast<HWND>(app::Window())); }
// a resize is a new swapchain size, the surface stays
uint32_t SurfaceEpoch() { return 0; }

void WindowPixels(int& w, int& h) {
    RECT r{};
    GetClientRect(static_cast<HWND>(app::Window()), &r);
    w = std::max<int>(r.right - r.left, 1);
    h = std::max<int>(r.bottom - r.top, 1);
}

}  // namespace host

namespace ui {

// main.cpp set up ImGui's Win32 backend on the game window: it reads the size, time, keyboard and mouse
void NewFrame(float width, float height) {
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(width, height);
}

}  // namespace ui
