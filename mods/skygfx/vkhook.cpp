// UE's Vulkan RHI, intercepted to draw the post effects where PC SkyGfxDE draws them: on the frame before the HUD.
// Measured on device (v1.87.0, Adreno, Probes=1): UE renders into the swapchain image itself, 2-3 render passes per
// frame: an optional clear (0 draws), the scene composite (1 draw), then the HUD (9-18 draws). So the frame is complete
// and HUD-free when the first swapchain pass with draws ends; the effects are recorded into UE's command buffer there
// (PC: before backbuffer bind number HudBind). A frame without that pass (none seen) simply gets no effects.
//
// UE's VulkanDynamicAPI resolves every entry point through vkGetInstanceProcAddr / vkGetDeviceProcAddr, so those two
// loader exports are hooked before UE creates its device (early feature) and hand out wrappers, to callers inside
// libUE4.so only: the Flutter UI's Vulkan in the same process is left alone. UE's swapchain gets TRANSFER_SRC usage
// (the effects copy the frame), when the surface supports it.
// [Debug] Probes=1 logs the frame structure ("P7 frame ...": swapchain passes with render area and draw count).
#include <dlfcn.h>
#include <mod/amlmod.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "skygfx.h"
#include "vkhook.h"

using namespace skygfx;

namespace skygfx {
namespace vkhook {
namespace {

Device g_dev;
std::atomic<bool> g_devReady{false};
vkhook::SceneDoneFn g_sceneDone = nullptr;
vkhook::ImageGoneFn g_imageGone = nullptr;

PFN_vkGetInstanceProcAddr o_GetInstanceProcAddr;
PFN_vkGetDeviceProcAddr o_GetDeviceProcAddr;
PFN_vkCreateDevice r_CreateDevice;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR r_SurfaceCaps;
PFN_vkCreateSwapchainKHR r_CreateSwapchain;
PFN_vkDestroySwapchainKHR r_DestroySwapchain;
PFN_vkGetSwapchainImagesKHR r_GetSwapchainImages;
PFN_vkCreateImageView r_CreateImageView;
PFN_vkCreateFramebuffer r_CreateFramebuffer;
PFN_vkCreateRenderPass r_CreateRenderPass;
PFN_vkCreateRenderPass2KHR r_CreateRenderPass2;
PFN_vkCmdBeginRenderPass r_BeginRenderPass;
PFN_vkCmdBeginRenderPass2KHR r_BeginRenderPass2;
PFN_vkCmdEndRenderPass r_EndRenderPass;
PFN_vkCmdEndRenderPass2KHR r_EndRenderPass2;
PFN_vkCmdDraw r_Draw;
PFN_vkCmdDrawIndexed r_DrawIndexed;
PFN_vkQueuePresentKHR r_QueuePresent;
PFN_vkBeginCommandBuffer r_BeginCommandBuffer;
PFN_vkResetCommandBuffer r_ResetCommandBuffer;
PFN_vkAllocateCommandBuffers r_AllocateCommandBuffers;
PFN_vkResetCommandPool r_ResetCommandPool;
PFN_vkCmdBindPipeline r_BindPipeline;
PFN_vkCmdBindDescriptorSets r_BindDescriptorSets;
PFN_vkCmdBindVertexBuffers r_BindVertexBuffers;
PFN_vkCmdBindIndexBuffer r_BindIndexBuffer;
PFN_vkCmdSetViewport r_SetViewport;
PFN_vkCmdSetScissor r_SetScissor;
PFN_vkCmdSetLineWidth r_SetLineWidth;
PFN_vkCmdSetDepthBias r_SetDepthBias;
PFN_vkCmdSetBlendConstants r_SetBlendConstants;
PFN_vkCmdSetDepthBounds r_SetDepthBounds;
PFN_vkCmdSetStencilCompareMask r_SetStencilCompareMask;
PFN_vkCmdSetStencilWriteMask r_SetStencilWriteMask;
PFN_vkCmdSetStencilReference r_SetStencilReference;

// ---------------------------------------------------------------- tracking (g_mu)
struct SwapInfo { VkSwapchainKHR swapchain; VkFormat format; VkExtent2D extent; };
struct Attachment { VkFormat format; VkImageLayout finalLayout; };
std::mutex g_mu;
std::unordered_map<VkSwapchainKHR, SwapInfo> g_swapchains;
std::unordered_map<VkImage, SwapInfo> g_swapImages;
std::unordered_map<VkImageView, VkImage> g_viewImage;
std::unordered_map<VkFramebuffer, std::vector<VkImage>> g_fbImages;
std::unordered_map<VkRenderPass, std::vector<Attachment>> g_rpAttachments;

// The pass being recorded on this thread (a command buffer is recorded on one thread at a time).
struct Pass { bool screen = false; int draws = 0; Target target{}; };
thread_local Pass t_pass;
std::atomic<bool> g_fxDone{false};  // the effects ran since the last present
std::atomic<unsigned> g_presents{0}, g_hits{0};  // see Presents() / SceneHits()
std::atomic<unsigned> g_unbegun{0};  // scene passes in a recording whose vkBeginCommandBuffer was not seen (skipped)

// ---------------------------------------------------------------- UE's command-buffer state (PC: SavedState)
// Binding and dynamic state persist across render passes in a command buffer, and UE's RHI skips commands that set
// state it believes is still set. The effects run between UE's scene pass and its HUD pass, so everything UE set in
// the command buffer is shadowed here as it records it, and replayed after the effects: UE's next pass finds exactly
// the state it left (its flipped viewport, its pipeline, its descriptor sets). Our pipelines keep every state but
// viewport/scissor static, which invalidates UE's other dynamic state too, so that is replayed as well.
struct DescBind {
    uint64_t seq;
    uint32_t first;
    VkPipelineLayout layout;
    std::vector<VkDescriptorSet> sets;
    std::vector<uint32_t> dynamicOffsets;
};
struct Shadow {
    uint64_t seq = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, DescBind> desc;  // graphics binds, keyed by firstSet (latest call each)
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    VkBuffer vb[16] = {}; VkDeviceSize vbOff[16] = {}; uint32_t vbCount = 0;
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize ibOff = 0; VkIndexType ibType = VK_INDEX_TYPE_UINT16;
    bool hasLine = false, hasBias = false, hasBlend = false, hasBounds = false;
    float lineWidth = 1, bias[3] = {}, blend[4] = {}, bounds[2] = {};
    uint32_t compare[2] = {}, write[2] = {}, ref[2] = {};
    uint8_t stencilSet = 0;  // bits: 1 compare front, 2 back, 4 write front, 8 back, 16 ref front, 32 back
    bool begun = false;      // vkBeginCommandBuffer seen for the current recording: the state above is all of it
};
// One shadow per command buffer, never erased (UE pools its command buffers, so this stays small; entries keep their
// address). A buffer may be begun on one thread and recorded on another, so the table is global; the thread-local
// cache keeps the per-command path free of the lock.
std::mutex g_shadowMu;
std::unordered_map<VkCommandBuffer, Shadow> g_shadows;
thread_local VkCommandBuffer t_cachedCb = VK_NULL_HANDLE;
thread_local Shadow* t_cached = nullptr;

Shadow& ShadowFor(VkCommandBuffer cb) {
    if (t_cachedCb != cb) {
        std::lock_guard<std::mutex> l(g_shadowMu);
        t_cached = &g_shadows[cb];
        t_cachedCb = cb;
    }
    return *t_cached;
}

void SetStencil(Shadow& s, VkStencilFaceFlags faces, uint32_t* slot, uint8_t bit, uint32_t v) {
    if (faces & VK_STENCIL_FACE_FRONT_BIT) { slot[0] = v; s.stencilSet |= bit; }
    if (faces & VK_STENCIL_FACE_BACK_BIT) { slot[1] = v; s.stencilSet |= bit << 1; }
}

void Replay(VkCommandBuffer cb) {
    Shadow& s = ShadowFor(cb);
    if (s.pipeline) r_BindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline);
    // The latest bind per firstSet, in recording order: every set ends with the value its last writer gave it.
    std::vector<const DescBind*> order;
    for (auto& kv : s.desc) order.push_back(&kv.second);
    std::sort(order.begin(), order.end(), [](const DescBind* a, const DescBind* b) { return a->seq < b->seq; });
    for (const DescBind* d : order)
        r_BindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->layout, d->first, (uint32_t)d->sets.size(),
                             d->sets.data(), (uint32_t)d->dynamicOffsets.size(), d->dynamicOffsets.data());
    for (uint32_t i = 0; i < s.vbCount; ++i)
        if (s.vb[i]) r_BindVertexBuffers(cb, i, 1, &s.vb[i], &s.vbOff[i]);
    if (s.ib) r_BindIndexBuffer(cb, s.ib, s.ibOff, s.ibType);
    if (!s.viewports.empty()) r_SetViewport(cb, 0, (uint32_t)s.viewports.size(), s.viewports.data());
    if (!s.scissors.empty()) r_SetScissor(cb, 0, (uint32_t)s.scissors.size(), s.scissors.data());
    if (s.hasLine) r_SetLineWidth(cb, s.lineWidth);
    if (s.hasBias) r_SetDepthBias(cb, s.bias[0], s.bias[1], s.bias[2]);
    if (s.hasBlend) r_SetBlendConstants(cb, s.blend);
    if (s.hasBounds) r_SetDepthBounds(cb, s.bounds[0], s.bounds[1]);
    if (s.stencilSet & 1) r_SetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, s.compare[0]);
    if (s.stencilSet & 2) r_SetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, s.compare[1]);
    if (s.stencilSet & 4) r_SetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, s.write[0]);
    if (s.stencilSet & 8) r_SetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, s.write[1]);
    if (s.stencilSet & 16) r_SetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, s.ref[0]);
    if (s.stencilSet & 32) r_SetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, s.ref[1]);
}

