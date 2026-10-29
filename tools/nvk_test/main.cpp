// nvk_test.nro: the reference for dk_test's GPU numbers. The same measure on NVK (the Vulkan driver the game uses
// today): every pixel shader of sdmc:/switch/street-nx/shadercache on full-screen passes at the screen's size, 8 each,
// with the same dummy texture (64x64) and constants (0.5), timed with GPU timestamps and checked against the wall
// clock. Also the time NVK takes to create each pipeline. Writes sdmc:/switch/street-nx/logs/nvk_test.log; the screen shows
// the same lines. + exits.
#include <switch.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

namespace fs = std::filesystem;

namespace {

const char* const kDir = "sdmc:/switch/street-nx/";
FILE* g_log = nullptr;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (g_log) {
        std::fputs(line, g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
    std::printf("%s\n", line);
    consoleUpdate(nullptr);
}

[[noreturn]] void Fail(const char* what, VkResult r) {
    Log("[nvk] FAILED: %s (VkResult %d)", what, int(r));
    if (g_log) std::fclose(g_log);
    while (appletMainLoop()) {
        consoleUpdate(nullptr);
        svcSleepThread(100'000'000);
    }
    consoleExit(nullptr);
    std::exit(1);
}

#define VK(call)                                    \
    do {                                            \
        VkResult r_ = (call);                       \
        if (r_ != VK_SUCCESS) Fail(#call, r_);      \
    } while (0)

uint64_t NowNs() {
    uint64_t ticks;
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks * 625 / 12;
}

std::vector<uint32_t> ReadSpirv(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() <= 0) return {};
    std::vector<uint32_t> w(size_t(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(w.data()), std::streamsize(w.size() * 4));
    return w;
}

bool IsPixelShader(const std::vector<uint32_t>& w) {
    // OpEntryPoint (opcode 15): its first operand is the execution model, 4 = Fragment
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xFFFF, count = w[i] >> 16;
        if (!count) break;
        if (op == 15) return w[i + 1] == 4;
        i += count;
    }
    return false;
}

// the same full-screen vertex shader as dk_test: ten interpolators for the SDK's VSOut
const char kScreenVs[] = R"(#version 460
layout(location = 0) out vec4 o[10];
void main() {
    vec2 uv = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    for (int i = 0; i < 10; ++i) o[i] = vec4(uv, 0.25 + 0.05 * float(i), 1.0);
}
)";

std::vector<uint32_t> CompileVs() {
    glslang::InitializeProcess();
    glslang::TShader sh(EShLangVertex);
    const char* src = kScreenVs;
    sh.setStrings(&src, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, EShLangVertex, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    if (!sh.parse(GetDefaultResources(), 460, false, EShMsgDefault)) {
        Log("[nvk] the vertex shader does not compile: %s", sh.getInfoLog());
        return {};
    }
    glslang::TProgram prog;
    prog.addShader(&sh);
    if (!prog.link(EShMsgDefault)) return {};
    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv(*prog.getIntermediate(EShLangVertex), spirv);
    return spirv;
}

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    float timestamp_period = 1;

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) return i;
        Fail("no memory type", VK_ERROR_UNKNOWN);
    }
    VkDeviceMemory Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want) const {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, want);
        VkDeviceMemory m;
        VK(vkAllocateMemory(device, &ai, nullptr, &m));
        return m;
    }
};

void CreateGpu(Gpu& g) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mvp-nx nvk_test";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK(vkCreateInstance(&ici, nullptr, &g.instance));
    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(g.instance, &n, &g.physical);
    if (r < 0 || !n) Fail("vkEnumeratePhysicalDevices", r);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.physical, &props);
    g.timestamp_period = props.limits.timestampPeriod;
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &driver;
    vkGetPhysicalDeviceProperties2(g.physical, &p2);
    Log("[nvk] GPU: %s; driver %s, %s; timestampPeriod %.3f ns", props.deviceName, driver.driverName,
        driver.driverInfo, g.timestamp_period);
    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fp(families);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &families, fp.data());
    for (uint32_t i = 0; i < families; i++)
        if (fp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g.family = i;
            break;
        }
    const float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VK(vkCreateDevice(g.physical, &dci, nullptr, &g.device));
    vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
}

VkShaderModule Module(const Gpu& g, const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m;
    VK(vkCreateShaderModule(g.device, &ci, nullptr, &m));
    return m;
}

}  // namespace

