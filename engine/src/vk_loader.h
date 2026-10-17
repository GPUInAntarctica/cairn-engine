// Cairn Engine - minimal dynamic Vulkan loader (volk-style, header-only).
// The engine deliberately does NOT link libvulkan: the loader is resolved at
// runtime with dlopen, so the same binary serves machines without any Vulkan
// driver (the backend simply reports unavailable and the software renderer
// takes over). Requires <vulkan/vulkan.h> headers at compile time only.
#pragma once

#ifdef CAIRN_HAS_VULKAN

#include <dlfcn.h>

#include <vulkan/vulkan.h>

namespace Cairn {
namespace vk {

// Every entry point the backend uses. Globals come from the library directly;
// instance/device functions are resolved through vkGetInstanceProcAddr (the
// loader trampoline is fine at this draw count).
#define CAIRN_VK_GLOBAL_FUNCS(X)                                    \
    X(CreateInstance, vkCreateInstance)                             \
    X(EnumerateInstanceVersion, vkEnumerateInstanceVersion)         \
    X(EnumerateInstanceExtensionProperties, vkEnumerateInstanceExtensionProperties) \
    X(EnumerateInstanceLayerProperties, vkEnumerateInstanceLayerProperties)

#define CAIRN_VK_INSTANCE_FUNCS(X)                                  \
    X(EnumeratePhysicalDevices, vkEnumeratePhysicalDevices)         \
    X(GetPhysicalDeviceProperties, vkGetPhysicalDeviceProperties)   \
    X(GetPhysicalDeviceFeatures, vkGetPhysicalDeviceFeatures)       \
    X(GetPhysicalDeviceFeatures2, vkGetPhysicalDeviceFeatures2)     \
    X(GetPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceMemoryProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties, vkGetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceFormatProperties, vkGetPhysicalDeviceFormatProperties) \
    X(CreateDevice, vkCreateDevice)                         \
    X(DestroyInstance, vkDestroyInstance)

#define CAIRN_VK_DEVICE_FUNCS(X)                                    \
    X(GetDeviceQueue, vkGetDeviceQueue)                             \
    X(DeviceWaitIdle, vkDeviceWaitIdle)                             \
    X(DestroyDevice, vkDestroyDevice)                               \
    X(CreateCommandPool, vkCreateCommandPool)                       \
    X(DestroyCommandPool, vkDestroyCommandPool)                     \
    X(AllocateCommandBuffers, vkAllocateCommandBuffers)             \
    X(FreeCommandBuffers, vkFreeCommandBuffers)                     \
    X(ResetCommandPool, vkResetCommandPool)                         \
    X(CreateFence, vkCreateFence)                                   \
    X(DestroyFence, vkDestroyFence)                                 \
    X(ResetFences, vkResetFences)                                   \
    X(WaitForFences, vkWaitForFences)                               \
    X(CreateBuffer, vkCreateBuffer)                                 \
    X(DestroyBuffer, vkDestroyBuffer)                               \
    X(CreateImage, vkCreateImage)                                   \
    X(DestroyImage, vkDestroyImage)                                 \
    X(AllocateMemory, vkAllocateMemory)                             \
    X(FreeMemory, vkFreeMemory)                                     \
    X(MapMemory, vkMapMemory)                                       \
    X(UnmapMemory, vkUnmapMemory)                                   \
    X(BindBufferMemory, vkBindBufferMemory)                         \
    X(BindImageMemory, vkBindImageMemory)                           \
    X(GetBufferMemoryRequirements, vkGetBufferMemoryRequirements)   \
    X(GetImageMemoryRequirements, vkGetImageMemoryRequirements)     \
    X(CreateImageView, vkCreateImageView)                           \
    X(DestroyImageView, vkDestroyImageView)                         \
    X(CreateSampler, vkCreateSampler)                               \
    X(DestroySampler, vkDestroySampler)                             \
    X(CreateShaderModule, vkCreateShaderModule)                     \
    X(DestroyShaderModule, vkDestroyShaderModule)                   \
    X(CreatePipelineLayout, vkCreatePipelineLayout)                 \
    X(DestroyPipelineLayout, vkDestroyPipelineLayout)               \
    X(CreateGraphicsPipelines, vkCreateGraphicsPipelines)           \
    X(DestroyPipeline, vkDestroyPipeline)                           \
    X(CreateDescriptorSetLayout, vkCreateDescriptorSetLayout)       \
    X(DestroyDescriptorSetLayout, vkDestroyDescriptorSetLayout)     \
    X(CreateDescriptorPool, vkCreateDescriptorPool)                 \
    X(DestroyDescriptorPool, vkDestroyDescriptorPool)               \
    X(AllocateDescriptorSets, vkAllocateDescriptorSets)             \
    X(ResetDescriptorPool, vkResetDescriptorPool)                   \
    X(UpdateDescriptorSets, vkUpdateDescriptorSets)                 \
    X(BeginCommandBuffer, vkBeginCommandBuffer)                     \
    X(EndCommandBuffer, vkEndCommandBuffer)                         \
    X(QueueSubmit, vkQueueSubmit)                                   \
    X(QueueWaitIdle, vkQueueWaitIdle)                               \
    X(CmdBeginRendering, vkCmdBeginRendering)                       \
    X(CmdEndRendering, vkCmdEndRendering)                           \
    X(CmdBindPipeline, vkCmdBindPipeline)                           \
    X(CmdSetViewport, vkCmdSetViewport)                             \
    X(CmdSetScissor, vkCmdSetScissor)                               \
    X(CmdSetCullMode, vkCmdSetCullMode)                             \
    X(CmdSetFrontFace, vkCmdSetFrontFace)                           \
    X(CmdBindDescriptorSets, vkCmdBindDescriptorSets)               \
    X(CmdBindVertexBuffers, vkCmdBindVertexBuffers)                 \
    X(CmdBindIndexBuffer, vkCmdBindIndexBuffer)                     \
    X(CmdDraw, vkCmdDraw)                                           \
    X(CmdDrawIndexed, vkCmdDrawIndexed)                             \
    X(CmdPushConstants, vkCmdPushConstants)                         \
    X(CmdPipelineBarrier2, vkCmdPipelineBarrier2)                   \
    X(CmdCopyBuffer, vkCmdCopyBuffer)                               \
    X(CmdCopyBufferToImage, vkCmdCopyBufferToImage)                 \
    X(CmdCopyImageToBuffer, vkCmdCopyImageToBuffer)                       \
    X(CmdFillBuffer, vkCmdFillBuffer)

struct Funcs {
#define CAIRN_VK_DECL(name, sym) PFN_##sym name = nullptr;
    CAIRN_VK_GLOBAL_FUNCS(CAIRN_VK_DECL)
    CAIRN_VK_INSTANCE_FUNCS(CAIRN_VK_DECL)
    CAIRN_VK_DEVICE_FUNCS(CAIRN_VK_DECL)
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
#undef CAIRN_VK_DECL
    // True once Resolve() found every instance/device entry point.
    bool ok = false;
};

// Loads libvulkan and resolves the global commands. Instance- and device-level
// commands are deliberately NOT resolved here: the spec lets an implementation
// return NULL from vkGetInstanceProcAddr when instance is NULL, and the major
// loaders do exactly that. Call Resolve() once an instance exists.
inline bool LoadFuncs(Funcs* out) {
    static const char* kCandidates[] = {
        "libvulkan.so.1", "libvulkan.so", "libvulkan.dylib", "vulkan-1.dll",
    };
    void* lib = nullptr;
    for (const char* name : kCandidates) {
        lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (lib) break;
    }
    if (!lib) return false;

    out->GetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!out->GetInstanceProcAddr) return false;

#define CAIRN_VK_LOAD_GLOBAL(name, sym) \
    out->name = reinterpret_cast<PFN_##sym>(dlsym(lib, #sym)); \
    if (!out->name) return false;
    CAIRN_VK_GLOBAL_FUNCS(CAIRN_VK_LOAD_GLOBAL)
#undef CAIRN_VK_LOAD_GLOBAL
    return true;
}

// Resolves instance- and device-level entry points against a live instance.
// Returns false when the driver is missing anything the backend needs, which
// is how a Vulkan 1.2 driver is rejected without linking the loader.
inline bool Resolve(Funcs* f, VkInstance instance) {
    if (!f->GetInstanceProcAddr) return false;
    bool complete = true;
#define CAIRN_VK_RESOLVE(name, sym)                                    \
    f->name = reinterpret_cast<PFN_##sym>(f->GetInstanceProcAddr(instance, #sym)); \
    if (!f->name) complete = false;
    CAIRN_VK_INSTANCE_FUNCS(CAIRN_VK_RESOLVE)
#undef CAIRN_VK_RESOLVE
    // Device-level commands are only reachable through vkGetDeviceProcAddr.
    // Querying them from vkGetInstanceProcAddr is not just discouraged: it
    // hands back stubs on several drivers, which fail silently at draw time.
    f->GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        f->GetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
    if (!f->GetDeviceProcAddr) {
        f->ok = false;
        return false;
    }
    return complete;
}

// Resolves the device commands once the VkDevice exists.
inline bool ResolveDevice(Funcs* f, VkDevice device) {
    if (!f->GetDeviceProcAddr) return false;
    bool complete = true;
#define CAIRN_VK_RESOLVE_DEV(name, sym)                                 \
    f->name = reinterpret_cast<PFN_##sym>(f->GetDeviceProcAddr(device, #sym)); \
    if (!f->name) complete = false;
    CAIRN_VK_DEVICE_FUNCS(CAIRN_VK_RESOLVE_DEV)
#undef CAIRN_VK_RESOLVE_DEV
    f->ok = complete;
    return complete;
}

}  // namespace vk
}  // namespace Cairn

#endif  // CAIRN_HAS_VULKAN