// A recording starts from no state. vkResetCommandBuffer / vkResetCommandPool end one without a Begin we can rely
// on to come through the same path, so they clear the shadow too; a pass in a recording that was not seen from its
// Begin gets no effects (its state is unknown, so it could not be handed back).
std::mutex g_poolMu;
std::unordered_map<VkCommandPool, std::vector<VkCommandBuffer>> g_poolBuffers;

VkResult VKAPI_CALL H_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    Shadow& s = ShadowFor(cb);
    s = Shadow{};
    s.begun = true;
    return r_BeginCommandBuffer(cb, bi);
}
VkResult VKAPI_CALL H_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
    ShadowFor(cb) = Shadow{};
    return r_ResetCommandBuffer(cb, f);
}
VkResult VKAPI_CALL H_AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    const VkResult r = r_AllocateCommandBuffers(d, ai, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(g_poolMu);
        std::vector<VkCommandBuffer>& v = g_poolBuffers[ai->commandPool];
        v.insert(v.end(), out, out + ai->commandBufferCount);
    }
    return r;
}
VkResult VKAPI_CALL H_ResetCommandPool(VkDevice d, VkCommandPool pool, VkCommandPoolResetFlags f) {
    {
        std::lock_guard<std::mutex> l(g_poolMu);
        auto it = g_poolBuffers.find(pool);
        if (it != g_poolBuffers.end())
            for (VkCommandBuffer cb : it->second) ShadowFor(cb) = Shadow{};
    }
    return r_ResetCommandPool(d, pool, f);
}
void VKAPI_CALL H_BindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) ShadowFor(cb).pipeline = p;
    r_BindPipeline(cb, bp, p);
}
void VKAPI_CALL H_BindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
                                     uint32_t n, const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn) {
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        Shadow& s = ShadowFor(cb);
        DescBind& d = s.desc[first];
        d.seq = ++s.seq;
        d.first = first;
        d.layout = layout;
        d.sets.assign(sets, sets + n);
        d.dynamicOffsets.assign(dyn, dyn + nd);
    }
    r_BindDescriptorSets(cb, bp, layout, first, n, sets, nd, dyn);
}
void VKAPI_CALL H_BindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) {
    Shadow& s = ShadowFor(cb);
    for (uint32_t i = 0; i < n && first + i < 16; ++i) {
        s.vb[first + i] = b[i];
        s.vbOff[first + i] = o[i];
        if (first + i + 1 > s.vbCount) s.vbCount = first + i + 1;
    }
    r_BindVertexBuffers(cb, first, n, b, o);
}
void VKAPI_CALL H_BindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    Shadow& s = ShadowFor(cb);
    s.ib = b; s.ibOff = o; s.ibType = t;
    r_BindIndexBuffer(cb, b, o, t);
}
void VKAPI_CALL H_SetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    Shadow& s = ShadowFor(cb);
    if (s.viewports.size() < first + n) s.viewports.resize(first + n);
    for (uint32_t i = 0; i < n; ++i) s.viewports[first + i] = v[i];
    r_SetViewport(cb, first, n, v);
}
void VKAPI_CALL H_SetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    Shadow& s = ShadowFor(cb);
    if (s.scissors.size() < first + n) s.scissors.resize(first + n);
    for (uint32_t i = 0; i < n; ++i) s.scissors[first + i] = r[i];
    r_SetScissor(cb, first, n, r);
}
void VKAPI_CALL H_SetLineWidth(VkCommandBuffer cb, float w) {
    Shadow& s = ShadowFor(cb);
    s.hasLine = true; s.lineWidth = w;
    r_SetLineWidth(cb, w);
}
void VKAPI_CALL H_SetDepthBias(VkCommandBuffer cb, float c, float clamp, float slope) {
    Shadow& s = ShadowFor(cb);
    s.hasBias = true; s.bias[0] = c; s.bias[1] = clamp; s.bias[2] = slope;
    r_SetDepthBias(cb, c, clamp, slope);
}
void VKAPI_CALL H_SetBlendConstants(VkCommandBuffer cb, const float b[4]) {
    Shadow& s = ShadowFor(cb);
    s.hasBlend = true; memcpy(s.blend, b, sizeof s.blend);
    r_SetBlendConstants(cb, b);
}
void VKAPI_CALL H_SetDepthBounds(VkCommandBuffer cb, float lo, float hi) {
    Shadow& s = ShadowFor(cb);
    s.hasBounds = true; s.bounds[0] = lo; s.bounds[1] = hi;
    r_SetDepthBounds(cb, lo, hi);
}
void VKAPI_CALL H_SetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
    SetStencil(ShadowFor(cb), f, ShadowFor(cb).compare, 1, v);
    r_SetStencilCompareMask(cb, f, v);
}
void VKAPI_CALL H_SetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
    SetStencil(ShadowFor(cb), f, ShadowFor(cb).write, 4, v);
    r_SetStencilWriteMask(cb, f, v);
}
void VKAPI_CALL H_SetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
    SetStencil(ShadowFor(cb), f, ShadowFor(cb).ref, 16, v);
    r_SetStencilReference(cb, f, v);
}

