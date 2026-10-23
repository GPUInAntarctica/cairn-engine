// Cairn Engine - offscreen Vulkan rendering backend.
//
// The backend never touches a Scene: the shared front end in renderer.cpp
// hands it a FrameDrawList and it fills a FrameTarget. There is no surface and
// no swapchain - every frame is recorded into one command buffer, submitted
// once, and read back with vkCmdCopyImageToBuffer into the host buffer the
// addon blits. libvulkan is resolved with dlopen (vk_loader.h), so a machine
// with no driver simply reports the backend unavailable and the software
// rasterizer takes over.
#include "backend_vulkan.h"

#include "cairn/log.h"
#include "vk_loader.h"

#ifdef CAIRN_HAS_VULKAN

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>

#include "shaders_spirv.h"

namespace Cairn {
namespace {

constexpr uint32_t kRequiredInstanceVersion = VK_API_VERSION_1_3;
constexpr VkSampleCountFlagBits kSampleLadder[] = {VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT,
                                                    VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT};

// std140 blocks the shaders declare. Everything is float4-aligned by
// construction, so packing is a straight append.
struct FrameUbo {
    float viewProj[16];
    float camPosOrtho[4];
    float camForward[4];
    float camRightTH[4];
    float camUp[4];
    float ambient[4];
    float fogColorDensity[4];
    float misc[4];
    float skyTop[4];
    float skyBottom[4];
    float viewport[4];
    float clearColor[4];
};
static_assert(sizeof(FrameUbo) == 240, "FrameUBO layout must match mesh.vert");

struct LightsUbo {
    float light[16][16];  // 4 vec4 per light
    // mesh.frag declares `int lightCount`, so this slot holds an int. Writing
    // a float here would be read back as a bit pattern: 1.0f is 1065353216,
    // and the shader's light loop then runs a billion times off the end of the
    // block, which faults the GPU outright.
    int32_t count = 0;
    float pad[3];
};
static_assert(sizeof(LightsUbo) == 16 * 64 + 16, "LightsUBO layout must match mesh.frag");

struct ShadowUbo {
    float matrix[4][16];
    float splits[4];
    float range[4];
    float texel[4];
    float params[4];
    float toLight[4];
};
static_assert(sizeof(ShadowUbo) == 4 * 64 + 5 * 16, "ShadowUBO layout must match mesh.frag");

struct DrawUbo {
    float model[16];
    float color[4];
    float emissive[4];
    float tilingSpec[4];
    float surf[4];
    float flags[4];
    float misc[4];
    // Bindless sampler slots into the fragment stage's texture arrays: x is the
    // albedo slot, y the normal slot. They replace the per-draw albedo/normal
    // descriptor pair, which is what let a batched run span several materials.
    float texIndex[4];
    // Padded so the std140 array stride in the shader (roundUp(alignof, sizeof))
    // equals the CPU stride (256 = minUniformBufferOffsetAlignment's upper bound).
    // That lets one instanced draw read draws[gl_InstanceIndex] and collapse a run
    // of identical draws into a single vkCmdDrawIndexed. Keep this a multiple of
    // 16 and >= 256.
    float pad[20];
};
static_assert(sizeof(DrawUbo) == 256, "DrawUBO stride must be 256 to match the shader's draws[]");

struct LineVert {
    float p[3];
    float c[4];
};

uint32_t SampleFlags(uint32_t supported) {
    for (VkSampleCountFlagBits s : kSampleLadder)
        if (supported & s) return s;
    return VK_SAMPLE_COUNT_1_BIT;
}

}  // namespace

// ---------------------------------------------------------------------------

struct VulkanBackend::Impl {
    vk::Funcs fn;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkPhysicalDeviceProperties physProps{};
    VkDeviceSize uniformAlignment = 256;
    uint32_t colorSamples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t depthSamples = VK_SAMPLE_COUNT_1_BIT;

    // ---------------------------------------------------------------- resources
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
    };
    struct Image {
        VkImage handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::vector<VkImageView> layerViews;  // one per array layer, when layered
        int w = 0, h = 0, layers = 0;
        // Layout the image is left in once the last recorded command that
        // touched it has run. Render targets persist across frames, so every
        // transition has to start from what the previous frame ended with
        // rather than from a guess.
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    // Descriptor sets the post chain borrows. The scene path no longer needs
    // per-draw sets: it samples through the bindless texture arrays instead.
    static constexpr size_t kPostSlots = 8;
    // Bindless texture array size. Must match the albedoMaps[] / normalMaps[]
    // declarations in mesh.frag and sprite.frag. It is a soft cap: when every
    // slot is live the least recently used one is stolen, so the worst case is
    // one extra draw's worth of slot churn, never a dropped texture.
    static constexpr uint32_t kBindlessSlots = 512;
    // One table per binding (0 albedo, 1 normal). A texture gets a stable slot
    // the first time a frame samples it, so a warm scene writes no descriptors
    // at all. Slot 0 is the 1x1 white stand-in that "no texture" resolves to.
    struct BindlessTable {
        std::map<std::pair<const Texture*, VkSampler>, uint32_t> index;
        std::vector<uint32_t> freeSlots;
        uint32_t next = 1;  // slot 0 stays white
        const Texture* slotTex[kBindlessSlots] = {};
        VkSampler slotSampler[kBindlessSlots] = {};
        uint64_t slotAge[kBindlessSlots] = {};
    };
    BindlessTable bindless[2];

    Buffer frameUbo, lightsUbo, shadowUbo, drawUbo, sceneVerts, sceneIndices;
    Buffer lineVerts, readback;
    Image colorMS, colorResolve, depthMS, depthResolve;
    Image postA, postB, bloomA, bloomB;
    Image shadowMap;

    VkSampler linearRepeat = VK_NULL_HANDLE;  // mipmapped, trilinear
    VkSampler linearClamp = VK_NULL_HANDLE;   // mipmapped, clamp, filtered 2D
    VkSampler pixelClamp = VK_NULL_HANDLE;    // nearest, clamp, no mips: pixel-perfect 2D
    VkSampler nearestClamp = VK_NULL_HANDLE;  // shadow map taps
    Image whiteTex;                            // 1x1 stand-in for "no texture"
    // The shadow binding is a sampler2DArray, so its placeholder has to be a
    // 2D_ARRAY view even though every other placeholder use is a plain 2D one.
    VkImageView whiteArrayView = VK_NULL_HANDLE;
    // Per-chunk frame descriptor sets. A dynamic uniform descriptor may only
    // span maxUniformBufferRange, so the DrawUBO is addressed through a window
    // of frame sets rather than one set covering the whole pool.
    std::vector<VkDescriptorSet> frameSets;
    uint32_t drawUboChunk = 256;
    // Layouts the render targets are left in across frames, so every pass can
    // transition from a known state instead of guessing.

    VkDescriptorSetLayout frameLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout texLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet frameSet = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> postSets;

    VkPipelineLayout scenePipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout postPipeLayout = VK_NULL_HANDLE;

    struct Pipelines {
        VkPipeline mesh = VK_NULL_HANDLE;
        VkPipeline sprite = VK_NULL_HANDLE;
        VkPipeline sky = VK_NULL_HANDLE;
        VkPipeline line = VK_NULL_HANDLE;
        VkPipeline shadow = VK_NULL_HANDLE;
        VkPipeline bright = VK_NULL_HANDLE;
        VkPipeline blur = VK_NULL_HANDLE;
        VkPipeline composite = VK_NULL_HANDLE;
        VkPipeline grading = VK_NULL_HANDLE;
        VkPipeline vignette = VK_NULL_HANDLE;
        VkPipeline fxaa = VK_NULL_HANDLE;
    };
    std::unordered_map<uint32_t, Pipelines> pipelineCache;

    struct MeshEntry {
        uint32_t vertexOffset = 0, indexOffset = 0, indexCount = 0;
        uint32_t vertexCount = 0;
    };
    struct TexEntry {
        VkImageView view = VK_NULL_HANDLE;
        uint32_t mipLevels = 1;
        uint64_t lastFrame = 0;
    };
    std::unordered_map<const Mesh*, MeshEntry> meshes;
    std::unordered_map<const Texture*, TexEntry> textures;
    // GPU-side allocations behind TexEntry::view, kept here so Destroy() (and
    // cache eviction) can free the image the view points at.
    std::unordered_map<const Texture*, Image> textureImages;
    size_t sceneVertexCapacity = 0, sceneIndexCapacity = 0;
    bool sceneBuffersDirty = true;
    uint64_t frameIndex = 0;

    int width = 0, height = 0;
    uint32_t samples = 1;
    std::string failure;  // set when a frame could not be recorded

    ~Impl() { Destroy(); }

