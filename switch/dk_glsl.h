#pragma once
// deko3d: the SPIR-V the Vulkan renderer makes (vk_shader.cpp, from the SDK's HLSL) as GLSL for uam, which reads GLSL
// only. The same text on the PC tool (tools/dk_shaders) and on the console, so a DKSH cache made on one serves both.
#include <cstdint>
#include <string>
#include <vector>

namespace dkglsl {

enum class Stage { Vertex, Pixel, Unknown };

struct Glsl {
    Stage stage = Stage::Unknown;
    std::string text;   // GLSL 4.60 with every binding explicit, as uam requires
    std::string error;  // why there is no text
};

// Vulkan's set 0 (vk_shader.h): b0-b1 the two constant buffers, t0-t3 at 2-5 and s0-s3 at 6-9. deko3d's slots: the
// constant buffers stay at 0 and 1, and each texture with its sampler is one combined sampler at the texture's
// register (t0 -> 0)
Glsl FromSpirv(const std::vector<uint32_t>& spirv);

}  // namespace dkglsl