// Probes=1 frame log
std::mutex g_seqMu;
std::string g_seq;
int g_seqPasses = 0;
unsigned g_frames = 0;

bool FromUE(void* ret) {
    Dl_info info;
    return dladdr(ret, &info) && info.dli_fname && strstr(info.dli_fname, "libUE4.so");
}

VkResult VKAPI_CALL H_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a,
                                   VkDevice* out) {
    const VkResult r = r_CreateDevice(pd, ci, a, out);
    if (r == VK_SUCCESS && !g_devReady.load()) {
        g_dev.physical = pd;
        g_dev.device = *out;
        g_dev.gdpa = o_GetDeviceProcAddr;
        g_dev.gipa = o_GetInstanceProcAddr;
        g_devReady.store(true);
        SKY_LOG("vk: UE device %p", (void*)*out);
    }
    return r;
}

VkResult VKAPI_CALL H_CreateSwapchain(VkDevice d, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a,
                                      VkSwapchainKHR* out) {
    VkSwapchainCreateInfoKHR info = *ci;
    VkSurfaceCapabilitiesKHR caps{};
    const bool canCopy = r_SurfaceCaps && g_dev.physical &&
                         r_SurfaceCaps(g_dev.physical, ci->surface, &caps) == VK_SUCCESS &&
                         (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (canCopy) info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkResult r = r_CreateSwapchain(d, &info, a, out);
    SKY_LOG("vk: swapchain %ux%u format %d usage 0x%x (%s) -> %d", info.imageExtent.width, info.imageExtent.height,
            info.imageFormat, info.imageUsage, canCopy ? "copyable" : "NOT copyable: post effects off", r);
    if (r == VK_SUCCESS && canCopy) {
        std::lock_guard<std::mutex> l(g_mu);
        g_swapchains[*out] = { *out, info.imageFormat, info.imageExtent };
    }
    return r;
}

void VKAPI_CALL H_DestroySwapchain(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks* a) {
    std::vector<VkImage> gone;
    {
        std::lock_guard<std::mutex> l(g_mu);
        for (auto it = g_swapImages.begin(); it != g_swapImages.end();)
            if (it->second.swapchain == sc) { gone.push_back(it->first); it = g_swapImages.erase(it); }
            else ++it;
        g_swapchains.erase(sc);
    }
    if (g_imageGone)
        for (VkImage img : gone) g_imageGone(img);
    r_DestroySwapchain(d, sc, a);
}

VkResult VKAPI_CALL H_GetSwapchainImages(VkDevice d, VkSwapchainKHR sc, uint32_t* n, VkImage* images) {
    const VkResult r = r_GetSwapchainImages(d, sc, n, images);
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && images) {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_swapchains.find(sc);
        if (it != g_swapchains.end())
            for (uint32_t i = 0; i < *n; ++i) g_swapImages[images[i]] = it->second;
    }
    return r;
}