    // -------------------------------------------------------------- memory types
    uint32_t FindMemory(uint32_t typeBits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
        for (uint32_t pass = 0; pass < 2; ++pass) {
            const VkMemoryPropertyFlags props =
                (pass == 0) ? (want | avoid) : want;  // second pass drops the `avoid` bits
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if (!(typeBits & (1u << i))) continue;
                if ((memProps.memoryTypes[i].propertyFlags & props) == props) return i;
            }
        }
        return UINT32_MAX;
    }

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* out) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (fn.CreateBuffer(device, &ci, nullptr, &out->handle) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        fn.GetBufferMemoryRequirements(device, out->handle, &req);
        const uint32_t type = FindMemory(req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         0);
        if (type == UINT32_MAX) {
            fn.DestroyBuffer(device, out->handle, nullptr);
            out->handle = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (fn.AllocateMemory(device, &ai, nullptr, &out->memory) != VK_SUCCESS) return false;
        fn.BindBufferMemory(device, out->handle, out->memory, 0);
        void* ptr = nullptr;
        if (fn.MapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) return false;
        out->mapped = ptr;
        out->capacity = size;
        return true;
    }

    void DestroyBuffer(Buffer* b) {
        if (b->handle && b->memory && b->mapped) fn.UnmapMemory(device, b->memory);
        if (b->handle) fn.DestroyBuffer(device, b->handle, nullptr);
        if (b->memory) fn.FreeMemory(device, b->memory, nullptr);
        *b = Buffer{};
    }

    bool EnsureBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* b) {
        if (b->capacity >= size && b->handle) return true;
        DestroyBuffer(b);
        return CreateBuffer(size, usage, b);
    }

    bool CreateImage(int w, int h, int layers, VkFormat format, VkImageUsageFlags usage, Image* out) {
        if (w <= 0 || h <= 0 || layers <= 0) return false;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        ci.mipLevels = 1;
        ci.arrayLayers = static_cast<uint32_t>(layers);
        ci.samples = static_cast<VkSampleCountFlagBits>(samples);
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (fn.CreateImage(device, &ci, nullptr, &out->handle) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        fn.GetImageMemoryRequirements(device, out->handle, &req);
        // Render targets and sampled images want device-local memory. Asking for
        // HOST_VISIBLE here is a trap: on a discrete GPU no image memory type is
        // both, so the fallback silently puts every attachment in system RAM.
        uint32_t type = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        if (type == UINT32_MAX) type = FindMemory(req.memoryTypeBits, 0, 0);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (ai.memoryTypeIndex == UINT32_MAX) return false;
        if (fn.AllocateMemory(device, &ai, nullptr, &out->memory) != VK_SUCCESS) return false;
        fn.BindImageMemory(device, out->handle, out->memory, 0);
        out->w = w;
        out->h = h;
        out->layers = layers;
        out->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Default view: every layer (the shadow array is sampled as a 2D array).
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out->handle;
        vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, static_cast<uint32_t>(layers)};
        vi.subresourceRange.aspectMask =
            (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                   : VK_IMAGE_ASPECT_COLOR_BIT;
        fn.CreateImageView(device, &vi, nullptr, &out->view);
        if (layers > 1) {
            // The shadow array is written one layer at a time, so keep a
            // per-layer view alongside the full 2D array used for sampling.
            for (int l = 0; l < layers; ++l) {
                vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vi.subresourceRange.baseArrayLayer = static_cast<uint32_t>(l);
                vi.subresourceRange.layerCount = 1;
                VkImageView v = VK_NULL_HANDLE;
                fn.CreateImageView(device, &vi, nullptr, &v);
                out->layerViews.push_back(v);
            }
        }
        return true;
    }

    void DestroyImage(Image* img) {
        if (img->view) fn.DestroyImageView(device, img->view, nullptr);
        for (VkImageView v : img->layerViews) fn.DestroyImageView(device, v, nullptr);
        if (img->handle) fn.DestroyImage(device, img->handle, nullptr);
        if (img->memory) fn.FreeMemory(device, img->memory, nullptr);
        *img = Image{};
    }

    // ------------------------------------------------------------------- helpers
    VkShaderModule LoadShader(const char* name) {
        const spirv::Module* m = spirv::Find(name);
        if (!m) return VK_NULL_HANDLE;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = m->byteCount;
        ci.pCode = static_cast<const uint32_t*>(m->code);
        VkShaderModule mod = VK_NULL_HANDLE;
        const uint32_t magic = static_cast<const uint32_t*>(m->code)[0];
        const VkResult res = fn.CreateShaderModule(device, &ci, nullptr, &mod);
        if (res != VK_SUCCESS) return VK_NULL_HANDLE;
        return mod;
    }

    // Transitions `img` to `to` from wherever it currently is, then records
    // that as its resting layout.
    void Transition(VkCommandBuffer cb, Image& img, VkImageAspectFlags aspect, VkImageLayout to,
                    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        const VkImageLayout from = img.layout;
        if (from == to) return;
        const bool firstUse = (from == VK_IMAGE_LAYOUT_UNDEFINED);
        ImageBarrier(cb, img.handle, aspect, from, to,
                     firstUse ? VK_PIPELINE_STAGE_2_NONE : srcStage,
                     firstUse ? 0 : srcAccess, dstStage, dstAccess);
        img.layout = to;
    }

    // Covers every mip level and layer: render targets and sampled textures
    // are created with a full chain, and a barrier that only names mip 0 would
    // leave the rest of the chain in a layout the descriptor does not declare.
    void ImageBarrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                      VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                      VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                      VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        fn.CmdPipelineBarrier2(cb, &dep);
    }

    // ------------------------------------------------------------- initialization
    bool CreatePipelines(uint32_t sampleCount, Pipelines* out);

    void Destroy() {
        if (!device) {
            if (instance) fn.DestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
            return;
        }
        // Nothing may be freed before the device is idle: the last frame's
        // command buffer only holds handles to these objects.
        fn.DeviceWaitIdle(device);
        for (auto& kv : textureImages) DestroyImage(&kv.second);
        textureImages.clear();
        textures.clear();
        meshes.clear();
        for (auto& kv : pipelineCache) {
            for (VkPipeline p : {kv.second.mesh, kv.second.sprite, kv.second.sky, kv.second.line,
                                 kv.second.shadow, kv.second.bright, kv.second.blur,
                                 kv.second.composite, kv.second.grading, kv.second.vignette,
                                 kv.second.fxaa})
                if (p) fn.DestroyPipeline(device, p, nullptr);
        }
        pipelineCache.clear();
        if (scenePipeLayout) fn.DestroyPipelineLayout(device, scenePipeLayout, nullptr);
        if (postPipeLayout) fn.DestroyPipelineLayout(device, postPipeLayout, nullptr);
        for (VkSampler s : {linearRepeat, linearClamp, pixelClamp, nearestClamp})
            if (s) fn.DestroySampler(device, s, nullptr);
        if (whiteArrayView) fn.DestroyImageView(device, whiteArrayView, nullptr);
        DestroyImage(&whiteTex);
        DestroyImage(&colorMS);
        DestroyImage(&colorResolve);
        DestroyImage(&depthMS);
        DestroyImage(&depthResolve);
        DestroyImage(&postA);
        DestroyImage(&postB);
        DestroyImage(&bloomA);
        DestroyImage(&bloomB);
        DestroyImage(&shadowMap);
        DestroyBuffer(&frameUbo);
        DestroyBuffer(&lightsUbo);
        DestroyBuffer(&shadowUbo);
        DestroyBuffer(&drawUbo);
        DestroyBuffer(&sceneVerts);
        DestroyBuffer(&sceneIndices);
        DestroyBuffer(&lineVerts);
        DestroyBuffer(&readback);
        if (descPool) fn.DestroyDescriptorPool(device, descPool, nullptr);
        if (frameLayout) fn.DestroyDescriptorSetLayout(device, frameLayout, nullptr);
        if (texLayout) fn.DestroyDescriptorSetLayout(device, texLayout, nullptr);
        if (bindlessLayout) fn.DestroyDescriptorSetLayout(device, bindlessLayout, nullptr);
        if (fence) fn.DestroyFence(device, fence, nullptr);
        if (pool) fn.DestroyCommandPool(device, pool, nullptr);
        fn.DestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
        if (instance) fn.DestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
};

