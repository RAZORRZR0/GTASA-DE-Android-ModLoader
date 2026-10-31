// Post effects on UE's Vulkan frame, before the HUD: a port of PC SkyGfxDE postfx.cpp (D3D11) pass for pass.
// Order as PC (the original frame): water drops, then CPostEffects::Render's SpeedFX, radiosity, rain grain. Every
// effect that reads the frame copies it first. vkhook.cpp calls Draw() inside UE's command buffer when the scene
// composite ends; inputs come from the game thread's FxState (fx.h). Shaders: shaders/*.vert|frag (shaders_spv.h).
// Vulkan specifics: every barrier uses ALL_COMMANDS / MEMORY access (a handful per frame, correctness over the last
// microsecond); host-written data (drop vertices, grain texels) sits in kRing per-frame slots so a frame still on the
// GPU is never overwritten; resources are destroyed kRetireFrames frames after they stop being used.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <functional>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "fx.h"
#include "shaders_spv.h"
#include "skygfx.h"
#include "vkhook.h"

namespace skygfx {
namespace {

constexpr int kRing = 6;          // per-frame slots of host-written data (UE keeps up to 3 frames in flight)
constexpr int kRetireFrames = 8;  // frames before a dropped resource is destroyed
enum { MAXDROPS = 2000, MAXDROPSMOVING = 700 };  // neo.h
struct DropVertex { float x, y; uint32_t rgba; float u0, v0, u1, v1; };

// PC kSpeedFx {speed, passes, shift, wobble} (gta_sa.exe 1.0 US 0x8D5190); the row comes from FxState.
struct SpeedFxRow { int passes, shift, wobble; };
constexpr SpeedFxRow kSpeedFx[7] = { {1, 4, 0}, {2, 4, 0}, {3, 4, 0}, {3, 4, 0}, {4, 4, 1}, {4, 4, 2}, {5, 4, 3} };
constexpr float kSpeedFxAlpha = 36.0f;  // CPostEffects::m_SpeedFXAlpha

// ---------------------------------------------------------------- device entry points (the real ones)
struct Api {
#define F(n) PFN_vk##n n;
    F(GetPhysicalDeviceMemoryProperties) F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements) F(AllocateMemory)
    F(FreeMemory) F(BindImageMemory) F(CreateImageView) F(DestroyImageView) F(CreateBuffer) F(DestroyBuffer)
    F(GetBufferMemoryRequirements) F(BindBufferMemory) F(MapMemory) F(CreateSampler) F(CreateRenderPass) F(CreateFramebuffer)
    F(DestroyFramebuffer) F(CreateShaderModule) F(DestroyShaderModule) F(CreateDescriptorSetLayout) F(CreatePipelineLayout)
    F(CreateGraphicsPipelines) F(CreateDescriptorPool) F(DestroyDescriptorPool) F(AllocateDescriptorSets)
    F(UpdateDescriptorSets) F(CmdPipelineBarrier) F(CmdCopyImage) F(CmdCopyBufferToImage) F(CmdBeginRenderPass)
    F(CmdEndRenderPass) F(CmdBindPipeline) F(CmdBindDescriptorSets) F(CmdBindVertexBuffers) F(CmdPushConstants)
    F(CmdSetViewport) F(CmdSetScissor) F(CmdDraw)
#undef F
} vk;
VkDevice D = VK_NULL_HANDLE;
VkPhysicalDeviceMemoryProperties g_mem;

bool LoadApi(const vkhook::Device& d) {
    D = d.device;
    bool ok = true;
    vk.GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)d.gipa(d.instance, "vkGetPhysicalDeviceMemoryProperties");
    ok = ok && vk.GetPhysicalDeviceMemoryProperties;
#define F(n) vk.n = (PFN_vk##n)d.gdpa(D, "vk" #n); if (!vk.n) { SKY_LOG("vkfx: missing vk" #n); ok = false; }
    F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements) F(AllocateMemory) F(FreeMemory) F(BindImageMemory)
    F(CreateImageView) F(DestroyImageView) F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements)
    F(BindBufferMemory) F(MapMemory) F(CreateSampler) F(CreateRenderPass) F(CreateFramebuffer) F(DestroyFramebuffer)
    F(CreateShaderModule) F(DestroyShaderModule) F(CreateDescriptorSetLayout) F(CreatePipelineLayout)
    F(CreateGraphicsPipelines) F(CreateDescriptorPool) F(DestroyDescriptorPool) F(AllocateDescriptorSets)
    F(UpdateDescriptorSets) F(CmdPipelineBarrier) F(CmdCopyImage) F(CmdCopyBufferToImage) F(CmdBeginRenderPass)
    F(CmdEndRenderPass) F(CmdBindPipeline) F(CmdBindDescriptorSets) F(CmdBindVertexBuffers) F(CmdPushConstants)
    F(CmdSetViewport) F(CmdSetScissor) F(CmdDraw)