VkResult VKAPI_CALL H_CreateImageView(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a,
                                      VkImageView* out) {
    const VkResult r = r_CreateImageView(d, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(g_mu);
        g_viewImage[*out] = ci->image;
    }
    return r;
}

VkResult VKAPI_CALL H_CreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo* ci, const VkAllocationCallbacks* a,
                                        VkFramebuffer* out) {
    const VkResult r = r_CreateFramebuffer(d, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(g_mu);
        std::vector<VkImage>& v = g_fbImages[*out];
        v.assign(ci->attachmentCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < ci->attachmentCount && ci->pAttachments; ++i) {
            auto it = g_viewImage.find(ci->pAttachments[i]);
            if (it != g_viewImage.end()) v[i] = it->second;
        }
    }
    return r;
}

VkResult VKAPI_CALL H_CreateRenderPass(VkDevice d, const VkRenderPassCreateInfo* ci, const VkAllocationCallbacks* a,
                                       VkRenderPass* out) {
    const VkResult r = r_CreateRenderPass(d, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(g_mu);
        std::vector<Attachment>& v = g_rpAttachments[*out];
        v.clear();
        for (uint32_t i = 0; i < ci->attachmentCount; ++i) v.push_back({ ci->pAttachments[i].format, ci->pAttachments[i].finalLayout });
    }
    return r;
}

