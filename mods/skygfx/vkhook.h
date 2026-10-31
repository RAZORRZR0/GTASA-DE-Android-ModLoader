// Interception of UE's Vulkan RHI (vkhook.cpp), the GPU side of the post effects (vkfx.cpp).
#pragma once
#include <vulkan/vulkan.h>

namespace skygfx {
namespace vkhook {

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;  // the loader's own (no wrappers)
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
};
// UE's device once it exists (else null). Entry points resolved through it are the real ones, never the wrappers.
const Device* Get();

// The frame's scene is complete in `image` (a swapchain image, in `layout`) and the HUD is not drawn yet: called on the
// thread recording `cb`, right after the first swapchain render pass with draws ends, at most once per present. The
// callback records its work into `cb` and must leave `image` in `layout` again.
struct Target {
    VkCommandBuffer cb;
    VkImage image;
    VkFormat format;
    VkExtent2D extent;
    VkImageLayout layout;
};
// Returns whether it recorded anything into t.cb.
typedef bool (*SceneDoneFn)(const Target& t);
// A swapchain image is gone (its swapchain was destroyed): drop anything made for it.
typedef void (*ImageGoneFn)(VkImage image);
void SetCallbacks(SceneDoneFn sceneDone, ImageGoneFn imageGone);

// Presents by UE so far, and frames whose scene pass got the effects: they must grow together (else flicker).
unsigned Presents();
unsigned SceneHits();
// Scene passes skipped because their recording was not seen from vkBeginCommandBuffer (state unknown).
unsigned Unbegun();

}  // namespace vkhook
}  // namespace skygfx
