// Cairn Engine - offscreen Vulkan rendering backend (internal).
// Renders the shared FrameDrawList into a FrameTarget: color into the RGBA8
// host buffer the addon blits, depth when MSAA is off. No surface, no
// swapchain - every frame ends with vkCmdCopyImageToBuffer.
#pragma once

#include "gpu_types.h"

#include <memory>
#include <string>

namespace Cairn {

class VulkanBackend {
public:
    VulkanBackend();
    ~VulkanBackend();
    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    // Creates instance/device/pipelines. Returns false with a logged reason;
    // the caller then falls back to the software backend for the session.
    bool Initialize(std::string* error);

    // Renders one frame and fills `target` (color always, depth when MSAA is
    // off) plus the backend-owned RenderStats fields. Blocking: the call
    // waits for the GPU and reads the pixels back before returning.
    void RenderFrame(const FrameDrawList& list, FrameTarget* target, RenderStats* stats,
                     int requestedSamples);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Cairn