VkResult VKAPI_CALL H_CreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2* ci, const VkAllocationCallbacks* a,
                                        VkRenderPass* out) {
    const VkResult r = r_CreateRenderPass2(d, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(g_mu);
        std::vector<Attachment>& v = g_rpAttachments[*out];
        v.clear();
        for (uint32_t i = 0; i < ci->attachmentCount; ++i) v.push_back({ ci->pAttachments[i].format, ci->pAttachments[i].finalLayout });
    }
    return r;
}

// Classifies the pass: does it render into a swapchain image, and in which layout does it leave it.
void BeginPass(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb, const VkRect2D& area) {
    t_pass = Pass{};
    std::lock_guard<std::mutex> l(g_mu);
    auto f = g_fbImages.find(fb);
    auto p = g_rpAttachments.find(rp);
    if (f == g_fbImages.end() || p == g_rpAttachments.end()) return;
    for (size_t i = 0; i < f->second.size() && i < p->second.size(); ++i) {
        auto s = g_swapImages.find(f->second[i]);
        if (s == g_swapImages.end()) continue;
        t_pass.screen = true;
        t_pass.target = { cb, s->first, s->second.format, s->second.extent, p->second[i].finalLayout };
        break;
    }
    if (t_pass.screen && cfg.probes) {
        std::lock_guard<std::mutex> sl(g_seqMu);
        char buf[40];
        snprintf(buf, sizeof buf, " S(%ux%u", area.extent.width, area.extent.height);
        g_seq += buf;
        ++g_seqPasses;
    }
}

// After the pass ended: the first swapchain pass with draws of the frame is the scene composite.
void EndPass() {
    if (!t_pass.screen) return;
    if (cfg.probes) {
        std::lock_guard<std::mutex> sl(g_seqMu);
        char buf[16];
        snprintf(buf, sizeof buf, ":%d)", t_pass.draws);
        g_seq += buf;
    }
    if (t_pass.draws > 0 && g_sceneDone && !g_fxDone.exchange(true)) {
        g_hits.fetch_add(1);
        if (!ShadowFor(t_pass.target.cb).begun) g_unbegun.fetch_add(1);
        else if (g_sceneDone(t_pass.target)) Replay(t_pass.target.cb);  // UE's next pass must find the state it left
    }
    t_pass.screen = false;
}