#undef F
    if (ok) vk.GetPhysicalDeviceMemoryProperties(d.physical, &g_mem);
    return ok;
}

// ---------------------------------------------------------------- deferred destruction
unsigned g_frame = 0;  // Draw() calls
std::vector<std::pair<unsigned, std::function<void()>>> g_retired;
void Retire(std::function<void()> fn) { g_retired.push_back({ g_frame, std::move(fn) }); }
void CollectRetired() {
    for (size_t i = 0; i < g_retired.size();)
        if (g_frame - g_retired[i].first >= kRetireFrames) {
            g_retired[i].second();
            g_retired[i] = std::move(g_retired.back());
            g_retired.pop_back();
        } else ++i;
}

// ---------------------------------------------------------------- resources
int MemType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

struct Image { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
               VkFramebuffer fb = VK_NULL_HANDLE; uint32_t w = 0, h = 0; };

bool MakeImage(Image& im, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    im.w = w; im.h = h;
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f; ci.extent = { w, h, 1 }; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.CreateImage(D, &ci, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vk.GetImageMemoryRequirements(D, im.image, &req);
    const int type = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, (uint32_t)type };
    if (type < 0 || vk.AllocateMemory(D, &ai, nullptr, &im.mem) != VK_SUCCESS) return false;
    if (vk.BindImageMemory(D, im.image, im.mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return vk.CreateImageView(D, &vi, nullptr, &im.view) == VK_SUCCESS;
}

void FreeImage(Image& im) {
    Image c = im;
    im = Image{};
    Retire([c] {
        if (c.fb) vk.DestroyFramebuffer(D, c.fb, nullptr);
        if (c.view) vk.DestroyImageView(D, c.view, nullptr);
        if (c.image) vk.DestroyImage(D, c.image, nullptr);
        if (c.mem) vk.FreeMemory(D, c.mem, nullptr);
    });
}

struct HostBuffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint8_t* ptr = nullptr; VkDeviceSize slot = 0; };

bool MakeHostBuffer(HostBuffer& b, VkDeviceSize slot, VkBufferUsageFlags usage) {
    b.slot = (slot + 255) & ~(VkDeviceSize)255;
    VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = b.slot * kRing; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.CreateBuffer(D, &ci, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vk.GetBufferMemoryRequirements(D, b.buf, &req);
    const int type = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, (uint32_t)type };
    if (type < 0 || vk.AllocateMemory(D, &ai, nullptr, &b.mem) != VK_SUCCESS) return false;
    if (vk.BindBufferMemory(D, b.buf, b.mem, 0) != VK_SUCCESS) return false;
    return vk.MapMemory(D, b.mem, 0, VK_WHOLE_SIZE, 0, (void**)&b.ptr) == VK_SUCCESS;
}

// ---------------------------------------------------------------- pipeline objects (per swapchain format)
VkFormat F = VK_FORMAT_UNDEFINED;
VkSampler g_point, g_linear, g_wrap;
VkDescriptorSetLayout g_setLayout;
VkPipelineLayout g_layout;
VkRenderPass g_rpLoad;    // onto the frame: load, store, colour-attachment layout throughout
VkRenderPass g_rpTarget;  // into an intermediate: contents discarded, left for sampling
enum Pipe { kCopyAlpha, kCopyOpaque, kThresh, kCopyAdd, kGrain, kDrop, kPipes };
VkPipeline g_pipe[kPipes];
bool g_initFailed = false;

VkRenderPass MakeRenderPass(VkFormat f, bool load) {
    VkAttachmentDescription a{};
    a.format = f; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
    const VkSubpassDependency dep[2] = {
        { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
        { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
    };
    VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1; ci.pAttachments = &a; ci.subpassCount = 1; ci.pSubpasses = &sp;
    ci.dependencyCount = 2; ci.pDependencies = dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    return vk.CreateRenderPass(D, &ci, nullptr, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
}

VkShaderModule Module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, bytes, code };
    VkShaderModule m = VK_NULL_HANDLE;
    return vk.CreateShaderModule(D, &ci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// Blend: 0 none, 1 alpha (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), 2 add (SRC_ALPHA, ONE), 3 grain (DST_COLOR, SRC_ALPHA).
// Alpha channel: ZERO, ONE (PC D3D11 blend states).
VkPipeline MakePipeline(VkShaderModule vs, VkShaderModule fs, int blend, bool drops) {
    const VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr },
    };
    const VkVertexInputBindingDescription bind{ 0, sizeof(DropVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    const VkVertexInputAttributeDescription attr[3] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8 }, { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12 },
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    if (drops) { vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind; vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attr; }
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = drops ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    if (blend) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = blend == 3 ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : blend == 2 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dyn };
    VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &cb; ci.pDynamicState = &ds;
    ci.layout = g_layout; ci.renderPass = g_rpLoad; ci.subpass = 0;  // g_rpTarget is compatible (same format, 1 sample)
    VkPipeline p = VK_NULL_HANDLE;
    return vk.CreateGraphicsPipelines(D, VK_NULL_HANDLE, 1, &ci, nullptr, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

bool InitPipelines(VkFormat f) {
    F = f;
    VkSamplerCreateInfo sc{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST; sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; sc.maxLod = 0.0f;
    bool ok = vk.CreateSampler(D, &sc, nullptr, &g_point) == VK_SUCCESS;
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    ok = ok && vk.CreateSampler(D, &sc, nullptr, &g_linear) == VK_SUCCESS;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ok = ok && vk.CreateSampler(D, &sc, nullptr, &g_wrap) == VK_SUCCESS;
    const VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo lc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &b };
    ok = ok && vk.CreateDescriptorSetLayout(D, &lc, nullptr, &g_setLayout) == VK_SUCCESS;
    const VkPushConstantRange pr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48 };
    VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &g_setLayout, 1, &pr };
    ok = ok && vk.CreatePipelineLayout(D, &pl, nullptr, &g_layout) == VK_SUCCESS;
    g_rpLoad = ok ? MakeRenderPass(f, true) : VK_NULL_HANDLE;
    g_rpTarget = ok ? MakeRenderPass(f, false) : VK_NULL_HANDLE;
    ok = ok && g_rpLoad && g_rpTarget;
    VkShaderModule quad = Module(kSpv_quad_vert, sizeof kSpv_quad_vert), copy = Module(kSpv_copy_frag, sizeof kSpv_copy_frag),
                   thresh = Module(kSpv_thresh_frag, sizeof kSpv_thresh_frag), grain = Module(kSpv_grain_frag, sizeof kSpv_grain_frag),
                   dropV = Module(kSpv_drop_vert, sizeof kSpv_drop_vert), dropF = Module(kSpv_drop_frag, sizeof kSpv_drop_frag);
    ok = ok && quad && copy && thresh && grain && dropV && dropF;
    if (ok) {
        g_pipe[kCopyAlpha] = MakePipeline(quad, copy, 1, false);
        g_pipe[kCopyOpaque] = MakePipeline(quad, copy, 0, false);
        g_pipe[kThresh] = MakePipeline(quad, thresh, 0, false);
        g_pipe[kCopyAdd] = MakePipeline(quad, copy, 2, false);
        g_pipe[kGrain] = MakePipeline(quad, grain, 3, false);
        g_pipe[kDrop] = MakePipeline(dropV, dropF, 1, true);
        for (VkPipeline p : g_pipe) ok = ok && p;
    }
    for (VkShaderModule m : { quad, copy, thresh, grain, dropV, dropF })
        if (m) vk.DestroyShaderModule(D, m, nullptr);
    if (!ok) SKY_LOG("vkfx: pipeline creation failed (format %d): post effects off", f);
    return ok;
}

// ---------------------------------------------------------------- frame-sized resources (PC Sized)
uint32_t W = 0, H = 0;
Image g_copy;        // CPostEffects::pRasterFrontBuffer
Image g_rad[8];      // radiosity halvings [0, g_radN), threshold result [g_radN]
int g_radN = 0;
Image g_grainTex;    // 64x64 R8
HostBuffer g_dropVb, g_grainStage;
VkDescriptorPool g_pool = VK_NULL_HANDLE;
VkDescriptorSet g_setCopyPoint, g_setCopyLinear, g_setRad[8], g_setGrain;
struct SwapFb { VkImageView view; VkFramebuffer fb; };
std::unordered_map<VkImage, SwapFb> g_swapFb;

VkDescriptorSet MakeSet(VkImageView view, VkSampler s) {
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, g_pool, 1, &g_setLayout };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vk.AllocateDescriptorSets(D, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    const VkDescriptorImageInfo ii{ s, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vk.UpdateDescriptorSets(D, 1, &w, 0, nullptr);
    return set;
}

bool MakeFb(Image& im) {
    VkFramebufferCreateInfo ci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass = g_rpTarget; ci.attachmentCount = 1; ci.pAttachments = &im.view; ci.width = im.w; ci.height = im.h; ci.layers = 1;
    return vk.CreateFramebuffer(D, &ci, nullptr, &im.fb) == VK_SUCCESS;
}

void FreeSized() {
    FreeImage(g_copy);
    for (Image& r : g_rad) if (r.image) FreeImage(r);
    if (g_pool) { VkDescriptorPool p = g_pool; Retire([p] { vk.DestroyDescriptorPool(D, p, nullptr); }); g_pool = VK_NULL_HANDLE; }
    W = H = 0;
}

// Radiosity works at the PS2's scale: m_RadiosityFilterPasses (2) halvings of 640x448, plus the halvings that bring
// this frame's height down to about 448 (PC postfx.cpp Sized).
bool Sized(uint32_t w, uint32_t h) {
    if (w == W && h == H && g_copy.image) return true;
    FreeSized();
    W = w; H = h;
    bool ok = MakeImage(g_copy, F, W, H, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    g_radN = 2 + (H > 448 ? (int)lroundf(log2f(H / 448.0f)) : 0);
    if (g_radN > 7) g_radN = 7;
    for (int i = 0; i <= g_radN && ok; ++i) {
        const uint32_t rw = i < g_radN ? (W >> (i + 1) ? W >> (i + 1) : 1) : g_rad[g_radN - 1].w;
        const uint32_t rh = i < g_radN ? (H >> (i + 1) ? H >> (i + 1) : 1) : g_rad[g_radN - 1].h;
        ok = MakeImage(g_rad[i], F, rw, rh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) && MakeFb(g_rad[i]);
    }
    const VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
    VkDescriptorPoolCreateInfo pc{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 16, 1, &ps };
    ok = ok && vk.CreateDescriptorPool(D, &pc, nullptr, &g_pool) == VK_SUCCESS;
    if (ok) {
        g_setCopyPoint = MakeSet(g_copy.view, g_point);
        g_setCopyLinear = MakeSet(g_copy.view, g_linear);
        for (int i = 0; i <= g_radN; ++i) g_setRad[i] = MakeSet(g_rad[i].view, g_linear);
        g_setGrain = MakeSet(g_grainTex.view, g_wrap);
        ok = g_setCopyPoint && g_setCopyLinear && g_setGrain;
        for (int i = 0; i <= g_radN; ++i) ok = ok && g_setRad[i];
    }
    if (!ok) { SKY_LOG("vkfx: frame-sized resources failed (%ux%u format %d)", W, H, F); FreeSized(); }
    else SKY_LOG("vkfx: resources for %ux%u, %d radiosity halvings", W, H, g_radN);
    return ok;
}

bool InitOnce(const vkhook::Target& t) {
    if (g_initFailed) return false;
    if (F == t.format) return true;
    if (F != VK_FORMAT_UNDEFINED) {  // the swapchain changed format: not seen on device, refuse rather than leak
        SKY_LOG("vkfx: swapchain format changed %d -> %d: post effects off", F, t.format);
        g_initFailed = true;
        return false;
    }
    const vkhook::Device* d = vkhook::Get();
    g_initFailed = !d || !LoadApi(*d) || !InitPipelines(t.format) ||
                   !MakeImage(g_grainTex, VK_FORMAT_R8_UNORM, 64, 64, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
                   !MakeHostBuffer(g_dropVb, MAXDROPS * 6 * sizeof(DropVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
                   !MakeHostBuffer(g_grainStage, 64 * 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (g_initFailed) SKY_LOG("vkfx: init failed: post effects off");
    else SKY_LOG("vkfx: ready (swapchain format %d)", t.format);
    return !g_initFailed;
}

SwapFb* SwapTarget(VkImage image) {
    auto it = g_swapFb.find(image);
    if (it != g_swapFb.end()) return &it->second;
    SwapFb s{};
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = F;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vk.CreateImageView(D, &vi, nullptr, &s.view) != VK_SUCCESS) return nullptr;
    VkFramebufferCreateInfo ci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass = g_rpLoad; ci.attachmentCount = 1; ci.pAttachments = &s.view; ci.width = W; ci.height = H; ci.layers = 1;
    if (vk.CreateFramebuffer(D, &ci, nullptr, &s.fb) != VK_SUCCESS) { vk.DestroyImageView(D, s.view, nullptr); return nullptr; }
    return &(g_swapFb[image] = s);
}

// ---------------------------------------------------------------- recording helpers
VkCommandBuffer C = VK_NULL_HANDLE;
bool g_recorded = false;  // this frame put commands into UE's buffer (it then needs UE's state back)
VkImage g_frameImage = VK_NULL_HANDLE;
VkImageLayout g_frameLayout = VK_IMAGE_LAYOUT_UNDEFINED;
SwapFb* g_frameFb = nullptr;

void Barrier(VkImage image, VkImageLayout from, VkImageLayout to) {
    g_recorded = true;
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk.CmdPipelineBarrier(C, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void FrameLayout(VkImageLayout to) {
    if (g_frameLayout != to) Barrier(g_frameImage, g_frameLayout, to);
    g_frameLayout = to;
}

// PC Grab: the frame into g_copy (sampled afterwards).
void Grab() {
    FrameLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Barrier(g_copy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy r{};
    r.srcSubresource = r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    r.extent = { W, H, 1 };
    vk.CmdCopyImage(C, g_frameImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    Barrier(g_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void BeginPass(VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h) {
    g_recorded = true;
    VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = rp; bi.framebuffer = fb; bi.renderArea = { { 0, 0 }, { w, h } };
    vk.CmdBeginRenderPass(C, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{ 0, 0, (float)w, (float)h, 0, 1 };
    const VkRect2D sc{ { 0, 0 }, { w, h } };
    vk.CmdSetViewport(C, 0, 1, &vp);
    vk.CmdSetScissor(C, 0, 1, &sc);
}

void BeginOnFrame() {
    FrameLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    BeginPass(g_rpLoad, g_frameFb->fb, W, H);
}

// One strip quad: uv rectangle per corner (TL, TR, BL, BR) and p, as PC Quad (12 floats).
void Quad(Pipe pipe, VkDescriptorSet set, const float c[12]) {
    vk.CmdBindPipeline(C, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe[pipe]);
    vk.CmdBindDescriptorSets(C, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, nullptr);
    vk.CmdPushConstants(C, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, c);
    vk.CmdDraw(C, 4, 1, 0, 0);
}

void Rect(Pipe pipe, VkDescriptorSet set, float u0, float v0, float u1, float v1, float p0) {
    const float c[12] = { u0, v0, u1, v0, u0, v1, u1, v1, p0, 0, 0, 0 };
    Quad(pipe, set, c);
}

// One full quad into an intermediate target (PC Rect into an Rt).
void RectInto(Image& dst, Pipe pipe, VkDescriptorSet set, float u0, float v0, float u1, float v1, float p0) {
    BeginPass(g_rpTarget, dst.fb, dst.w, dst.h);
    Rect(pipe, set, u0, v0, u1, v1, p0);
    vk.CmdEndRenderPass(C);
}

// ---------------------------------------------------------------- SpeedFX (gta_sa.exe 1.0 US CPostEffects::SpeedFX, 0x7030A0)
// The frame is copied once, then drawn `passes` times as a full-screen strip quad at alpha 36 with point sampling and
// clamp, each pass shrinking the UV rectangle by shift * 0.0025 per side (a zoom) plus a per-frame random wobble of
// wobble * 0.004 * rand()/32767, with the original's corner signs (BL's v uses the u wobble, as in 0x7030A0).
void SpeedFx(const FxState& s) {
    // Looking behind, the original zeroes every UV offset: each pass redraws the frame onto itself, no visible change.
    if (s.speedFxRow < 0 || s.speedFxRow > 6 || s.speedFxLook == 1) return;
    Grab();
    BeginOnFrame();
    const SpeedFxRow& r = kSpeedFx[s.speedFxRow];
    const bool look = s.speedFxLook != 0;
    const int shift = look ? r.shift / 2 : r.shift, wobble = look ? 0 : r.wobble;  // integer halving, as in 0x7030A0
    const float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    float rx = 0, ry = 0;
    if (wobble > 0) {
        // PC rand() is MSVC's (0..32767); bionic's reaches 2^31-1 and threw the UVs off the image (the torn frame).
        rx = (float)(rand() & 0x7FFF) * 0.000030518509f * (u1 * wobble * 0.004f);
        ry = (float)(rand() & 0x7FFF) * 0.000030518509f * (v1 * wobble * 0.004f);
    }
    const float stepU = u1 * shift * 0.0025f, stepV = v1 * shift * 0.0025f;
    for (int k = 1; k <= r.passes; ++k) {
        // Sideways, the original keeps only the right-edge u offsets (TR, BR): a horizontal stretch from the left.
        const float a = look ? 0 : stepU * k, t = look ? 0 : stepV * k, sr = stepU * k;
        const float c[12] = { u0 + a + rx, v0 + t + ry, u1 - sr - rx, v0 + t + ry,  // TL, TR
                              u0 + a + rx, v1 - t - rx, u1 - sr - rx, v1 - t - ry,  // BL, BR
                              kSpeedFxAlpha / 255.0f, 0, 0, 0 };
        Quad(kCopyAlpha, g_setCopyPoint, c);
    }
    vk.CmdEndRenderPass(C);
}

// ---------------------------------------------------------------- radiosity (CPostEffects::Radiosity, skygfx "PS2" path)
// 1. downsample: m_RadiosityFilterPasses bilinear halvings, starting Offset PS2 pixels in (PS2: 6, up-left);
// 2. D = 2 * (D - limit/2) saturated, limit = m_nHighLightMinIntensity * 128/255;
// 3. added to the frame with SRC_ALPHA/ONE at alpha m_RadiosityIntensity, once.
void Radiosity(const FxState& s) {
    if (!s.radiosity) return;
    const int limit = s.intensityLimit * 128 / 255;
    Grab();
    const float ou = s.radiosityOffset / 640, ov = s.radiosityOffset / 448;
    RectInto(g_rad[0], kCopyOpaque, g_setCopyLinear, ou, ov, 1 + ou, 1 + ov, 1);
    for (int i = 1; i < g_radN; ++i) RectInto(g_rad[i], kCopyOpaque, g_setRad[i - 1], 0, 0, 1, 1, 1);
    RectInto(g_rad[g_radN], kThresh, g_setRad[g_radN - 1], 0, 0, 1, 1, limit / 255.0f);
    BeginOnFrame();
    Rect(kCopyAdd, g_setRad[g_radN], 0, 0, 1, 1, s.radiosityIntensity / 255.0f);
    vk.CmdEndRenderPass(C);
}

// ---------------------------------------------------------------- grain (skygfx CPostEffects::Grain_PS2)
// A 64x64 texture of the PS2 VU random generator's low bytes masked by the strength (a bitmask, as on the PS2),
// regenerated every frame, tiled 5 x 7 times per 640x448 and blended as dst * (1 + 2 * alpha).
void Grain(const FxState& s) {
    if (s.grainMask <= 0 || s.grainStrength <= 0) return;
    const int slot = (int)(g_frame % kRing);
    uint8_t* px = g_grainStage.ptr + g_grainStage.slot * slot;
    uint32_t r = 0x3F800000 | (rand() & 0x7FFF);  // vrinit (PC rand() is 15 bits)
    for (int i = 0; i < 64 * 64; ++i) {
        px[i] = (uint8_t)(r & s.grainMask);
        r = (((r << 1) ^ ((r >> 4) & 1) ^ ((r >> 22) & 1)) & 0x7FFFFF) | 0x3F800000;  // vradvance
    }
    Barrier(g_grainTex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = g_grainStage.slot * slot;
    c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = { 64, 64, 1 };
    vk.CmdCopyBufferToImage(C, g_grainStage.buf, g_grainTex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    Barrier(g_grainTex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    BeginOnFrame();
    Rect(kGrain, g_setGrain, 0, 0, 5.0f * W / 640, 7.0f * H / 448, s.grainStrength);
    vk.CmdEndRenderPass(C);
}

// ---------------------------------------------------------------- neo water drops (skygfx neoWaterdrops.cpp)
// Simulated at a fixed 30 Hz (the original's per-frame steps assume ~30 fps); positions and sizes in frame pixels,
// scaled by height/480 as in skygfx. The drop mask is drawn procedurally (skygfx loads "dropmask" from neo.txd).
struct Drop { float x, y, time, size, uvsize, ttl; uint8_t alpha; bool active, fades; };
struct DropMoving { Drop* drop; float dist; };
Drop g_drops[MAXDROPS];
DropMoving g_moving[MAXDROPSMOVING];
int g_numDrops = 0, g_numMoving = 0, g_splashDuration = -1;
float g_vec[3], g_vecLen = 0, g_distMoved = 0, g_rainAngle = 0, g_lastPos[3], g_scaling = 1;
bool g_dropsEnabled = false, g_havePos = false;
constexpr float MAXSIZE = 15, MINSIZE = 4;
std::mutex g_srcMu;          // splash sources, written from any thread
int g_splash = -1;
float g_dropFill = 0.0f;

int SC(float x) { return (int)(x * g_scaling); }

Drop* PlaceNew(const FxState& s, float x, float y, float size, float ttl, bool fades) {
    if (s.noDrops || g_numDrops >= s.maxDrops) return nullptr;  // [WaterDrops] MaxDrops (skygfx: 2000)
    for (Drop& d : g_drops) {
        if (d.active) continue;
        ++g_numDrops;
        d = { x, y, 0.0f, size, (SC(MAXSIZE) - size + 1.0f) / (SC(MAXSIZE) - SC(MINSIZE) + 1.0f), ttl, 0xFF, true, fades };
        return &d;
    }
    return nullptr;
}

void FillScreenMoving(const FxState& s, float amount) {
    int n = (int)((g_vec[2] <= 5.0f ? 1.0f : 1.5f) * amount * 20.0f);
    while (n-- > 0 && g_numDrops < MAXDROPS && g_numMoving < MAXDROPSMOVING) {
        const float x = (float)(rand() % W), y = (float)(rand() % H);
        const float size = (float)(rand() % (SC(MAXSIZE) - SC(MINSIZE)) + SC(MINSIZE));
        if (Drop* d = PlaceNew(s, x, y, size, 2000.0f, true))
            for (DropMoving& m : g_moving)
                if (!m.drop) { ++g_numMoving; m = { d, 0.0f }; break; }
    }
}

void NewTrace(const FxState& s, DropMoving& m) {
    if (g_numDrops >= MAXDROPS) return;
    m.dist = 0.0f;
    PlaceNew(s, m.drop->x, m.drop->y, (float)SC(MINSIZE), 500.0f, true);
}

void MoveDrop(const FxState& s, DropMoving& m) {
    Drop* d = m.drop;
    if (!d->active) { m.drop = nullptr; --g_numMoving; return; }
    if (g_vec[2] <= 0.0f || g_distMoved <= 0.3f) return;
    if (g_vecLen <= 0.5f || s.firstPerson) {  // movement out of the centre
        const float step = g_vec[2] * 0.2f;
        float dx = d->x - W * 0.5f + g_vec[0];
        float dy = d->y - H * (s.firstPerson ? 1.2f : 0.5f) - g_vec[1];
        const float sum = fabsf(dx) + fabsf(dy);
        if (sum >= 0.001f) { dx /= sum; dy /= sum; }
        m.dist += step;
        if (m.dist > 20.0f) NewTrace(s, m);
        d->x += dx * step;
        d->y += dy * step;
    } else {  // movement when the camera turns
        m.dist += g_vecLen;
        if (m.dist > 20.0f) NewTrace(s, m);
        d->x -= g_vec[0];
        d->y += g_vec[1];
    }
    if (d->x < 0.0f || d->y < 0.0f || d->x > W || d->y > H) { m.drop = nullptr; --g_numMoving; }
}

void DropsTick(const FxState& s) {  // WaterDrops::Process at 30 Hz
    g_scaling = H / 480.0f;
    // CalculateMovement: camera movement in camera space (x right, y up, z forward) x 10. DE's matrix right points
    // right (skygfx negates RW's, which points left).
    const float* c = s.cam;
    float delta[3] = { c[12] - g_lastPos[0], c[13] - g_lastPos[1], c[14] - g_lastPos[2] };
    if (!g_havePos) delta[0] = delta[1] = delta[2] = 0.0f;
    memcpy(g_lastPos, c + 12, sizeof(g_lastPos));
    g_havePos = true;
    g_distMoved = sqrtf(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
    for (int i = 0; i < 3; ++i) g_vec[i] = 10.0f * (c[i * 4] * delta[0] + c[i * 4 + 1] * delta[1] + c[i * 4 + 2] * delta[2]);
    const float fwd = g_vec[1], up = g_vec[2];  // rows are right, forward, up: reorder to right, up, forward
    g_vec[1] = up; g_vec[2] = fwd;
    g_vecLen = sqrtf(g_vec[0] * g_vec[0] + g_vec[1] * g_vec[1]);
    g_dropsEnabled = s.dropsEnabled;
    g_rainAngle = acosf(fminf(fmaxf(c[6], -1.0f), 1.0f)) * 57.29578f;  // forward.z: 0 looking up, 180 looking down

    if (!s.noRain && s.rain != 0.0f && g_dropsEnabled) {  // SprayDrops
        const float t = fmaxf(180.0f - g_rainAngle, 40.0f);
        FillScreenMoving(s, (t - 40.0f) / 150.0f * s.rain * 0.5f);
    }
    int splash;
    float fill;
    {
        std::lock_guard<std::mutex> l(g_srcMu);
        splash = g_splash; g_splash = -1;
        fill = g_dropFill; g_dropFill = 0.0f;
    }
    if (splash >= 0) g_splashDuration = splash;
    if (g_splashDuration >= 0) {
        if (g_numDrops < MAXDROPS) FillScreenMoving(s, 1.0f);
        --g_splashDuration;
    }
    if (fill > 0.0f) FillScreenMoving(s, fill);  // boat splash / wake / water splash particles
    if (g_dropsEnabled)  // ProcessMoving
        for (DropMoving& m : g_moving)
            if (m.drop) MoveDrop(s, m);
    for (Drop& d : g_drops) {  // fade: CTimer::ms_fTimeStep * 1000 / 50 at 30 fps
        if (!d.active) continue;
        d.time += 33.0f;
        if (d.time >= d.ttl) { --g_numDrops; d.active = false; }
        else if (d.fades) d.alpha = (uint8_t)(255 - d.time / d.ttl * 255);
    }
}

void ClearDrops() {
    for (Drop& d : g_drops) d.active = false;
    for (DropMoving& m : g_moving) m.drop = nullptr;
    g_numDrops = g_numMoving = 0;
}

double Now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

void WaterDrops(const FxState& s) {
    static double last = 0, acc = 0;
    const double now = Now();
    if (last == 0) last = now;
    acc += now - last;
    last = now;
    if (acc > 0.25) acc = 1.0 / 30;  // after a stall: one step, not a burst
    if (!s.waterDrops) { if (g_numDrops) ClearDrops(); return; }
    for (; acc >= 1.0 / 30; acc -= 1.0 / 30) DropsTick(s);

    if (!g_dropsEnabled || g_numDrops <= 0 || s.hideDrops) return;
    if (s.noDrops) { ClearDrops(); return; }  // camera underwater: the drops go at once

    const int slot = (int)(g_frame % kRing);
    DropVertex* v = (DropVertex*)(g_dropVb.ptr + g_dropVb.slot * slot);
    uint32_t n = 0;
    static const float xy[8] = { -1, -1, -1, 1, 1, 1, 1, -1 }, uv[8] = { 0, 0, 0, 1, 1, 1, 1, 0 };
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (const Drop& d : g_drops) {  // AddToRenderList
        if (!d.active) continue;
        const float tmp = (d.uvsize * (300.0f - 40.0f) + 40.0f) * g_scaling;  // frame window, scaled like the drop
        const float u1a = fmaxf(d.x - tmp, 0.0f) / W, v1a = fmaxf(d.y - tmp, 0.0f) / H;
        const float u1b = fminf(d.x + tmp, (float)W) / W, v1b = fminf(d.y + tmp, (float)H) / H;
        const float scale = d.size * 0.5f;
        const uint32_t rgba = 0x00FFFFFFu | (uint32_t)d.alpha << 24;
        for (int k : tri)
            v[n++] = { d.x + xy[k * 2] * scale, d.y + xy[k * 2 + 1] * scale, rgba, uv[k * 2], uv[k * 2 + 1],
                       k >= 2 ? u1b : u1a, k % 3 == 0 ? v1b : v1a };
    }
    if (!n) return;
    Grab();
    BeginOnFrame();
    const float c[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.0f / W, 1.0f / H };
    vk.CmdBindPipeline(C, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe[kDrop]);
    vk.CmdBindDescriptorSets(C, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &g_setCopyLinear, 0, nullptr);
    vk.CmdPushConstants(C, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, c);
    const VkDeviceSize off = g_dropVb.slot * slot;
    vk.CmdBindVertexBuffers(C, 0, 1, &g_dropVb.buf, &off);
    vk.CmdDraw(C, n, 1, 0, 0);
    vk.CmdEndRenderPass(C);
}

// ---------------------------------------------------------------- entry (vkhook callbacks, UE's recording thread)
std::mutex g_drawMu;  // Draw and ImageGone
std::atomic<unsigned> g_drawn{0}, g_liveDrops{0};  // for Status (game thread)

// Returns whether anything was recorded into UE's command buffer.
bool Draw(const vkhook::Target& t) {
    FxState s;
    if (!LatestFx(&s) || !s.on) return false;
    std::lock_guard<std::mutex> l(g_drawMu);
    if (!InitOnce(t) || !Sized(t.extent.width, t.extent.height)) return false;
    ++g_frame;
    CollectRetired();
    g_frameFb = SwapTarget(t.image);
    if (!g_frameFb) return false;
    C = t.cb;
    g_recorded = false;
    g_frameImage = t.image;
    g_frameLayout = t.layout;
    WaterDrops(s);
    SpeedFx(s);
    Radiosity(s);
    Grain(s);
    FrameLayout(t.layout);  // as UE left it: the HUD pass starts from here
    g_drawn.store(g_frame);
    g_liveDrops.store((unsigned)g_numDrops);
    static bool logged = false;
    if (!logged) { logged = true; SKY_LOG("vkfx: first frame drawn (%ux%u, layout %d)", W, H, t.layout); }
    return g_recorded;
}

void ImageGone(VkImage image) {
    std::lock_guard<std::mutex> l(g_drawMu);
    auto it = g_swapFb.find(image);
    if (it == g_swapFb.end()) return;
    const SwapFb s = it->second;
    g_swapFb.erase(it);
    Retire([s] { vk.DestroyFramebuffer(D, s.fb, nullptr); vk.DestroyImageView(D, s.view, nullptr); });
}

void Init() { vkhook::SetCallbacks(&Draw, &ImageGone); }

void Status() {
    SKY_LOG("vkfx: drawn=%u presents=%u hits=%u unbegun=%u drops=%u size=%ux%u halvings=%d", g_drawn.load(), vkhook::Presents(),
            vkhook::SceneHits(), vkhook::Unbegun(), g_liveDrops.load(), W, H, g_radN);
}

const Feature kFeature = {"vkfx", Init, nullptr, true, Status};  // early: the callbacks must exist before UE's first frame
const Registrar kRegistrar(kFeature);

}  // namespace

void FxSplash(int frames) {
    std::lock_guard<std::mutex> l(g_srcMu);
    g_splash = frames;
}

void FxDropFill(float amount) {
    std::lock_guard<std::mutex> l(g_srcMu);
    g_dropFill += amount;
}

}  // namespace skygfx
