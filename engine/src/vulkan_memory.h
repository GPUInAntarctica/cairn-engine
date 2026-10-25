// Cairn Engine - Vulkan device memory selection (internal).
//
// Which VkMemoryType a buffer or image lands in is a pure decision: given the
// device's memory properties and the allocation's typeBits, the same answer has
// to come out every time. Backend code makes those decisions here instead of
// hand-writing a search per allocation, so one selection routine covers every
// site - and the routine can be tested against memory properties a test builds
// by hand, which is the only way to pin the choice a device would make without
// that device on the desk.
#pragma once

#include <vulkan/vulkan.h>

namespace Cairn {
namespace VkMemory {

constexpr VkMemoryPropertyFlags kHostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
constexpr VkMemoryPropertyFlags kHostCoherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags kHostCached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
constexpr VkMemoryPropertyFlags kDeviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

// True when the memory type at `type` carries every bit in `prop`. A type the
// device does not report is false, so the check is safe on the short
// VkPhysicalDeviceMemoryProperties a test synthesises.
inline bool HasProperty(const VkPhysicalDeviceMemoryProperties& mem, uint32_t type,
                        VkMemoryPropertyFlags prop) {
    if (type >= VK_MAX_MEMORY_TYPES) return false;
    return (mem.memoryTypes[type].propertyFlags & prop) == prop;
}

// Index of the first memory type compatible with `typeBits` that carries every
// bit in `want` and, if the device offers one, every bit in `prefer`. Order is
// the device's own: Vulkan does not rank memory types, so "first compatible
// type carrying all of `want`" is the answer every caller gets, and the second
// pass dropping `prefer` is what keeps a device without that combination
// allocating instead of failing.
//
// Returns UINT32_MAX when no compatible type carries `want`, which the caller
// treats as an allocation failure rather than as permission to pick something
// unrelated.
inline uint32_t SelectType(const VkPhysicalDeviceMemoryProperties& mem, uint32_t typeBits,
                           VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer = 0) {
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const VkMemoryPropertyFlags props = (pass == 0) ? (want | prefer) : want;
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
            if (!(typeBits & (1u << i))) continue;
            if ((mem.memoryTypes[i].propertyFlags & props) == props) return i;
        }
    }
    return UINT32_MAX;
}

// What the buffer a frame is read back through has to be: host-visible, so the
// pixels can be mapped, and coherent, so the read needs no explicit
// vkInvalidateMappedMemoryRanges.
constexpr VkMemoryPropertyFlags kStaging = kHostVisible | kHostCoherent;

// Host-cached is preferred for it. That buffer is written by the device and
// read on the host, through PCIe: an uncached heap delivers those reads one
// cache line at a time, which for a 1080p frame is several tens of
// milliseconds - more than the frame that produced them costs. A device with
// no cached host heap falls back to the coherent type it always used, so
// asking costs it nothing.
constexpr VkMemoryPropertyFlags kStagingPreferCached = kHostCached;

inline uint32_t SelectStagingType(const VkPhysicalDeviceMemoryProperties& mem, uint32_t typeBits,
                                  bool cached) {
    return SelectType(mem, typeBits, kStaging, cached ? kStagingPreferCached : 0);
}

}  // namespace VkMemory
}  // namespace Cairn