void VKAPI_CALL H_BeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents c) {
    r_BeginRenderPass(cb, bi, c);
    BeginPass(cb, bi->renderPass, bi->framebuffer, bi->renderArea);
}

void VKAPI_CALL H_BeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, const VkSubpassBeginInfo* si) {
    r_BeginRenderPass2(cb, bi, si);
    BeginPass(cb, bi->renderPass, bi->framebuffer, bi->renderArea);
}

void VKAPI_CALL H_EndRenderPass(VkCommandBuffer cb) {
    r_EndRenderPass(cb);
    EndPass();
}

void VKAPI_CALL H_EndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo* ei) {
    r_EndRenderPass2(cb, ei);
    EndPass();
}

void VKAPI_CALL H_Draw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    if (t_pass.screen) ++t_pass.draws;
    r_Draw(cb, vc, ic, fv, fi);
}

void VKAPI_CALL H_DrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t fin) {
    if (t_pass.screen) ++t_pass.draws;
    r_DrawIndexed(cb, ic, inst, fi, vo, fin);
}

VkResult VKAPI_CALL H_QueuePresent(VkQueue q, const VkPresentInfoKHR* pi) {
    g_fxDone.store(false);
    g_presents.fetch_add(1);
    if (cfg.probes) {
        std::lock_guard<std::mutex> sl(g_seqMu);
        if (++g_frames % 300 == 0)
            SKY_LOG("P7 frame %u tid=%ld passes=%d:%.300s", g_frames, (long)syscall(SYS_gettid), g_seqPasses, g_seq.c_str());
        g_seq.clear();
        g_seqPasses = 0;
    }
    return r_QueuePresent(q, pi);
}

PFN_vkVoidFunction VKAPI_CALL H_GetDeviceProcAddrFwd(VkDevice d, const char* name);

// Swaps a resolved entry point for its wrapper, keeping the real one. Null stays null.
PFN_vkVoidFunction Wrap(const char* name, PFN_vkVoidFunction real) {
    if (!real) return real;
#define W(n, r, h) if (!strcmp(name, n)) { if (!r) r = (decltype(r))real; return (PFN_vkVoidFunction)&h; }
    W("vkGetDeviceProcAddr", o_GetDeviceProcAddr, H_GetDeviceProcAddrFwd)
    W("vkCreateDevice", r_CreateDevice, H_CreateDevice)
    W("vkCreateSwapchainKHR", r_CreateSwapchain, H_CreateSwapchain)
    W("vkDestroySwapchainKHR", r_DestroySwapchain, H_DestroySwapchain)
    W("vkGetSwapchainImagesKHR", r_GetSwapchainImages, H_GetSwapchainImages)
    W("vkCreateImageView", r_CreateImageView, H_CreateImageView)
    W("vkCreateFramebuffer", r_CreateFramebuffer, H_CreateFramebuffer)
    W("vkCreateRenderPass", r_CreateRenderPass, H_CreateRenderPass)
    W("vkCreateRenderPass2KHR", r_CreateRenderPass2, H_CreateRenderPass2)
    W("vkCreateRenderPass2", r_CreateRenderPass2, H_CreateRenderPass2)
    W("vkCmdBeginRenderPass", r_BeginRenderPass, H_BeginRenderPass)
    W("vkCmdBeginRenderPass2KHR", r_BeginRenderPass2, H_BeginRenderPass2)
    W("vkCmdBeginRenderPass2", r_BeginRenderPass2, H_BeginRenderPass2)
    W("vkCmdEndRenderPass", r_EndRenderPass, H_EndRenderPass)
    W("vkCmdEndRenderPass2KHR", r_EndRenderPass2, H_EndRenderPass2)
    W("vkCmdEndRenderPass2", r_EndRenderPass2, H_EndRenderPass2)
    W("vkCmdDraw", r_Draw, H_Draw)
    W("vkCmdDrawIndexed", r_DrawIndexed, H_DrawIndexed)
    W("vkQueuePresentKHR", r_QueuePresent, H_QueuePresent)
    W("vkBeginCommandBuffer", r_BeginCommandBuffer, H_BeginCommandBuffer)
    W("vkResetCommandBuffer", r_ResetCommandBuffer, H_ResetCommandBuffer)
    W("vkAllocateCommandBuffers", r_AllocateCommandBuffers, H_AllocateCommandBuffers)
    W("vkResetCommandPool", r_ResetCommandPool, H_ResetCommandPool)
    W("vkCmdBindPipeline", r_BindPipeline, H_BindPipeline)
    W("vkCmdBindDescriptorSets", r_BindDescriptorSets, H_BindDescriptorSets)
    W("vkCmdBindVertexBuffers", r_BindVertexBuffers, H_BindVertexBuffers)
    W("vkCmdBindIndexBuffer", r_BindIndexBuffer, H_BindIndexBuffer)
    W("vkCmdSetViewport", r_SetViewport, H_SetViewport)
    W("vkCmdSetScissor", r_SetScissor, H_SetScissor)
    W("vkCmdSetLineWidth", r_SetLineWidth, H_SetLineWidth)
    W("vkCmdSetDepthBias", r_SetDepthBias, H_SetDepthBias)
    W("vkCmdSetBlendConstants", r_SetBlendConstants, H_SetBlendConstants)
    W("vkCmdSetDepthBounds", r_SetDepthBounds, H_SetDepthBounds)
    W("vkCmdSetStencilCompareMask", r_SetStencilCompareMask, H_SetStencilCompareMask)
    W("vkCmdSetStencilWriteMask", r_SetStencilWriteMask, H_SetStencilWriteMask)
    W("vkCmdSetStencilReference", r_SetStencilReference, H_SetStencilReference)
#undef W
    return real;
}