// ---------------------------------------------------------------------------
// Probe: is there a usable Vulkan 1.3 device? Cheap and cached for the process.
// ---------------------------------------------------------------------------
static bool ProbeVulkan() {
    vk::Funcs f;
    if (!vk::LoadFuncs(&f)) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "cairn-engine";
    app.apiVersion = kRequiredInstanceVersion;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (f.CreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return false;
    if (!vk::Resolve(&f, instance)) {
        f.DestroyInstance(instance, nullptr);
        return false;   // a pre-1.3 driver: dynamic rendering is mandatory here
    }

    uint32_t count = 0;
    f.EnumeratePhysicalDevices(instance, &count, nullptr);
    bool ok = false;
    if (count) {
        std::vector<VkPhysicalDevice> devices(count);
        f.EnumeratePhysicalDevices(instance, &count, devices.data());
        for (VkPhysicalDevice dev : devices) {
            VkPhysicalDeviceProperties props{};
            f.GetPhysicalDeviceProperties(dev, &props);
            // Every draw the backend makes goes through dynamic rendering,
            // which is core (hence mandatory) from Vulkan 1.3 on.
            if (props.apiVersion >= VK_API_VERSION_1_3) {
                ok = true;
                break;
            }
        }
    }
    f.DestroyInstance(instance, nullptr);
    return ok;
}

bool VulkanBackendAvailable() {
    static const bool available = ProbeVulkan();
    return available;
}

// ---------------------------------------------------------------------------

VulkanBackend::VulkanBackend() = default;
VulkanBackend::~VulkanBackend() {
    if (impl_) impl_->Destroy();
}

bool VulkanBackend::Initialize(std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        Log::Warning("Vulkan backend unavailable: %s", what);
        if (impl_) impl_->Destroy();
        return false;
    };
    impl_ = std::make_unique<Impl>();
    Impl& I = *impl_;
    if (!vk::LoadFuncs(&I.fn)) return fail("libvulkan could not be loaded");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "cairn-engine";
    app.apiVersion = kRequiredInstanceVersion;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (I.fn.CreateInstance(&ici, nullptr, &I.instance) != VK_SUCCESS)
        return fail("vkCreateInstance failed");
    if (!vk::Resolve(&I.fn, I.instance)) return fail("driver is missing Vulkan 1.3 entry points");

    uint32_t deviceCount = 0;
    I.fn.EnumeratePhysicalDevices(I.instance, &deviceCount, nullptr);
    if (!deviceCount) return fail("no Vulkan physical device");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    I.fn.EnumeratePhysicalDevices(I.instance, &deviceCount, devices.data());

    // Discrete GPUs first (llvmpipe / software adapters sort last on type).
    std::stable_sort(devices.begin(), devices.end(), [&](VkPhysicalDevice a, VkPhysicalDevice b) {
        auto type = [&](VkPhysicalDevice d) {
            VkPhysicalDeviceProperties p{};
            I.fn.GetPhysicalDeviceProperties(d, &p);
            return p.deviceType;
        };
        auto score = [](VkPhysicalDeviceType t) {
            return t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0 : 1;
        };
        return score(type(a)) < score(type(b));
    });

    uint32_t family = UINT32_MAX;
    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties props{};
        I.fn.GetPhysicalDeviceProperties(dev, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;  // dynamic rendering is core in 1.3
        uint32_t families = 0;
        I.fn.GetPhysicalDeviceQueueFamilyProperties(dev, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        I.fn.GetPhysicalDeviceQueueFamilyProperties(dev, &families, queues.data());
        for (uint32_t i = 0; i < families; ++i) {
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                family = i;
                break;
            }
        }
        if (family != UINT32_MAX) {
            I.physical = dev;
            break;
        }
        family = UINT32_MAX;
    }
    if (family == UINT32_MAX) return fail("no Vulkan device with a graphics queue");
    I.queueFamily = family;
    I.fn.GetPhysicalDeviceProperties(I.physical, &I.physProps);
    I.fn.GetPhysicalDeviceMemoryProperties(I.physical, &I.memProps);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    // The scene path samples albedo / normal maps through bindless texture
    // arrays (one array per binding, indexed per instance), so the device must
    // expose descriptor indexing and dynamic, non-uniform indexing of
    // sampled-image arrays. A device without them falls back to the software
    // rasterizer rather than losing the feature silently.
    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have2.pNext = &have12;
    I.fn.GetPhysicalDeviceFeatures2(I.physical, &have2);
    if (!have2.features.shaderSampledImageArrayDynamicIndexing || !have12.descriptorIndexing ||
        !have12.shaderSampledImageArrayNonUniformIndexing)
        return fail("device lacks descriptor-indexing features for the bindless texture path");
    if (I.physProps.limits.maxPerStageDescriptorSampledImages < 2 * Impl::kBindlessSlots)
        return fail("device cannot bind both bindless texture arrays in one stage");

    // Vulkan 1.3 moves dynamicRendering and synchronization2 out of the core
    // VkPhysicalDeviceFeatures struct; both are opt-in through the 1.3 feature
    // struct and every draw the backend makes depends on them.
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    // 1.2 features chain in front of 1.3; the structs are walked in pNext order.
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.descriptorIndexing = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.pNext = &features13;
    VkPhysicalDeviceFeatures coreFeatures{};
    coreFeatures.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features12;
    dci.pEnabledFeatures = &coreFeatures;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (I.fn.CreateDevice(I.physical, &dci, nullptr, &I.device) != VK_SUCCESS)
        return fail("vkCreateDevice failed");
    if (!vk::ResolveDevice(&I.fn, I.device))
        return fail("driver is missing Vulkan 1.3 device entry points");
    I.fn.GetDeviceQueue(I.device, family, 0, &I.queue);
    I.colorSamples = SampleFlags(I.physProps.limits.framebufferColorSampleCounts);
    I.depthSamples = SampleFlags(I.physProps.limits.framebufferDepthSampleCounts);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (I.fn.CreateCommandPool(I.device, &pci, nullptr, &I.pool) != VK_SUCCESS)
        return fail("vkCreateCommandPool failed");
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = I.pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (I.fn.AllocateCommandBuffers(I.device, &cbi, &I.cmd) != VK_SUCCESS)
        return fail("vkAllocateCommandBuffers failed");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (I.fn.CreateFence(I.device, &fci, nullptr, &I.fence) != VK_SUCCESS)
        return fail("vkCreateFence failed");

    // Host-visible, persistently mapped uniform + vertex storage. Offscreen
    // rendering of a scene is far too small to justify staging copies.
    struct BufSpec {
        Impl::Buffer* b;
        VkDeviceSize bytes;
        VkBufferUsageFlags usage;
    };
    const BufSpec specs[] = {
        {&I.frameUbo, sizeof(FrameUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT},
        {&I.lightsUbo, sizeof(LightsUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT},
        {&I.shadowUbo, sizeof(ShadowUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT},
        {&I.drawUbo, 65536ull * 8, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT},
        {&I.lineVerts, 256 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT},
        {&I.readback, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT},
    };
    for (const BufSpec& s : specs) {
        if (!I.CreateBuffer(s.bytes, s.usage, s.b)) return fail("host buffer allocation failed");
    }

    // Samplers: mipmapped repeat (world meshes), mipmapped clamp (filtered 2D),
    // a mip-less nearest/clamp for pixel-perfect 2D, and a point sampler for the
    // shadow cascades (the shader does its own 3x3 PCF over individual texels).
    auto makeSampler = [&](VkFilter filter, VkSamplerAddressMode mode, float maxLod) {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = filter;
        ci.minFilter = filter;
        ci.mipmapMode = filter == VK_FILTER_NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                     : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.addressModeU = mode;
        ci.addressModeV = mode;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod = maxLod;
        VkSampler s = VK_NULL_HANDLE;
        I.fn.CreateSampler(I.device, &ci, nullptr, &s);
        return s;
    };
    I.linearRepeat = makeSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 16.0f);
    I.linearClamp = makeSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 16.0f);
    // maxLod 0 pins pixel-perfect sprites to the base level so a mip chain can
    // never blur AI-drawn pixel art; the software rasterizer does the same.
    I.pixelClamp = makeSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f);
    I.nearestClamp = makeSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 16.0f);
    if (!I.linearRepeat || !I.linearClamp || !I.pixelClamp || !I.nearestClamp)
        return fail("sampler creation failed");

    // 1x1 white stand-in for materials with no albedo / normal map bound.
    {
        const uint8_t white[4] = {255, 255, 255, 255};
        Impl::Buffer staging;
        if (!I.CreateBuffer(sizeof(white), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging))
            return fail("texture staging failed");
        std::memcpy(staging.mapped, white, sizeof(white));
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {1, 1, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        I.fn.CreateImage(I.device, &ci, nullptr, &I.whiteTex.handle);
        VkMemoryRequirements req{};
        I.fn.GetImageMemoryRequirements(I.device, I.whiteTex.handle, &req);
        const uint32_t type = I.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type != UINT32_MAX
                                 ? type
                                 : I.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
        I.fn.AllocateMemory(I.device, &ai, nullptr, &I.whiteTex.memory);
        I.fn.BindImageMemory(I.device, I.whiteTex.handle, I.whiteTex.memory, 0);
        VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbi.commandPool = I.pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        VkCommandBuffer one = VK_NULL_HANDLE;
        I.fn.AllocateCommandBuffers(I.device, &cbi, &one);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        I.fn.BeginCommandBuffer(one, &bi);
        I.ImageBarrier(one, I.whiteTex.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                     0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {1, 1, 1};
        I.fn.CmdCopyBufferToImage(one, staging.handle, I.whiteTex.handle,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        I.ImageBarrier(one, I.whiteTex.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        I.fn.EndCommandBuffer(one);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &one;
        I.fn.QueueSubmit(I.queue, 1, &si, VK_NULL_HANDLE);
        I.fn.QueueWaitIdle(I.queue);
        I.whiteTex.view = VK_NULL_HANDLE;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = I.whiteTex.handle;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        I.fn.CreateImageView(I.device, &vi, nullptr, &I.whiteTex.view);
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        I.fn.CreateImageView(I.device, &vi, nullptr, &I.whiteArrayView);
        I.whiteTex.w = I.whiteTex.h = I.whiteTex.layers = 1;
        I.DestroyBuffer(&staging);
        I.fn.FreeCommandBuffers(I.device, I.pool, 1, &one);
    }

    // Descriptor layouts. set 0 is frame state plus the dynamic DrawUBO; set 1
    // is the bindless texture arrays the scene path samples. The post chain
    // still uses the two-sampler texLayout as its own set 0.
    {
        VkDescriptorSetLayoutBinding binds[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            binds[i].binding = i;
            binds[i].descriptorType = (i == 3)   ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                     : (i == 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binds[i].descriptorCount = 1;
            binds[i].stageFlags = (i <= 0 || i == 4) ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                                                      : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 5;
        ci.pBindings = binds;
        if (I.fn.CreateDescriptorSetLayout(I.device, &ci, nullptr, &I.frameLayout) != VK_SUCCESS)
            return fail("descriptor layout failed");
        VkDescriptorSetLayoutBinding tb[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            tb[i].binding = i;
            tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tb[i].descriptorCount = 1;
            tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        ci.bindingCount = 2;
        ci.pBindings = tb;
        if (I.fn.CreateDescriptorSetLayout(I.device, &ci, nullptr, &I.texLayout) != VK_SUCCESS)
            return fail("descriptor layout failed");
        // Bindless arrays: one slot per texture the frame samples, addressed by
        // a per-instance index so a batched run can span several materials.
        for (uint32_t i = 0; i < 2; ++i) {
            tb[i].binding = i;
            tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tb[i].descriptorCount = Impl::kBindlessSlots;
            tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        if (I.fn.CreateDescriptorSetLayout(I.device, &ci, nullptr, &I.bindlessLayout) != VK_SUCCESS)
            return fail("descriptor layout failed");
    }
    {
        constexpr uint32_t kChunks = 8;   // 8 * 256 = 2048 draws per frame
        VkDescriptorPoolSize sizes[3]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = 4 * kChunks;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        sizes[1].descriptorCount = 4 * kChunks;
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        // The two bindless arrays (written white once, then per texture), the
        // shadow sampler of every frame set, and the post sets.
        sizes[2].descriptorCount =
            (uint32_t)(2 * Impl::kBindlessSlots + kChunks + 2 * Impl::kPostSlots);
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        // Frame-set windows, the bindless set and the post sets.
        ci.maxSets = (uint32_t)(kChunks + 1 + Impl::kPostSlots);
        ci.poolSizeCount = 3;
        ci.pPoolSizes = sizes;
        if (I.fn.CreateDescriptorPool(I.device, &ci, nullptr, &I.descPool) != VK_SUCCESS)
            return fail("descriptor pool failed");

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = I.descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &I.frameLayout;
        I.frameSets.assign(kChunks, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < kChunks; ++i)
            I.fn.AllocateDescriptorSets(I.device, &ai, &I.frameSets[i]);
        I.frameSet = I.frameSets[0];

        // The bindless set starts fully white so every slot holds a valid
        // descriptor before any texture is uploaded; slots are then updated as
        // textures are first sampled and when the cache evicts them.
        ai.pSetLayouts = &I.bindlessLayout;
        if (I.fn.AllocateDescriptorSets(I.device, &ai, &I.bindlessSet) != VK_SUCCESS)
            return fail("bindless descriptor set failed");
        {
            VkDescriptorImageInfo white{};
            white.sampler = I.linearClamp;
            white.imageView = I.whiteTex.view;
            white.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            std::vector<VkDescriptorImageInfo> whites(Impl::kBindlessSlots, white);
            VkWriteDescriptorSet w[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = I.bindlessSet;
                w[i].dstBinding = i;
                w[i].descriptorCount = Impl::kBindlessSlots;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = whites.data();
            }
            I.fn.UpdateDescriptorSets(I.device, 2, w, 0, nullptr);
        }

        // Post-chain sets, bound directly as set 0 of postPipeLayout and
        // rewritten per pass.
        I.postSets.assign(Impl::kPostSlots, VK_NULL_HANDLE);
        std::vector<VkDescriptorSetLayout> postLayouts(Impl::kPostSlots, I.texLayout);
        ai.pSetLayouts = postLayouts.data();
        ai.descriptorSetCount = (uint32_t)Impl::kPostSlots;
        if (I.fn.AllocateDescriptorSets(I.device, &ai, I.postSets.data()) != VK_SUCCESS)
            return fail("post descriptor sets failed");
    }
    {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pc.offset = 0;
        pc.size = 64;  // the shadow stage's lightViewProj
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2;
        ci.pSetLayouts = nullptr;  // filled below
        VkDescriptorSetLayout layouts[2] = {I.frameLayout, I.bindlessLayout};
        ci.pSetLayouts = layouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        if (I.fn.CreatePipelineLayout(I.device, &ci, nullptr, &I.scenePipeLayout) != VK_SUCCESS)
            return fail("pipeline layout failed");
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.size = 32;  // two vec4 for the post chain
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &I.texLayout;
        if (I.fn.CreatePipelineLayout(I.device, &ci, nullptr, &I.postPipeLayout) != VK_SUCCESS)
            return fail("post pipeline layout failed");
    }
    {
        Impl::Pipelines first;
        if (!I.CreatePipelines(VK_SAMPLE_COUNT_1_BIT, &first)) return fail("pipeline creation failed");
        I.pipelineCache.emplace(VK_SAMPLE_COUNT_1_BIT, first);
    }
    return true;
}

// Material cull mode (0 none, 1 back, 2 front) as a Vulkan value.
static VkCullModeFlags CullModeFor(int mode) {
    return mode == 1 ? VK_CULL_MODE_BACK_BIT : mode == 2 ? VK_CULL_MODE_FRONT_BIT
                                                          : (VkCullModeFlags)0;
}

// ---------------------------------------------------------------------------

bool VulkanBackend::Impl::CreatePipelines(uint32_t sampleCount, Pipelines* out) {
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    // A pipeline's create-info borrows the module/vertex-state structs below, so
    // all eleven go out in ONE vkCreateGraphicsPipelines call. That is both the
    // faster path and the one drivers are best tested on.
    struct Pending {
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        VkPipelineShaderStageCreateInfo stages[2]{};
        VkPipelineVertexInputStateCreateInfo vertexInput{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState blendAttachment{VK_FALSE};
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkPipelineDepthStencilStateCreateInfo depthStencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkDynamicState dynamicStates[3]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                         VK_DYNAMIC_STATE_CULL_MODE};
        VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        const char* name = "";
    };
    std::vector<Pending*> pending;

    auto add = [&](const char* name, VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout,
                   uint32_t colorAttachments, const VkPipelineVertexInputStateCreateInfo* vertexInput,
                   VkPrimitiveTopology topology, VkPipelineColorBlendAttachmentState blend,
                   VkPipelineDepthStencilStateCreateInfo depth, bool dynamicCull) {
        if (!vs || !fs) return;
        Pending* p = new Pending();
        p->name = name;
        p->stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        p->stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        p->stages[0].module = vs;
        p->stages[0].pName = "main";
        p->stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        p->stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        p->stages[1].module = fs;
        p->stages[1].pName = "main";
        if (vertexInput) p->vertexInput = *vertexInput;
        p->assembly.topology = topology;
        p->assembly.primitiveRestartEnable = VK_FALSE;
        p->viewport.viewportCount = 1;
        p->viewport.scissorCount = 1;
        p->raster.polygonMode = VK_POLYGON_MODE_FILL;
        p->raster.cullMode = dynamicCull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        // Every vertex stage negates clip-space y so +y stays up in a y-down
        // framebuffer. That reversal flips the winding Vulkan sees, so an
        // engine-side counter-clockwise front face is counter-clockwise here
        // too; declaring CLOCKWISE culls the near faces and renders meshes
        // inside-out (a cube shows its interior).
        p->raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        p->raster.lineWidth = 1.0f;
        p->multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(sampleCount);
        p->blendAttachment = blend;
        p->blend.attachmentCount = colorAttachments;
        p->blend.pAttachments = colorAttachments ? &p->blendAttachment : nullptr;
        p->depthStencil = depth;
        p->depthStencil.maxDepthBounds = 1.0f;
        p->dynamic.dynamicStateCount = dynamicCull ? 3 : 2;
        p->dynamic.pDynamicStates = p->dynamicStates;
        p->rendering.viewMask = 0;  // non-multiview rendering: one view
        p->rendering.colorAttachmentCount = colorAttachments;
        p->rendering.pColorAttachmentFormats = colorAttachments ? &p->colorFormat : nullptr;
        // The main pass always binds a depth attachment, so every scene
        // pipeline must declare D32_SFLOAT even when it never tests it;
        // post passes run in a depthless rendering instance.
        p->rendering.depthAttachmentFormat =
            layout == postPipeLayout ? VK_FORMAT_UNDEFINED : depthFormat;
        p->info.pNext = &p->rendering;
        p->info.stageCount = 2;
        p->info.pStages = p->stages;
        p->info.pVertexInputState = &p->vertexInput;
        p->info.pInputAssemblyState = &p->assembly;
        p->info.pViewportState = &p->viewport;
        p->info.pRasterizationState = &p->raster;
        p->info.pMultisampleState = &p->multisample;
        p->info.pDepthStencilState = &p->depthStencil;
        p->info.pColorBlendState = &p->blend;
        // Never NULL: a pipeline with dynamic viewport/scissor but no dynamic
        // state at all deadlocks the NVIDIA driver instead of failing cleanly.
        p->info.pDynamicState = &p->dynamic;
        p->info.layout = layout;
        p->info.renderPass = VK_NULL_HANDLE;   // dynamic rendering
        p->info.subpass = 0;
        pending.push_back(p);
    };

    // Vertex layouts. MeshVertex is {vec3 pos, vec3 nrm, vec2 uv, vec4 tan} =
    // 48 bytes tightly packed, which matches the four locations in mesh.vert.
    VkVertexInputBindingDescription meshBinding{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription meshAttrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
    };
    VkPipelineVertexInputStateCreateInfo meshInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    meshInput.vertexBindingDescriptionCount = 1;
    meshInput.pVertexBindingDescriptions = &meshBinding;
    meshInput.vertexAttributeDescriptionCount = 4;
    meshInput.pVertexAttributeDescriptions = meshAttrs;

    VkVertexInputBindingDescription spriteBinding{0, sizeof(Vertex2D), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription spriteAttrs[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
    };
    VkPipelineVertexInputStateCreateInfo spriteInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    spriteInput.vertexBindingDescriptionCount = 1;
    spriteInput.pVertexBindingDescriptions = &spriteBinding;
    spriteInput.vertexAttributeDescriptionCount = 4;
    spriteInput.pVertexAttributeDescriptions = spriteAttrs;

    VkVertexInputBindingDescription lineBinding{0, sizeof(LineVert), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription lineAttrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},
    };
    VkPipelineVertexInputStateCreateInfo lineInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    lineInput.vertexBindingDescriptionCount = 1;
    lineInput.pVertexBindingDescriptions = &lineBinding;
    lineInput.vertexAttributeDescriptionCount = 2;
    lineInput.pVertexAttributeDescriptions = lineAttrs;

    // Fullscreen passes draw from gl_VertexIndex and need no attributes.
    VkPipelineVertexInputStateCreateInfo emptyInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    // colorWriteMask is NOT defaulted by the API: a zero-initialised
    // attachment state writes no colour components at all, which renders a
    // perfectly valid pipeline completely invisible.
    constexpr VkColorComponentFlags kWriteRGBA =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    const VkPipelineColorBlendAttachmentState noBlend{
        VK_FALSE,      VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, kWriteRGBA};
    // Sprites and text blend in display space, exactly like the software path:
    // the shader writes the encoded source color and the hardware applies
    // SRC_ALPHA / ONE_MINUS_SRC_ALPHA. Alpha itself keeps the destination.
    const VkPipelineColorBlendAttachmentState alphaBlend{
        VK_TRUE,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, kWriteRGBA};
    auto depth = [&](bool test, bool write, VkCompareOp op) {
        VkPipelineDepthStencilStateCreateInfo d{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        d.depthTestEnable = test ? VK_TRUE : VK_FALSE;
        d.depthWriteEnable = write ? VK_TRUE : VK_FALSE;
        d.depthCompareOp = op;
        return d;
    };
    const VkPipelineDepthStencilStateCreateInfo kDepthOn = depth(true, true, VK_COMPARE_OP_LESS);
    const VkPipelineDepthStencilStateCreateInfo kDepthTest = depth(true, false, VK_COMPARE_OP_LESS);
    const VkPipelineDepthStencilStateCreateInfo kDepthOff = depth(false, false, VK_COMPARE_OP_ALWAYS);
    const VkPipelineDepthStencilStateCreateInfo kLineDepth =
        depth(true, false, VK_COMPARE_OP_LESS_OR_EQUAL);

    // Modules stay alive for the whole batch: a pipeline keeps its modules
    // internally, but handing a later pipeline an already-destroyed handle is a
    // use-after-free that surfaces as a driver crash.
    VkShaderModule meshV = LoadShader("mesh.vert");
    VkShaderModule meshF = LoadShader("mesh.frag");
    VkShaderModule spriteV = LoadShader("sprite.vert");
    VkShaderModule spriteF = LoadShader("sprite.frag");
    VkShaderModule skyV = LoadShader("sky.vert");
    VkShaderModule skyF = LoadShader("sky.frag");
    VkShaderModule lineV = LoadShader("line.vert");
    VkShaderModule lineF = LoadShader("line.frag");
    VkShaderModule shadowV = LoadShader("shadow.vert");
    VkShaderModule shadowF = LoadShader("shadow.frag");
    VkShaderModule brightF = LoadShader("bright.frag");
    VkShaderModule blurF = LoadShader("blur.frag");
    VkShaderModule compositeF = LoadShader("composite.frag");
    VkShaderModule gradingF = LoadShader("grading.frag");
    VkShaderModule vignetteF = LoadShader("vignette.frag");
    VkShaderModule fxaaF = LoadShader("fxaa.frag");

    add("mesh", meshV, meshF, scenePipeLayout, 1, &meshInput, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        noBlend, kDepthOn, true);
    add("sprite", spriteV, spriteF, scenePipeLayout, 1, &spriteInput,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, alphaBlend, kDepthTest, false);
    add("sky", skyV, skyF, scenePipeLayout, 1, &emptyInput, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        noBlend, kDepthOff, false);
    add("line", lineV, lineF, scenePipeLayout, 1, &lineInput, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        noBlend, kLineDepth, false);
    add("shadow", shadowV, shadowF, scenePipeLayout, 0, &meshInput,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, noBlend, kDepthOn, true);
    // The post chain shares sky.vert (a fullscreen triangle) and reads through
    // set 0 of the texture layout, rewritten between passes.
    add("bright", skyV, brightF, postPipeLayout, 1, &emptyInput,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, noBlend, kDepthOff, false);
    add("blur", skyV, blurF, postPipeLayout, 1, &emptyInput, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        noBlend, kDepthOff, false);
    add("composite", skyV, compositeF, postPipeLayout, 1, &emptyInput,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, noBlend, kDepthOff, false);
    add("grading", skyV, gradingF, postPipeLayout, 1, &emptyInput,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, noBlend, kDepthOff, false);
    add("vignette", skyV, vignetteF, postPipeLayout, 1, &emptyInput,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, noBlend, kDepthOff, false);
    add("fxaa", skyV, fxaaF, postPipeLayout, 1, &emptyInput, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        noBlend, kDepthOff, false);

    std::vector<VkGraphicsPipelineCreateInfo> infos;
    std::vector<VkPipeline> pipelines(pending.size(), VK_NULL_HANDLE);
    for (Pending* p : pending) infos.push_back(p->info);
    VkResult result = VK_SUCCESS;
    if (!infos.empty())
        result = fn.CreateGraphicsPipelines(device, VK_NULL_HANDLE, (uint32_t)infos.size(),
                                            infos.data(), nullptr, pipelines.data());
    const bool ok = result == VK_SUCCESS;
    if (ok) {
        size_t i = 0;
        for (Pending* p : pending) {
            VkPipeline made = pipelines[i++];
            const std::string name = p->name;
            if (name == "mesh") out->mesh = made;
            else if (name == "sprite") out->sprite = made;
            else if (name == "sky") out->sky = made;
            else if (name == "line") out->line = made;
            else if (name == "shadow") out->shadow = made;
            else if (name == "bright") out->bright = made;
            else if (name == "blur") out->blur = made;
            else if (name == "composite") out->composite = made;
            else if (name == "grading") out->grading = made;
            else if (name == "vignette") out->vignette = made;
            else if (name == "fxaa") out->fxaa = made;
        }
    }
    for (Pending* p : pending) delete p;
    for (VkShaderModule mod : {meshV, meshF, spriteV, spriteF, skyV, skyF, lineV, lineF, shadowV,
                               shadowF, brightF, blurF, compositeF, gradingF, vignetteF, fxaaF})
        if (mod) fn.DestroyShaderModule(device, mod, nullptr);
    return ok && out->mesh && out->sprite && out->sky && out->line && out->shadow && out->bright &&
           out->blur && out->composite && out->grading && out->vignette && out->fxaa;
}

// ---------------------------------------------------------------------------
// Frame recording.
// ---------------------------------------------------------------------------

void VulkanBackend::RenderFrame(const FrameDrawList& list, FrameTarget* target, RenderStats* stats,
                                int requestedSamples) {
    Impl& I = *impl_;
    if (!I.device || !I.cmd || !list.cam || !target) return;
    using Buffer = Impl::Buffer;
    using Image = Impl::Image;
    using Pipelines = Impl::Pipelines;
    using TexEntry = Impl::TexEntry;
    vk::Funcs& fn = I.fn;
    VkDevice device = I.device;
    VkCommandPool pool = I.pool;
    VkCommandBuffer cmd = I.cmd;
    const CameraView& cam = *list.cam;
    const int w = target->width, h = target->height;
    if (w <= 0 || h <= 0 || list.width <= 0 || list.height <= 0) return;

    // ---- sample count ------------------------------------------------------
    uint32_t want = (uint32_t)std::clamp(requestedSamples, 1, 8);
    uint32_t chosen = VK_SAMPLE_COUNT_1_BIT;
    for (VkSampleCountFlagBits s : kSampleLadder)
        if (s <= want && (I.colorSamples & s) && (I.depthSamples & s)) chosen = s;
    auto pipelines = I.pipelineCache.find(chosen);
    if (pipelines == I.pipelineCache.end()) {
        Impl::Pipelines fresh;
        if (!I.CreatePipelines(chosen, &fresh)) {
            if (stats) stats->backend = "software";
            return;
        }
        pipelines = I.pipelineCache.emplace(chosen, fresh).first;
    }

    // ---- render targets ----------------------------------------------------
    const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
    const bool resizeTargets = I.width != w || I.height != h || I.samples != chosen;
    if (resizeTargets) {
        I.samples = chosen;
        I.width = w;
        I.height = h;
        for (Impl::Image* img : {&I.colorMS, &I.colorResolve, &I.depthMS, &I.depthResolve, &I.postA, &I.postB,
                           &I.bloomA, &I.bloomB})
            I.DestroyImage(img);
        const VkImageUsageFlags colorUse = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (chosen > 1 && !I.CreateImage(w, h, 1, VK_FORMAT_R8G8B8A8_UNORM, colorUse, &I.colorMS))
            return;
        const int colorLayers = chosen > 1 ? 1 : 1;
        if (!I.CreateImage(w, h, colorLayers, VK_FORMAT_R8G8B8A8_UNORM, colorUse, &I.colorResolve))
            return;
        const VkImageUsageFlags depthUse = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (chosen > 1 && !I.CreateImage(w, h, 1, VK_FORMAT_D32_SFLOAT, depthUse, &I.depthMS))
            return;
        if (!I.CreateImage(w, h, 1, VK_FORMAT_D32_SFLOAT, depthUse, &I.depthResolve))
            return;
        for (Impl::Image* img : {&I.postA, &I.postB})
            if (!I.CreateImage(w, h, 1, VK_FORMAT_R8G8B8A8_UNORM, colorUse, img)) return;
        for (Impl::Image* img : {&I.bloomA, &I.bloomB})
            if (!I.CreateImage(hw, hh, 1, VK_FORMAT_R8G8B8A8_UNORM, colorUse, img)) return;
    }
    // The staging buffer has to hold the color copy plus the depth readback.
    I.EnsureBuffer((VkDeviceSize)w * h * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &I.readback);
    // The single-sample color target the frame is read back from.
    Impl::Image& sceneColor = (chosen > 1) ? I.colorResolve : I.colorResolve;
    Impl::Image& colorTarget = (chosen > 1) ? I.colorMS : I.colorResolve;
    Impl::Image& depthTarget = (chosen > 1) ? I.depthMS : I.depthResolve;
    Impl::Image& depthRead = I.depthResolve;

    // ---- shadow cascades ---------------------------------------------------
    int cascades = std::clamp(list.cascadeCount, 0, kMaxCascades);
    const int cascadeSize = std::clamp(list.cascadeSize, 256, 4096);
    if (cascades > 0) {
        const bool need = I.shadowMap.handle == VK_NULL_HANDLE || I.shadowMap.w != cascadeSize ||
                          I.shadowMap.layers != cascades;
        if (need) {
            I.DestroyImage(&I.shadowMap);
            if (!I.CreateImage(cascadeSize, cascadeSize, cascades, VK_FORMAT_D32_SFLOAT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                               &I.shadowMap))
                cascades = 0;
        }
    }
    const bool shadows = cascades > 0 && I.shadowMap.handle != VK_NULL_HANDLE;

    // ---- mesh + texture residency -----------------------------------------
    // Total geometry the frame references. Growing the buffers invalidates
    // every cached offset, so the decision has to look at the whole list, not
    // just the meshes that are new.
    size_t vertexBytes = 0, indexCount = 0;
    for (const auto& d : list.opaque) {
        if (!d.mesh) continue;
        vertexBytes += d.mesh->vertices.size() * sizeof(MeshVertex);
        indexCount += d.mesh->indices.size();
    }
    for (const auto& c : list.casters) {
        if (!c.mesh) continue;
        vertexBytes += c.mesh->vertices.size() * sizeof(MeshVertex);
        indexCount += c.mesh->indices.size();
    }
    bool rebuildGeometry = false;
    (void)rebuildGeometry;
    // Growing invalidates every cached offset, so the next upload pass walks
    // the whole draw list again.
    if (!I.sceneVerts.handle ||
        vertexBytes > I.sceneVertexCapacity || indexCount * 4 > I.sceneIndexCapacity) {
        const size_t cap = std::max<size_t>(1 << 18, vertexBytes * 2);
        const size_t icap = std::max<size_t>(1 << 18, indexCount * 2 * 4);
        I.meshes.clear();
        I.sceneBuffersDirty = true;
        if (!I.EnsureBuffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &I.sceneVerts) ||
            !I.EnsureBuffer(icap, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &I.sceneIndices))
            return;
        I.sceneVertexCapacity = cap;
        I.sceneIndexCapacity = icap;
    }
    if (I.sceneBuffersDirty) {
        auto* vb = static_cast<uint8_t*>(I.sceneVerts.mapped);
        auto* ib = static_cast<uint32_t*>(I.sceneIndices.mapped);
        size_t vCursor = 0, iCursor = 0;
        auto place = [&](const std::shared_ptr<Mesh>& mesh) {
            if (!mesh || mesh->vertices.empty() || mesh->indices.empty()) return;
            if (vCursor + mesh->vertices.size() * sizeof(MeshVertex) > I.sceneVertexCapacity ||
                (iCursor + mesh->indices.size()) * 4 > I.sceneIndexCapacity)
                return;
            Impl::MeshEntry e;
            e.vertexOffset = static_cast<uint32_t>(vCursor);
            e.indexOffset = static_cast<uint32_t>(iCursor);
            e.indexCount = static_cast<uint32_t>(mesh->indices.size());
            e.vertexCount = static_cast<uint32_t>(mesh->vertices.size());
            std::memcpy(vb + vCursor, mesh->vertices.data(), mesh->vertices.size() * sizeof(MeshVertex));
            std::memcpy(ib + iCursor, mesh->indices.data(), mesh->indices.size() * sizeof(uint32_t));
            vCursor += mesh->vertices.size() * sizeof(MeshVertex);
            iCursor += mesh->indices.size();
            I.meshes[mesh.get()] = e;
        };
        for (const auto& d : list.opaque) place(d.mesh);
        for (const auto& c : list.casters) place(c.mesh);
        I.sceneBuffersDirty = false;
    }

    // ---- bindless texture slots --------------------------------------------
    // Each distinct (texture, sampler) pair gets a stable slot in the fragment
    // stage's sampler arrays. The slot index travels in the draw uniform, so a
    // batched run no longer has to share one albedo / normal pair. Descriptor
    // writes are queued here and applied once the frame's uploads have created
    // the views.
    struct BindlessUpdate {
        uint32_t binding;
        uint32_t slot;
        const Texture* tex;  // null means "white"
        VkSampler sampler;
    };
    std::vector<BindlessUpdate> bindlessUpdates;
    auto bindlessSlot = [&](uint32_t binding, const Texture* tex, VkSampler sampler) -> uint32_t {
        if (!tex) return 0;  // slot 0 stays white on both bindings
        Impl::BindlessTable& table = I.bindless[binding];
        const auto key = std::make_pair(tex, sampler);
        auto it = table.index.find(key);
        if (it != table.index.end()) {
            table.slotAge[it->second] = I.frameIndex;
            return it->second;
        }
        uint32_t slot;
        if (!table.freeSlots.empty()) {
            slot = table.freeSlots.back();
            table.freeSlots.pop_back();
        } else if (table.next < Impl::kBindlessSlots) {
            slot = table.next++;
        } else {
            // Every slot is taken: steal the least recently used one. A texture
            // the frame already drew has today's age, so the victim is normally
            // one this frame does not reference and can be rebound. Only a frame
            // that alone needs more than the whole table has no better option.
            slot = 0;
            uint64_t oldest = UINT64_MAX;
            for (uint32_t s = 1; s < Impl::kBindlessSlots; ++s) {
                if (!table.slotTex[s]) continue;  // free or never allocated
                if (table.slotAge[s] < oldest) {
                    oldest = table.slotAge[s];
                    slot = s;
                }
            }
            if (slot == 0) return 0;  // nothing to steal: fall back to white
            table.index.erase(std::make_pair(table.slotTex[slot], table.slotSampler[slot]));
        }
        table.index.emplace(key, slot);
        table.slotTex[slot] = tex;
        table.slotSampler[slot] = sampler;
        table.slotAge[slot] = I.frameIndex;
        bindlessUpdates.push_back({binding, slot, tex, sampler});
        return slot;
    };
    // Eviction destroys a texture's image, so its slots go back to white before
    // the next frame binds the set (and are then reusable).
    auto releaseBindless = [&](const Texture* tex) {
        for (uint32_t binding = 0; binding < 2; ++binding) {
            Impl::BindlessTable& table = I.bindless[binding];
            for (auto it = table.index.begin(); it != table.index.end();) {
                if (it->first.first == tex) {
                    const uint32_t slot = it->second;
                    bindlessUpdates.push_back({binding, slot, nullptr, I.linearClamp});
                    table.freeSlots.push_back(slot);
                    table.slotTex[slot] = nullptr;
                    table.slotSampler[slot] = VK_NULL_HANDLE;
                    table.slotAge[slot] = 0;
                    it = table.index.erase(it);
                } else {
                    ++it;
                }
            }
        }
    };

    // Textures: upload the first time a frame references one. Entries that stop
    // being referenced are dropped so a long session does not pin every asset
    // it ever drew.
    ++I.frameIndex;
    std::vector<const Texture*> pendingUploads;
    auto noteTexture = [&](const Texture* tex) {
        if (!tex) return;
        auto it = I.textures.find(tex);
        if (it != I.textures.end()) {
            it->second.lastFrame = I.frameIndex;
            return;
        }
        // A texture can be reached twice in one frame (a material that uses the
        // same image as albedo and normal map). Uploading it twice would drop
        // the first image on the floor without freeing it.
        if (std::find(pendingUploads.begin(), pendingUploads.end(), tex) != pendingUploads.end())
            return;
        pendingUploads.push_back(tex);
    };
    for (const auto& d : list.opaque) {
        noteTexture(d.mat.albedo);
        noteTexture(d.mat.normalMap);
    }
    for (const auto& item : list.items2D) {
        noteTexture(item.mat.albedo);
        noteTexture(item.mat.normalMap);
    }

    // Slots for this frame's draws. Albedo and normal are resolved separately,
    // which is what lets two draws sharing a mesh but not a map still batch.
    std::vector<uint32_t> opaqueAlbedoSlot(list.opaque.size(), 0);
    std::vector<uint32_t> opaqueNormalSlot(list.opaque.size(), 0);
    for (size_t i = 0; i < list.opaque.size(); ++i) {
        opaqueAlbedoSlot[i] = bindlessSlot(0, list.opaque[i].mat.albedo, I.linearRepeat);
        opaqueNormalSlot[i] = bindlessSlot(1, list.opaque[i].mat.normalMap, I.linearRepeat);
    }
    std::vector<uint32_t> item2DAlbedoSlot(list.items2D.size(), 0);
    std::vector<uint32_t> item2DNormalSlot(list.items2D.size(), 0);
    for (size_t i = 0; i < list.items2D.size(); ++i) {
        const MaterialGpu& m = list.items2D[i].mat;
        item2DAlbedoSlot[i] = bindlessSlot(0, m.albedo, m.pixelPerfect ? I.pixelClamp : I.linearRepeat);
        item2DNormalSlot[i] = bindlessSlot(1, m.normalMap, m.pixelPerfect ? I.pixelClamp : I.linearClamp);
    }

    // ---- uniform blocks ----------------------------------------------------
    {
        FrameUbo f{};
        std::memcpy(f.viewProj, list.viewProj.m, sizeof(f.viewProj));
        f.camPosOrtho[0] = list.camPos.x; f.camPosOrtho[1] = list.camPos.y;
        f.camPosOrtho[2] = list.camPos.z; f.camPosOrtho[3] = list.ortho ? 1.0f : 0.0f;
        f.camForward[0] = list.camForward.x; f.camForward[1] = list.camForward.y;
        f.camForward[2] = list.camForward.z;
        f.camRightTH[0] = list.camRight.x; f.camRightTH[1] = list.camRight.y;
        f.camRightTH[2] = list.camRight.z; f.camRightTH[3] = list.tanHalfFov;
        f.camUp[0] = list.camUp.x; f.camUp[1] = list.camUp.y; f.camUp[2] = list.camUp.z;
        // Ambient is used raw on both backends (it is authored, not decoded).
        f.ambient[0] = cam.ambient.r; f.ambient[1] = cam.ambient.g;
        f.ambient[2] = cam.ambient.b; f.ambient[3] = 1.0f;
        f.fogColorDensity[0] = SrgbToLinearFast(cam.fogColor.r);
        f.fogColorDensity[1] = SrgbToLinearFast(cam.fogColor.g);
        f.fogColorDensity[2] = SrgbToLinearFast(cam.fogColor.b);
        f.fogColorDensity[3] = cam.fogDensity;
        f.misc[0] = cam.fogEnabled ? 1.0f : 0.0f;
        f.misc[1] = (float)(int)cam.tonemap;
        f.misc[2] = cam.exposure;
        f.misc[3] = cam.skyEnabled ? 1.0f : 0.0f;
        f.skyTop[0] = cam.skyTop.r; f.skyTop[1] = cam.skyTop.g;
        f.skyTop[2] = cam.skyTop.b; f.skyTop[3] = 1.0f;
        f.skyBottom[0] = cam.skyBottom.r; f.skyBottom[1] = cam.skyBottom.g;
        f.skyBottom[2] = cam.skyBottom.b; f.skyBottom[3] = 1.0f;
        f.viewport[0] = (float)list.width; f.viewport[1] = (float)list.height;
        f.viewport[2] = list.aspect; f.viewport[3] = list.orthoSize;
        f.clearColor[0] = cam.clearColor.r; f.clearColor[1] = cam.clearColor.g;
        f.clearColor[2] = cam.clearColor.b; f.clearColor[3] = 1.0f;
        std::memcpy(I.frameUbo.mapped, &f, sizeof(f));
    }
    {
        LightsUbo l{};
        const int count = (int)std::min<size_t>(list.lights.size(), kMaxLights);
        for (int i = 0; i < count; ++i) {
            const GpuLight& gl = list.lights[i];
            float* d = l.light[i];
            d[0] = gl.pos.x; d[1] = gl.pos.y; d[2] = gl.pos.z; d[3] = (float)gl.type;
            d[4] = gl.dir.x; d[5] = gl.dir.y; d[6] = gl.dir.z; d[7] = gl.intensity;
            d[8] = gl.color.r; d[9] = gl.color.g; d[10] = gl.color.b; d[11] = gl.range;
            d[12] = gl.cosOuter; d[13] = gl.cosInner; d[14] = 0.0f; d[15] = 0.0f;
        }
        l.count = count;
        std::memcpy(I.lightsUbo.mapped, &l, sizeof(l));
    }
    {
        ShadowUbo s{};
        const int count = shadows ? cascades : 0;
        for (int c = 0; c < count; ++c) {
            std::memcpy(s.matrix[c], list.cascadeMatrix[c].m, sizeof(s.matrix[c]));
            s.splits[c] = list.cascadeSplits[c];
            s.range[c] = list.cascadeRange[c];
            s.texel[c] = list.cascadeTexel[c];
        }
        s.params[0] = (float)count;
        s.params[1] = list.shadowStrength;
        s.params[2] = (float)cascadeSize;
        s.toLight[0] = list.shadowToLight.x;
        s.toLight[1] = list.shadowToLight.y;
        s.toLight[2] = list.shadowToLight.z;
        std::memcpy(I.shadowUbo.mapped, &s, sizeof(s));
    }

    // ---- per-draw uniform staging -----------------------------------------
    const VkDeviceSize stride = std::max<VkDeviceSize>(256, I.uniformAlignment);
    // A dynamic uniform descriptor can only span maxUniformBufferRange, so each
    // frame-set window covers drawUboChunk draws.
    I.drawUboChunk = (uint32_t)std::max<size_t>(1, 65536 / stride);
    const size_t maxDraws = (I.drawUbo.capacity / stride);
    const size_t totalDraws = list.opaque.size() + list.items2D.size() + list.casters.size() + 1;
    if (totalDraws > maxDraws) {
        // Guard rather than corrupt: the pool is sized far above any real scene.
        Log::Warning("Vulkan backend: %zu draws exceeds the uniform pool (%zu); frame truncated",
                     totalDraws, maxDraws);
    }
    const size_t drawBudget = std::min(totalDraws, maxDraws);
    std::vector<DrawUbo> drawData;
    drawData.reserve(drawBudget);
    auto pushDraw = [&](const MaterialGpu& mat, const Mat4& model, uint32_t albedoSlot,
                        uint32_t normalSlot) {
        if (drawData.size() >= drawBudget) return;
        DrawUbo d{};
        std::memcpy(d.model, model.m, sizeof(d.model));
        d.color[0] = mat.color.r; d.color[1] = mat.color.g;
        d.color[2] = mat.color.b; d.color[3] = mat.color.a;
        d.emissive[0] = mat.emissive.r; d.emissive[1] = mat.emissive.g;
        d.emissive[2] = mat.emissive.b; d.emissive[3] = 1.0f;
        d.tilingSpec[0] = mat.tiling.x; d.tilingSpec[1] = mat.tiling.y;
        d.tilingSpec[2] = mat.specular; d.tilingSpec[3] = mat.shininess;
        d.surf[0] = mat.metallic; d.surf[1] = mat.roughness; d.surf[2] = mat.normalStrength;
        d.flags[0] = mat.unlit ? 1.0f : 0.0f;
        d.flags[1] = mat.receiveShadows ? 1.0f : 0.0f;
        d.flags[2] = (mat.albedo && mat.albedo->isSRGB()) ? 1.0f : 0.0f;
        d.flags[3] = mat.normalMap ? 1.0f : 0.0f;
        d.misc[0] = mat.spriteLighting ? 1.0f : 0.0f;
        d.texIndex[0] = (float)albedoSlot;
        d.texIndex[1] = (float)normalSlot;
        drawData.push_back(d);
    };
    const Mat4 kIdentity = Mat4::Identity();
    for (size_t i = 0; i < list.opaque.size(); ++i)
        pushDraw(list.opaque[i].mat, list.opaque[i].model, opaqueAlbedoSlot[i], opaqueNormalSlot[i]);
    const size_t opaqueCount = drawData.size();
    for (size_t i = 0; i < list.items2D.size(); ++i)
        pushDraw(list.items2D[i].mat, kIdentity, item2DAlbedoSlot[i], item2DNormalSlot[i]);
    const size_t item2DCount = drawData.size() - opaqueCount;
    for (const auto& c : list.casters) {
        MaterialGpu mat;
        pushDraw(mat, c.model, 0, 0);
    }
    for (size_t i = 0; i < drawData.size(); ++i)
        std::memcpy(static_cast<uint8_t*>(I.drawUbo.mapped) + i * stride, &drawData[i],
                    sizeof(DrawUbo));

    // ---- 2D + line vertex staging -----------------------------------------
    size_t vert2DBytes = 0;
    for (const auto& item : list.items2D) vert2DBytes += item.vertCount * sizeof(Vertex2D);
    if (!list.verts2D.empty() &&
        !I.EnsureBuffer(std::max<size_t>(vert2DBytes, 4096), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        &I.lineVerts))
        return;
    if (!list.verts2D.empty())
        std::memcpy(I.lineVerts.mapped, list.verts2D.data(), vert2DBytes);

    std::vector<LineVert> lineData;
    lineData.reserve(list.lines.size() * 2);
    for (const DebugLine& l : list.lines) {
        lineData.push_back({{l.a.x, l.a.y, l.a.z}, {l.color.r, l.color.g, l.color.b, 1.0f}});
        lineData.push_back({{l.b.x, l.b.y, l.b.z}, {l.color.r, l.color.g, l.color.b, 1.0f}});
    }
    if (!lineData.empty() &&
        !I.EnsureBuffer(std::max<size_t>(lineData.size() * sizeof(LineVert), 4096),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &I.lineVerts))
        return;

    // ---- record ------------------------------------------------------------
    fn.ResetCommandPool(device, pool, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn.BeginCommandBuffer(cmd, &begin);

    // Texture uploads first, so later passes can sample them. Their staging
    // buffers have to outlive the recording: a command buffer only holds
    // handles, so freeing one here would invalidate a copy that has not run.
    std::vector<Impl::Buffer> frameStaging;
    if (!pendingUploads.empty()) {
        for (const Texture* tex : pendingUploads) {
            // Defensive: never overwrite a live image without freeing it.
            auto stale = I.textureImages.find(tex);
            if (stale != I.textureImages.end()) {
                I.DestroyImage(&stale->second);
                I.textureImages.erase(stale);
            }
            TexEntry entry;
            const int levels = std::max(1, tex->mipLevels());
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = VK_FORMAT_R8G8B8A8_UNORM;
            ci.extent = {static_cast<uint32_t>(tex->width()), static_cast<uint32_t>(tex->height()), 1};
            ci.mipLevels = static_cast<uint32_t>(levels);
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            Impl::Image img;
            if (fn.CreateImage(device, &ci, nullptr, &img.handle) != VK_SUCCESS) continue;
            VkMemoryRequirements req{};
            fn.GetImageMemoryRequirements(device, img.handle, &req);
            uint32_t type = I.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
            if (type == UINT32_MAX) type = I.FindMemory(req.memoryTypeBits, 0, 0);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type;
            if (ai.memoryTypeIndex == UINT32_MAX ||
                fn.AllocateMemory(device, &ai, nullptr, &img.memory) != VK_SUCCESS) {
                I.DestroyImage(&img);
                continue;
            }
            fn.BindImageMemory(device, img.handle, img.memory, 0);

            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img.handle;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(levels), 0, 1};
            fn.CreateImageView(device, &vi, nullptr, &img.view);

            // Byte offset of each mip level in the staging chain.
            std::vector<size_t> levelOffset(levels, 0);
            size_t total = 0;
            for (int l = 0; l < levels; ++l) {
                levelOffset[l] = total;
                total += (size_t)tex->levelWidth(l) * tex->levelHeight(l) * 4;
            }
            Buffer staging;
            if (!I.CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging)) {
                I.DestroyImage(&img);
                continue;
            }
            uint8_t* dst = static_cast<uint8_t*>(staging.mapped);
            std::memcpy(dst, tex->pixels(), (size_t)tex->width() * tex->height() * 4);
            // Texture keeps its chain private; rebuild the coarse levels here
            // with the same 2x2 box the CPU sampler uses, so both backends see
            // identical mip content.
            for (int l = 1; l < levels; ++l) {
                const int sw = tex->levelWidth(l - 1), sh = tex->levelHeight(l - 1);
                const int dw = tex->levelWidth(l), dh = tex->levelHeight(l);
                const uint8_t* src = dst + levelOffset[l - 1];
                uint8_t* out = dst + levelOffset[l];
                for (int y = 0; y < dh; ++y) {
                    for (int x = 0; x < dw; ++x) {
                        int acc[4] = {0, 0, 0, 0};
                        for (int sy = 0; sy < 2; ++sy) {
                            for (int sx = 0; sx < 2; ++sx) {
                                const uint8_t* s =
                                    src + ((size_t)std::min(y * 2 + sy, sh - 1) * sw +
                                           std::min(x * 2 + sx, sw - 1)) * 4;
                                for (int c = 0; c < 4; ++c) acc[c] += s[c];
                            }
                        }
                        uint8_t* d = out + ((size_t)y * dw + x) * 4;
                        for (int c = 0; c < 4; ++c) d[c] = (uint8_t)((acc[c] + 2) >> 2);
                    }
                }
            }
            I.ImageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            std::vector<VkBufferImageCopy> regions;
            for (int l = 0; l < levels; ++l) {
                VkBufferImageCopy region{};
                region.bufferOffset = levelOffset[l];
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(l), 0, 1};
                region.imageExtent = {static_cast<uint32_t>(tex->levelWidth(l)),
                                      static_cast<uint32_t>(tex->levelHeight(l)), 1};
                regions.push_back(region);
            }
            frameStaging.push_back(staging);
            fn.CmdCopyBufferToImage(cmd, staging.handle, img.handle,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    (uint32_t)regions.size(), regions.data());
            I.ImageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            // Freed after the fence, below.

            entry.view = img.view;
            entry.mipLevels = static_cast<uint32_t>(levels);
            entry.lastFrame = I.frameIndex;
            I.textureImages[tex] = std::move(img);
            I.textures[tex] = entry;
        }
        if (I.textures.size() > 512) {
            for (auto it = I.textures.begin(); it != I.textures.end();) {
                if (it->second.lastFrame + 512 < I.frameIndex) {
                    releaseBindless(it->first);
                    I.DestroyImage(&I.textureImages[it->first]);
                    I.textureImages.erase(it->first);
                    it = I.textures.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    // Apply the queued bindless writes: slot assignments made while the draw
    // list was walked, plus the white-out of every evicted texture's slots.
    if (!bindlessUpdates.empty()) {
        std::vector<VkDescriptorImageInfo> infos(bindlessUpdates.size());
        std::vector<VkWriteDescriptorSet> writes(bindlessUpdates.size());
        for (size_t i = 0; i < bindlessUpdates.size(); ++i) {
            const BindlessUpdate& u = bindlessUpdates[i];
            VkImageView view = I.whiteTex.view;
            if (u.tex) {
                auto it = I.textureImages.find(u.tex);
                if (it != I.textureImages.end()) view = it->second.view;
            }
            infos[i].sampler = u.sampler;
            infos[i].imageView = view;
            infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = I.bindlessSet;
            writes[i].dstBinding = u.binding;
            writes[i].dstArrayElement = u.slot;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        fn.UpdateDescriptorSets(device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }

    // ---- frame descriptors -------------------------------------------------
    // One set per DrawUBO window; binding 4 of window i starts at i * 64 KiB and
    // the per-draw dynamic offset is relative to that window.
    const VkDeviceSize kWindow = (VkDeviceSize)I.drawUboChunk * stride;
    for (size_t w = 0; w < I.frameSets.size(); ++w) {
        VkDescriptorBufferInfo infos[5]{};
        infos[0] = {I.frameUbo.handle, 0, sizeof(FrameUbo)};
        infos[1] = {I.lightsUbo.handle, 0, sizeof(LightsUbo)};
        infos[2] = {I.shadowUbo.handle, 0, sizeof(ShadowUbo)};
        infos[3] = {I.drawUbo.handle, (VkDeviceSize)w * kWindow, kWindow};
        infos[4] = {I.drawUbo.handle, (VkDeviceSize)w * kWindow, kWindow};
        // With cascades off the binding still needs a valid image; the 1x1
        // white array stands in (the shader short-circuits on count == 0).
        VkDescriptorImageInfo shadowImage{};
        shadowImage.sampler = I.nearestClamp;
        shadowImage.imageView = shadows ? I.shadowMap.view : I.whiteArrayView;
        shadowImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[5]{};
        const VkDescriptorType types[5] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC};
        for (uint32_t i = 0; i < 5; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = I.frameSets[w];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = types[i];
            if (types[i] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                writes[i].pImageInfo = &shadowImage;
            else
                writes[i].pBufferInfo = &infos[i];
        }
        fn.UpdateDescriptorSets(device, 5, writes, 0, nullptr);
    }
    // Draw `n` lives in window n / drawUboChunk at offset (n % drawUboChunk) * stride.
    // Set 1 is the one bindless table, identical for every draw.
    auto bindFrameSet = [&](VkCommandBuffer cb, uint32_t set) {
        const uint32_t window = set / I.drawUboChunk;
        const VkDescriptorSet target = I.frameSets[window < I.frameSets.size() ? window : 0];
        const uint32_t offset = (set % I.drawUboChunk) * (uint32_t)stride;
        const VkDescriptorSet sets[2] = {target, I.bindlessSet};
        fn.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, I.scenePipeLayout, 0, 2, sets, 1,
                                 &offset);
    };
    // Same, but the dynamic offset points at the run's first draw while the
    // shader reads draws[gl_InstanceIndex] for the rest. Shadow casters use it
    // too: they read no textures, but the layout still declares set 1 and the
    // bindless table is a valid (if unread) binding. The window is taken from
    // the run's first draw — using window 0 unconditionally would overrun the
    // 64 KiB descriptor range once a frame had more than drawUboChunk draws.
    auto bindBatch = [&](VkCommandBuffer cb, uint32_t base) {
        const uint32_t window = base / I.drawUboChunk;
        const VkDescriptorSet target = I.frameSets[window < I.frameSets.size() ? window : 0];
        const uint32_t offset = (base % I.drawUboChunk) * (uint32_t)stride;
        const VkDescriptorSet sets[2] = {target, I.bindlessSet};
        fn.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, I.scenePipeLayout, 0, 2, sets, 1,
                                 &offset);
    };

    // The post chain keeps its own two-sampler set layout and rewrites a set
    // per pass.
    auto writePostSet = [&](uint32_t slot, VkImageView src, VkImageView second) {
        if (slot >= I.postSets.size()) return;
        VkDescriptorImageInfo images[2]{};
        images[0].sampler = I.linearClamp;
        images[0].imageView = src;
        images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        images[1].sampler = I.linearClamp;
        images[1].imageView = second;
        images[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = I.postSets[slot];
        w.dstBinding = 0;
        w.descriptorCount = 2;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = images;
        fn.UpdateDescriptorSets(device, 1, &w, 0, nullptr);
    };

    VkViewport viewport{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    int opaqueBatches = 0;
    int shadowBatches = 0;
    // CAIRN_NO_INSTANCING=1 suppresses run merging so the per-draw path can be
    // diffed against the instanced one; they must be identical.
    static const bool noInstancing = std::getenv("CAIRN_NO_INSTANCING") != nullptr;

    // ---- shadow cascades ---------------------------------------------------
    if (shadows) {
        VkViewport cascadeViewport{0.0f, 0.0f, (float)cascadeSize, (float)cascadeSize, 0.0f, 1.0f};
        VkRect2D cascadeScissor{{0, 0}, {static_cast<uint32_t>(cascadeSize),
                                          static_cast<uint32_t>(cascadeSize)}};
        const uint32_t firstCaster = opaqueCount + item2DCount;
        for (int c = 0; c < cascades; ++c) {
            // The array is left readable by the main pass; move the layer back
            // to a depth attachment for this frame's pass.
            I.Transition(cmd, I.shadowMap, VK_IMAGE_ASPECT_DEPTH_BIT,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            // A single-cascade map is not "layered", so CreateImage leaves
            // layerViews empty and the whole-image view is the layer view.
            depth.imageView = I.shadowMap.layers > 1 ? I.shadowMap.layerViews[c] : I.shadowMap.view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.0f, 0};
            VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
            info.renderArea = {{0, 0}, {static_cast<uint32_t>(cascadeSize),
                                         static_cast<uint32_t>(cascadeSize)}};
            info.layerCount = 1;
        info.viewMask = 0;  // non-multiview rendering: one view
            info.pDepthAttachment = &depth;
            fn.CmdBeginRendering(cmd, &info);
            fn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->second.shadow);
            fn.CmdSetViewport(cmd, 0, 1, &cascadeViewport);
            fn.CmdSetScissor(cmd, 0, 1, &cascadeScissor);
            // The cascade matrix is the same for every caster in this pass, so it
            // is pushed once; the per-caster model comes from draws[].
            fn.CmdPushConstants(cmd, I.scenePipeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64,
                                list.cascadeMatrix[c].m);
            for (size_t i = 0; i < list.casters.size();) {
                const uint32_t slot = (uint32_t)(firstCaster + i);
                if (slot >= drawData.size()) break;
                if (!IntersectsShadowVolume(list.cascadeMatrix[c], list.casters[i].bounds)) {
                    ++i;
                    continue;
                }
                auto meshIt = I.meshes.find(list.casters[i].mesh.get());
                if (meshIt == I.meshes.end()) {
                    ++i;
                    continue;
                }
                const VkCullModeFlags cull = CullModeFor(list.casters[i].cullMode);
                // Extend the run: same mesh and cull mode, still inside the
                // cascade volume, and inside one draw-UBO window (the shader
                // cannot index past its 64 KiB).
                const size_t windowEnd =
                    ((slot / I.drawUboChunk) + 1) * (size_t)I.drawUboChunk - firstCaster;
                size_t j = i + 1;
                if (!noInstancing) {
                    for (; j < list.casters.size() && j < windowEnd; ++j) {
                        const uint32_t s2 = (uint32_t)(firstCaster + j);
                        if (s2 >= drawData.size()) break;
                        if (!IntersectsShadowVolume(list.cascadeMatrix[c], list.casters[j].bounds))
                            break;
                        if (list.casters[j].mesh.get() != list.casters[i].mesh.get()) break;
                        if (CullModeFor(list.casters[j].cullMode) != cull) break;
                    }
                }
                bindBatch(cmd, slot);
                const VkDeviceSize vOffset = meshIt->second.vertexOffset;
                fn.CmdBindVertexBuffers(cmd, 0, 1, &I.sceneVerts.handle, &vOffset);
                fn.CmdBindIndexBuffer(cmd, I.sceneIndices.handle,
                                       (VkDeviceSize)meshIt->second.indexOffset * 4,
                                       VK_INDEX_TYPE_UINT32);
                // The shadow pipeline declares CULL_MODE dynamic, so it has to
                // be set before the first draw or the draw is invalid.
                fn.CmdSetCullMode(cmd, cull);
                fn.CmdDrawIndexed(cmd, meshIt->second.indexCount, (uint32_t)(j - i), 0, 0, 0);
                ++shadowBatches;
                i = j;
            }
            fn.CmdEndRendering(cmd);
            I.Transition(cmd, I.shadowMap, VK_IMAGE_ASPECT_DEPTH_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    }

    // ---- main pass ---------------------------------------------------------
    {
        // The multisample images stay in attachment layouts forever; the
        // resolve targets move on to SHADER_READ / TRANSFER_SRC every frame,
        // so each one is transitioned from wherever the last frame ended.
        if (chosen > 1) {
            I.Transition(cmd, I.colorMS, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            I.Transition(cmd, I.depthMS, VK_IMAGE_ASPECT_DEPTH_BIT,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }
        I.Transition(cmd, sceneColor, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        I.Transition(cmd, depthRead, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = colorTarget.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0, 0, 0, 1}};
        VkRenderingAttachmentInfo depthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttach.imageView = depthTarget.view;
        depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttach.clearValue.depthStencil = {1.0f, 0};
        // Vulkan 1.3 expresses multisample resolve on the attachment itself.
        if (chosen > 1) {
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = sceneColor.view;
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            depthAttach.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depthAttach.resolveImageView = depthRead.view;
            depthAttach.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        }

        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
        info.layerCount = 1;
        info.viewMask = 0;  // non-multiview rendering: one view
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depthAttach;
        fn.CmdBeginRendering(cmd, &info);
        fn.CmdSetViewport(cmd, 0, 1, &viewport);
        fn.CmdSetScissor(cmd, 0, 1, &scissor);
        const uint32_t zero = 0;

        // Sky: fullscreen triangle, no depth interaction.
        fn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->second.sky);
        {
            const VkDescriptorSet sets[2] = {I.frameSet, I.bindlessSet};
            fn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, I.scenePipeLayout, 0, 2,
                                     sets, 1, &zero);
        }
        fn.CmdDraw(cmd, 3, 1, 0, 0);

        // Opaque meshes, instanced. Culling is dynamic state so one pipeline
        // serves every material; the GL->VK y flip makes front faces clockwise.
        // A run of consecutive draws sharing a mesh and cull mode collapses
        // into one vkCmdDrawIndexed: the dynamic offset points at the run's
        // first draw and the vertex shader reads draws[gl_InstanceIndex]. The
        // textures are addressed per instance through the bindless table, so
        // the run no longer has to share an albedo / normal pair.
        fn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->second.mesh);
        for (size_t i = 0; i < list.opaque.size() && i < opaqueCount;) {
            const FrameDrawList::OpaqueDraw& first = list.opaque[i];
            auto meshIt = I.meshes.find(first.mesh.get());
            if (meshIt == I.meshes.end()) { ++i; continue; }
            const VkCullModeFlags cull = CullModeFor(first.mat.cullMode);
            // Extend the run: same mesh, same cull mode. Never cross a draw-UBO
            // window — the shader cannot index past its 64 KiB.
            const size_t windowEnd = ((i / I.drawUboChunk) + 1) * (size_t)I.drawUboChunk;
            size_t j = i + 1;
            if (!noInstancing) {
                for (; j < list.opaque.size() && j < opaqueCount && j < windowEnd; ++j) {
                    const FrameDrawList::OpaqueDraw& d = list.opaque[j];
                    if (d.mesh.get() != first.mesh.get()) break;
                    if (CullModeFor(d.mat.cullMode) != cull) break;
                }
            }
            bindBatch(cmd, static_cast<uint32_t>(i));
            const VkDeviceSize vOffset = meshIt->second.vertexOffset;
            fn.CmdBindVertexBuffers(cmd, 0, 1, &I.sceneVerts.handle, &vOffset);
            fn.CmdBindIndexBuffer(cmd, I.sceneIndices.handle,
                                   (VkDeviceSize)meshIt->second.indexOffset * 4, VK_INDEX_TYPE_UINT32);
            fn.CmdSetCullMode(cmd, cull);
            fn.CmdDrawIndexed(cmd, meshIt->second.indexCount, (uint32_t)(j - i), 0, 0, 0);
            ++opaqueBatches;
            i = j;
        }

        // Sprites and text: already depth-sorted, alpha blended in display space.
        fn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->second.sprite);
        {
            const VkDeviceSize vBase = 0;
            fn.CmdBindVertexBuffers(cmd, 0, 1, &I.lineVerts.handle, &vBase);
            size_t itemIndex = 0;
            for (size_t i = 0; i < list.items2D.size() && i < item2DCount; ++i) {
                const auto& item = list.items2D[i];
                if (item.vertCount == 0) continue;
                const size_t slot = opaqueCount + itemIndex;
                bindFrameSet(cmd, static_cast<uint32_t>(slot));
                fn.CmdDraw(cmd, item.vertCount, 1, item.firstVert, 0);
                ++itemIndex;
            }
        }

        // Debug / editor lines.
        if (!lineData.empty()) {
            std::memcpy(I.lineVerts.mapped, lineData.data(), lineData.size() * sizeof(LineVert));
            fn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->second.line);
            {
                const VkDescriptorSet sets[2] = {I.frameSet, I.bindlessSet};
                fn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, I.scenePipeLayout, 0,
                                         2, sets, 1, &zero);
            }
            const VkDeviceSize vBase = 0;
            fn.CmdBindVertexBuffers(cmd, 0, 1, &I.lineVerts.handle, &vBase);
            fn.CmdDraw(cmd, (uint32_t)lineData.size(), 1, 0, 0);
        }
        fn.CmdEndRendering(cmd);
    }

    // ---- post chain --------------------------------------------------------
    const float bloomStrength = std::clamp(cam.postBloomStrength, 0.0f, 2.0f);
    const float contrast = std::clamp(cam.postContrast, 0.0f, 2.0f);
    const float saturation = std::clamp(cam.postSaturation, 0.0f, 2.0f);
    const float vignette = std::clamp(cam.postVignette, 0.0f, 1.0f);
    const bool wantBloom = bloomStrength > 1e-4f;
    const bool wantGrading = std::fabs(contrast - 1.0f) > 1e-4f || std::fabs(saturation - 1.0f) > 1e-4f;
    const bool wantVignette = vignette > 1e-4f;
    int postPasses = 0;

    // The color attachment has to leave COLOR_ATTACHMENT_OPTIMAL before any
    // post pass samples it; the depth image goes to GENERAL for readback.
    I.Transition(cmd, sceneColor, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    I.Transition(cmd, depthRead, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);

    // Post passes each rewrite a set, so each gets a slot of its own: reusing
    // one would mean updating a set an earlier pass in the same command buffer
    // is still bound to.
    uint32_t postSlot = 0;
    auto postSetSlot = [&]() {
        return postSlot < I.postSets.size() ? postSlot++ : I.postSets.size() - 1;
    };
    // `dstPrev` is the layout the destination is in right now; the pass moves
    // it to COLOR_ATTACHMENT_OPTIMAL, renders, then leaves it readable for the
    // next pass and for the final copy.
    auto runPost = [&](VkPipeline pipeline, Impl::Image* src, Impl::Image* dst, VkImageView second,
                       const float* push) {
        // Descriptors declare both samplers as SHADER_READ_ONLY_OPTIMAL, so the
        // sources have to be there before the pass, whatever the previous pass
        // left behind.
        I.Transition(cmd, *src, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        I.Transition(cmd, *dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = dst->view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0, 0, 0, 1}};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        // The bloom chain runs at half resolution; renderArea follows the
        // destination, not the framebuffer.
        info.renderArea = {{0, 0}, {static_cast<uint32_t>(dst->w), static_cast<uint32_t>(dst->h)}};
        info.layerCount = 1;
        info.viewMask = 0;  // non-multiview rendering: one view
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        fn.CmdBeginRendering(cmd, &info);
        fn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const uint32_t slot = postSetSlot();
        writePostSet(slot, src->view, second);
        fn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, I.postPipeLayout, 0, 1,
                                 &I.postSets[slot], 0, nullptr);
        fn.CmdPushConstants(cmd, I.postPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, push);
        fn.CmdDraw(cmd, 3, 1, 0, 0);
        fn.CmdEndRendering(cmd);
        I.Transition(cmd, *dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };

    // Every destination leaves runPost() in SHADER_READ_ONLY_OPTIMAL, so each
    // pass after the first reads its source from that layout.
    Impl::Image* current = &sceneColor;
    Impl::Image* scratchPost = &I.postA;
    if (wantBloom) {
        const float pushBright[8] = {std::clamp(cam.postBloomThreshold, 0.0f, 1.0f), 0, 0, 0,
                                     (float)w, (float)h, 0, 0};
        runPost(pipelines->second.bright, current, &I.bloomA, I.whiteTex.view, pushBright);
        const float pushH[8] = {1.0f / (float)hw, 0, 0, 0, 0, 0, 0, 0};
        runPost(pipelines->second.blur, &I.bloomA, &I.bloomB, I.whiteTex.view, pushH);
        const float pushV[8] = {0, 1.0f / (float)hh, 0, 0, 0, 0, 0, 0};
        runPost(pipelines->second.blur, &I.bloomB, &I.bloomA, I.whiteTex.view, pushV);
        const float pushComposite[8] = {bloomStrength, 0, 0, 0, (float)hw, (float)hh, 0, 0};
        runPost(pipelines->second.composite, current, scratchPost, I.bloomA.view, pushComposite);
        current = scratchPost;
        scratchPost = (scratchPost == &I.postA) ? &I.postB : &I.postA;
        ++postPasses;
    }
    if (wantGrading) {
        const float push[8] = {contrast, saturation, 0, 0, 0, 0, 0, 0};
        runPost(pipelines->second.grading, current, scratchPost, I.whiteTex.view, push);
        current = scratchPost;
        scratchPost = (scratchPost == &I.postA) ? &I.postB : &I.postA;
        ++postPasses;
    }
    if (wantVignette) {
        const float push[8] = {vignette, std::clamp(cam.postVignetteRadius, 0.0f, 1.0f),
                               std::clamp(cam.postVignetteSoftness, 0.0f, 1.0f), 0,
                               (float)w / (float)h, (float)w, (float)h, 0};
        runPost(pipelines->second.vignette, current, scratchPost, I.whiteTex.view, push);
        current = scratchPost;
        scratchPost = (scratchPost == &I.postA) ? &I.postB : &I.postA;
        ++postPasses;
    }
    if (cam.postFxaa) {
        const float push[8] = {(float)w, (float)h, 0, 0, 0, 0, 0, 0};
        runPost(pipelines->second.fxaa, current, scratchPost, I.whiteTex.view, push);
        current = scratchPost;
        ++postPasses;
    }

    // ---- readback ----------------------------------------------------------
    {
        const VkDeviceSize colorBytes = (VkDeviceSize)w * h * 4;
        VkBufferImageCopy colorCopy{};
        colorCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        colorCopy.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        // Copies need TRANSFER_SRC_OPTIMAL (or GENERAL); the post chain leaves
        // its result readable for sampling only.
        I.Transition(cmd, *current, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        fn.CmdCopyImageToBuffer(cmd, current->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                I.readback.handle, 1, &colorCopy);
        VkBufferImageCopy depthCopy{};
        depthCopy.bufferOffset = colorBytes;
        depthCopy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        depthCopy.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        fn.CmdCopyImageToBuffer(cmd, depthRead.handle, VK_IMAGE_LAYOUT_GENERAL, I.readback.handle, 1,
                                &depthCopy);
    }
    fn.EndCommandBuffer(cmd);

    fn.ResetFences(device, 1, &I.fence);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &I.cmd;
    if (fn.QueueSubmit(I.queue, 1, &submit, I.fence) != VK_SUCCESS) {
        if (stats) stats->backend = "software";
        return;
    }
    fn.WaitForFences(device, 1, &I.fence, VK_TRUE, UINT64_MAX);
    // The copies that read these staging buffers have now run.
    for (Impl::Buffer& b : frameStaging) I.DestroyBuffer(&b);

    const uint8_t* src = static_cast<const uint8_t*>(I.readback.mapped);
    const size_t colorSize = (size_t)w * h * 4;
    std::memcpy(target->color.data(), src, colorSize);
    if (target->depth.size() >= (size_t)w * h) {
        std::memcpy(target->depth.data(), src + colorSize, (size_t)w * h * sizeof(float));
    }
    target->color.resize(colorSize);
    target->width = w;
    target->height = h;

    if (stats) {
        // Draw calls are API submissions, so instanced runs count once and the
        // shadow cascades are included.
        stats->drawCalls = opaqueBatches + shadowBatches + (int)list.items2D.size() + 1;
        stats->triangles = 0;
        for (const auto& d : list.opaque)
            if (d.mesh) stats->triangles += (int)(d.mesh->indices.size() / 3);
        for (const auto& item : list.items2D) stats->triangles += (int)(item.vertCount / 3);
        stats->lightCount = (int)std::min<size_t>(list.lights.size(), kMaxLights);
        stats->shadowedPixels = 0;  // resolved on the GPU, not counted per pixel
        stats->postPasses = postPasses;
        stats->backend = "vulkan";
    }
}

}  // namespace Cairn

#else  // !CAIRN_HAS_VULKAN

// The engine built without the Vulkan headers/compiler: the backend reports
// itself unavailable and the software rasterizer runs every frame.
namespace Cairn {

bool VulkanBackendAvailable() { return false; }

struct VulkanBackend::Impl {};

VulkanBackend::VulkanBackend() = default;
VulkanBackend::~VulkanBackend() = default;
bool VulkanBackend::Initialize(std::string* error) {
    if (error) *error = "engine built without Vulkan support";
    return false;
}
void VulkanBackend::RenderFrame(const FrameDrawList&, FrameTarget*, RenderStats*, int) {}

}  // namespace Cairn

#endif  // CAIRN_HAS_VULKAN