int main(int, char**) {
    consoleInit(nullptr);
    std::error_code ec;
    fs::create_directories(fs::path(kDir) / "logs", ec);
    g_log = std::fopen((std::string(kDir) + "logs/nvk_test.log").c_str(), "w");
    Log("[nvk] mvp-nx NVK reference for the deko3d GPU numbers (dk_test.log)");

    uint32_t width = 1280, height = 720;
    if (appletGetOperationMode() == AppletOperationMode_Console) width = 1920, height = 1080;

    Gpu g;
    CreateGpu(g);

    // the game's pixel shaders
    struct Shader {
        std::string name;
        VkShaderModule module = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        double create_ms = 0, pass_ms = 0;
    };
    std::vector<Shader> shaders;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(fs::path(kDir) / "shadercache", ec))
        if (e.path().extension() == ".spv") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& p : files) {
        std::vector<uint32_t> w = ReadSpirv(p);
        if (w.size() < 5 || !IsPixelShader(w)) continue;
        shaders.push_back({p.stem().string(), Module(g, w)});
    }
    Log("[nvk] %zu pixel shaders of %zu SPIR-V modules in the cache", shaders.size(), files.size());
    const std::vector<uint32_t> vs_spirv = CompileVs();
    if (vs_spirv.empty()) Fail("the vertex shader", VK_ERROR_UNKNOWN);
    VkShaderModule vs = Module(g, vs_spirv);

    // the target, the texture (64x64 gradient), the constants and the descriptors: the SDK's set 0 (vk_shader.h)
    auto make_image = [&](VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImage& image,
                          VkImageView& view) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {w, h, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        VK(vkCreateImage(g.device, &ii, nullptr, &image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g.device, image, &req);
        VK(vkBindImageMemory(g.device, image, g.Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK(vkCreateImageView(g.device, &vi, nullptr, &view));
    };
    VkImage target, texture;
    VkImageView target_view, texture_view;
    make_image(VK_FORMAT_R8G8B8A8_UNORM, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, target, target_view);
    make_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, texture,
               texture_view);

    auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        VK(vkCreateBuffer(g.device, &bi, nullptr, &buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g.device, buffer, &req);
        VkDeviceMemory m = g.Allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK(vkBindBufferMemory(g.device, buffer, m, 0));
        void* p;
        VK(vkMapMemory(g.device, m, 0, size, 0, &p));
        return static_cast<uint8_t*>(p);
    };
    VkBuffer ubo, staging;
    float* constants = reinterpret_cast<float*>(make_buffer(4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ubo));
    std::fill(constants, constants + 1024, 0.5f);
    uint32_t* texels = reinterpret_cast<uint32_t*>(make_buffer(64 * 64 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging));
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x)
            texels[y * 64 + x] = (x < 8 && y < 8) ? 0xFFFFFFFFu : 0xFF000000u | (y * 4) << 8 | (x * 4);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler;
    VK(vkCreateSampler(g.device, &si, nullptr, &sampler));

    VkDescriptorSetLayoutBinding b[10] = {};
    for (uint32_t i = 0; i < 10; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : i < 6 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                      : VK_DESCRIPTOR_TYPE_SAMPLER;
        b[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 10;
    dl.pBindings = b;
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(g.device, &dl, nullptr, &set_layout));
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 4}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    VkDescriptorPool pool;
    VK(vkCreateDescriptorPool(g.device, &dp, nullptr, &pool));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &set_layout;
    VkDescriptorSet set;
    VK(vkAllocateDescriptorSets(g.device, &da, &set));
    VkDescriptorBufferInfo bufinfo{ubo, 0, 4096};
    VkDescriptorImageInfo imginfo{VK_NULL_HANDLE, texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo smpinfo{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w[10] = {};
    for (uint32_t i = 0; i < 10; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = b[i].descriptorType;
        if (i < 2) w[i].pBufferInfo = &bufinfo;
        else w[i].pImageInfo = i < 6 ? &imginfo : &smpinfo;
    }
    vkUpdateDescriptorSets(g.device, 10, w, 0, nullptr);

    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout;
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(g.device, &pl, nullptr, &layout));

    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    VkRenderPass pass;
    VK(vkCreateRenderPass(g.device, &rp, nullptr, &pass));
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = pass;
    fb.attachmentCount = 1;
    fb.pAttachments = &target_view;
    fb.width = width;
    fb.height = height;
    fb.layers = 1;
    VkFramebuffer framebuffer;
    VK(vkCreateFramebuffer(g.device, &fb, nullptr, &framebuffer));

    // one pipeline per pixel shader, timed (NVK compiles the shader here)
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkViewport vp{0, 0, float(width), float(height), 0, 1};
    VkRect2D sc{{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo vs_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vs_state.viewportCount = vs_state.scissorCount = 1;
    vs_state.pViewports = &vp;
    vs_state.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    for (Shader& s : shaders) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = s.module;
        st[1].pName = "main";
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.stageCount = 2;
        pi.pStages = st;
        pi.pVertexInputState = &vin;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vs_state;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pColorBlendState = &cbs;
        pi.layout = layout;
        pi.renderPass = pass;
        const uint64_t t = NowNs();
        VK(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pi, nullptr, &s.pipeline));
        s.create_ms = (NowNs() - t) / 1e6;
    }
    double create_sum = 0;
    for (const Shader& s : shaders) create_sum += s.create_ms;
    Log("[nvk] %zu pipelines created in %.0f ms (NVK compiles each shader here)", shaders.size(), create_sum);

    // the commands: the texture upload, then 8 full-screen passes per shader between two timestamps
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = g.family;
    VkCommandPool cpool;
    VK(vkCreateCommandPool(g.device, &cpi, nullptr, &cpool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cpool;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK(vkAllocateCommandBuffers(g.device, &cai, &cmd));
    VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qp.queryCount = uint32_t(shaders.size() * 2);
    VkQueryPool queries;
    VK(vkCreateQueryPool(g.device, &qp, nullptr, &queries));

    constexpr uint32_t kPasses = 8;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(cmd, &bi));
    vkCmdResetQueryPool(cmd, queries, 0, qp.queryCount);
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = texture;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &ib);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {64, 64, 1};
    vkCmdCopyBufferToImage(cmd, staging, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &ib);
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = pass;
    rbi.framebuffer = framebuffer;
    rbi.renderArea = sc;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    for (size_t i = 0; i < shaders.size(); i++) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shaders[i].pipeline);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, uint32_t(i * 2));
        for (uint32_t p = 0; p < kPasses; p++) vkCmdDraw(cmd, 4, 1, 0, 0);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, uint32_t(i * 2 + 1));
    }
    vkCmdEndRenderPass(cmd);
    VK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK(vkCreateFence(g.device, &fi, nullptr, &fence));
    VkSubmitInfo sub_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub_info.commandBufferCount = 1;
    sub_info.pCommandBuffers = &cmd;
    const uint64_t t0 = NowNs();
    VK(vkQueueSubmit(g.queue, 1, &sub_info, fence));
    VK(vkWaitForFences(g.device, 1, &fence, VK_TRUE, 10'000'000'000ull));
    const double wall_ms = (NowNs() - t0) / 1e6;
    std::vector<uint64_t> stamps(qp.queryCount);
    VK(vkGetQueryPoolResults(g.device, queries, 0, qp.queryCount, stamps.size() * 8, stamps.data(), 8,
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    // the same scale as dk_test (x1.627 a unit), and NVK's own timestampPeriod for comparison
    double gpu_ms = 0, gpu_ms_period = 0;
    for (size_t i = 0; i < shaders.size(); i++) {
        const double units = double(stamps[i * 2 + 1] - stamps[i * 2]);
        shaders[i].pass_ms = units * 1.627 / kPasses / 1e6;
        gpu_ms += units * 1.627 / 1e6;
        gpu_ms_period += units * g.timestamp_period / 1e6;
    }
    std::vector<Shader> sorted = shaders;
    std::sort(sorted.begin(), sorted.end(), [](const Shader& a, const Shader& b) { return a.pass_ms > b.pass_ms; });
    Log("[nvk] GPU cost of the game's %zu pixel shaders: full-screen passes of %ux%u, %u each", sorted.size(), width,
        height, kPasses);
    for (const Shader& s : sorted)
        Log("[nvk]   %s  %.3f ms a pass (pipeline created in %.1f ms)", s.name.c_str(), s.pass_ms, s.create_ms);
    Log("[nvk] GPU (x1.627) %.1f ms, (x timestampPeriod %.3f) %.1f ms, against %.1f ms of wall clock for all of it", gpu_ms,
        g.timestamp_period, gpu_ms_period, wall_ms);
    Log("[nvk] done. Press + to exit.");
    if (g_log) std::fclose(g_log);
    g_log = nullptr;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
        svcSleepThread(16'000'000);
    }
    vkDeviceWaitIdle(g.device);
    vkDestroyDevice(g.device, nullptr);
    vkDestroyInstance(g.instance, nullptr);
    consoleExit(nullptr);
    return 0;
}