// vkGetDeviceProcAddr as handed to UE through vkGetInstanceProcAddr.
PFN_vkVoidFunction VKAPI_CALL H_GetDeviceProcAddrFwd(VkDevice d, const char* name) {
    return Wrap(name, o_GetDeviceProcAddr(d, name));
}

// The loader exports themselves: wrappers only for libUE4.so.
PFN_vkVoidFunction VKAPI_CALL H_GetDeviceProcAddr(VkDevice d, const char* name) {
    PFN_vkVoidFunction real = o_GetDeviceProcAddr(d, name);
    return FromUE(__builtin_return_address(0)) ? Wrap(name, real) : real;
}

PFN_vkVoidFunction VKAPI_CALL H_GetInstanceProcAddr(VkInstance i, const char* name) {
    PFN_vkVoidFunction real = o_GetInstanceProcAddr(i, name);
    if (!FromUE(__builtin_return_address(0))) return real;
    if (i && !g_dev.instance) g_dev.instance = i;
    if (i && !r_SurfaceCaps)
        r_SurfaceCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)o_GetInstanceProcAddr(i, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    return Wrap(name, real);
}

void Init() {
    void* vk = aml->GetLibHandle("libvulkan.so");
    void* gipa = vk ? (void*)aml->GetSym(vk, "vkGetInstanceProcAddr") : nullptr;
    void* gdpa = vk ? (void*)aml->GetSym(vk, "vkGetDeviceProcAddr") : nullptr;
    if (!gipa || !gdpa || !aml->Hook(gipa, (void*)&H_GetInstanceProcAddr, (void**)&o_GetInstanceProcAddr) ||
        !aml->Hook(gdpa, (void*)&H_GetDeviceProcAddr, (void**)&o_GetDeviceProcAddr)) {
        SKY_LOG("vk: cannot hook the Vulkan loader (lib %p gipa %p gdpa %p): post effects off", vk, gipa, gdpa);
        return;
    }
    SKY_LOG("vk: loader hooked");
}

const Feature kFeature = {"vkhook", Init, nullptr, true};  // early: before UE creates its Vulkan device
const Registrar kRegistrar(kFeature);

}  // namespace

const Device* Get() { return g_devReady.load() ? &g_dev : nullptr; }

unsigned Presents() { return g_presents.load(); }
unsigned SceneHits() { return g_hits.load(); }
unsigned Unbegun() { return g_unbegun.load(); }

void SetCallbacks(SceneDoneFn sceneDone, ImageGoneFn imageGone) {
    g_imageGone = imageGone;
    g_sceneDone = sceneDone;
}

}  // namespace vkhook
}  // namespace skygfx
