#include "render/renderer.h"

#include <volk.h>

#include "app/log.h"
#include "render/shaders_embedded.h"
#include "render/skybox.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

namespace render {
namespace {

constexpr int      kFramesInFlight = 2;
constexpr int      kSkyboxSize     = 2048;
constexpr uint32_t kSkyboxSeed     = 0x5eed1234u;

struct VkFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void Check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw VkFailure(std::string(what) + " failed (VkResult " + std::to_string(r) + ")");
}

// ---- camera ---------------------------------------------------------------------------------

struct Vec3 {
    float x, y, z;
};
Vec3  operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3  operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3  operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3  Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3  Normalize(Vec3 a) { return a * (1.0f / std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z)); }
float Radians(float deg) { return deg * 0.017453293f; }

// Must match the push_constant block in shaders/blackhole.frag.
struct PushConstants {
    float camPos[4];    // xyz, w = tan(vertical fov / 2)
    float camRight[4];  // xyz, w = aspect ratio
    float camUp[4];     // xyz, w = animation time in seconds
    float camFwd[4];    // xyz, w = 1 when the target needs sRGB encoding in the shader
};
static_assert(sizeof(PushConstants) == 64);

void Store(float out[4], Vec3 v, float w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

// A slow drift around the hole, a few degrees above the disk plane, so the lensed far side of
// the disk arches over the shadow the way it does in Interstellar. Units are GM/c^2.
PushConstants MakeCamera(double seconds, float aspect, bool manualGamma) {
    const float t    = static_cast<float>(seconds);
    const float az   = 0.6f + t * 0.012f;
    const float inc  = Radians(7.0f + 4.0f * std::sin(t * 0.037f));
    const float dist = 29.0f + 4.0f * std::sin(t * 0.021f);
    const float roll = Radians(-6.0f + 3.0f * std::sin(t * 0.029f));

    const Vec3 pos   = {dist * std::cos(inc) * std::cos(az), dist * std::sin(inc), dist * std::cos(inc) * std::sin(az)};
    const Vec3 fwd   = Normalize(Vec3{0.0f, 0.0f, 0.0f} - pos);
    const Vec3 right = Normalize(Cross(fwd, {0.0f, 1.0f, 0.0f}));
    const Vec3 up    = Cross(right, fwd);

    PushConstants pc{};
    Store(pc.camPos, pos, std::tan(Radians(24.0f)));
    Store(pc.camRight, right * std::cos(roll) + up * std::sin(roll), aspect);
    Store(pc.camUp, up * std::cos(roll) - right * std::sin(roll), static_cast<float>(std::fmod(seconds, 100000.0)));
    Store(pc.camFwd, fwd, manualGamma ? 1.0f : 0.0f);
    return pc;
}

// ---- per-window state -----------------------------------------------------------------------

struct Frame {
    VkCommandBuffer cmd            = VK_NULL_HANDLE;
    VkSemaphore     imageAvailable = VK_NULL_HANDLE;
    VkFence         fence          = VK_NULL_HANDLE;
};

struct Target {
    HWND           hwnd      = nullptr;
    VkSurfaceKHR   surface   = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat       format    = VK_FORMAT_UNDEFINED;
    VkExtent2D     extent{};
    bool           canCapture = false;
    bool           stale      = true;

    std::vector<VkImage>       images;
    std::vector<VkImageView>   views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore>   renderDone;  // one per swapchain image, not per frame in flight

    std::array<Frame, kFramesInFlight> frames{};
    uint32_t                           frameIndex = 0;
};

bool IsSrgb(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB; }

// The SPIR-V blobs are byte arrays with no alignment promise; Vulkan wants uint32_t words.
std::vector<uint32_t> ShaderWords(const char* name) {
    const ShaderBlob* blob = shader_blob(name);
    if (!blob || blob->size % 4 != 0) throw VkFailure(std::string("missing shader ") + name);
    std::vector<uint32_t> words(blob->size / 4);
    std::memcpy(words.data(), blob->data, blob->size);
    return words;
}

bool WriteBmp(const std::string& path, const uint8_t* pixels, uint32_t w, uint32_t h, bool swapRB) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    const uint32_t imageBytes = w * h * 4;
    uint8_t        header[54] = {'B', 'M'};
    auto           put32      = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(2, 54 + imageBytes);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, static_cast<uint32_t>(-static_cast<int32_t>(h)));  // negative height: top-down rows
    header[26] = 1;
    header[28] = 32;
    put32(34, imageBytes);
    std::fwrite(header, 1, sizeof(header), f);

    std::vector<uint8_t> row(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(row.data(), pixels + static_cast<size_t>(y) * w * 4, row.size());
        for (uint32_t x = 0; x < w; ++x) {
            if (swapRB) std::swap(row[x * 4 + 0], row[x * 4 + 2]);
            row[x * 4 + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    return std::fclose(f) == 0;
}

}  // namespace

// ---- the renderer ---------------------------------------------------------------------------

struct Renderer::Impl {
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu      = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    VkQueue          queue    = VK_NULL_HANDLE;
    uint32_t         family   = 0;
    VkCommandPool    pool     = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSet  = VK_NULL_HANDLE;

    VkImage        skyImage  = VK_NULL_HANDLE;
    VkDeviceMemory skyMemory = VK_NULL_HANDLE;
    VkImageView    skyView   = VK_NULL_HANDLE;
    VkSampler      sampler   = VK_NULL_HANDLE;

    // Created with the first window, for its swapchain format; later windows must match it.
    VkRenderPass renderPass       = VK_NULL_HANDLE;
    VkFormat     renderPassFormat = VK_FORMAT_UNDEFINED;
    VkPipeline   pipeline         = VK_NULL_HANDLE;

    std::vector<std::unique_ptr<Target>> targets;

    HWND        captureWindow = nullptr;
    std::string capturePath;

    ~Impl() {
        if (device) {
            vkDeviceWaitIdle(device);
            for (auto& t : targets) DestroyTarget(*t);
            targets.clear();
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyRenderPass(device, renderPass, nullptr);
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            vkDestroySampler(device, sampler, nullptr);
            vkDestroyImageView(device, skyView, nullptr);
            vkDestroyImage(device, skyImage, nullptr);
            vkFreeMemory(device, skyMemory, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    // -- set-up --------------------------------------------------------------------------------

    void Init() {
        Check(volkInitialize(), "loading vulkan-1.dll");

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "The Black Hole";
        appInfo.apiVersion       = VK_API_VERSION_1_1;

        const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};

        // BLACK_HOLE_VALIDATE=1 turns on the Khronos validation layer when it is installed.
        std::vector<const char*> layers;
        if (std::getenv("BLACK_HOLE_VALIDATE")) {
            uint32_t n = 0;
            vkEnumerateInstanceLayerProperties(&n, nullptr);
            std::vector<VkLayerProperties> props(n);
            vkEnumerateInstanceLayerProperties(&n, props.data());
            for (const auto& p : props) {
                if (std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                }
            }
        }

        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo        = &appInfo;
        ici.enabledExtensionCount   = 2;
        ici.ppEnabledExtensionNames = extensions;
        ici.enabledLayerCount       = static_cast<uint32_t>(layers.size());
        ici.ppEnabledLayerNames     = layers.data();
        Check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
        volkLoadInstance(instance);

        PickDevice();

        const float             priority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &priority;

        const char*        deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount    = 1;
        dci.pQueueCreateInfos       = &qci;
        dci.enabledExtensionCount   = 1;
        dci.ppEnabledExtensionNames = deviceExtensions;
        Check(vkCreateDevice(gpu, &dci, nullptr, &device), "vkCreateDevice");
        volkLoadDevice(device);
        vkGetDeviceQueue(device, family, 0, &queue);

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        Check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");

        CreateSkybox();
        CreateDescriptors();
    }

    // Prefers a discrete GPU; needs a queue that draws and can present to a Win32 window.
    void PickDevice() {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> gpus(n);
        vkEnumeratePhysicalDevices(instance, &n, gpus.data());

        int best = -1;
        for (VkPhysicalDevice candidate : gpus) {
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> exts(extCount);
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, exts.data());
            const bool hasSwapchain = std::any_of(exts.begin(), exts.end(), [](const auto& e) {
                return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            });
            if (!hasSwapchain) continue;

            uint32_t qCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qCount, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qCount, qs.data());
            for (uint32_t i = 0; i < qCount; ++i) {
                if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                if (!vkGetPhysicalDeviceWin32PresentationSupportKHR(candidate, i)) continue;

                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(candidate, &props);
                const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                                  : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                               : 1;
                if (score > best) {
                    best   = score;
                    gpu    = candidate;
                    family = i;
                }
                break;
            }
        }
        if (!gpu) throw VkFailure("no Vulkan device can present to a window");

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        app::Log("gpu: %s", props.deviceName);
    }

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        }
        throw VkFailure("no suitable memory type");
    }

    void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buffer,
                      VkDeviceMemory& memory) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = size;
        bci.usage       = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(device, &bci, nullptr, &buffer), "vkCreateBuffer");

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buffer, &req);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, props);
        Check(vkAllocateMemory(device, &mai, nullptr, &memory), "vkAllocateMemory");
        Check(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");
    }

    void OneShot(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = pool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        Check(vkAllocateCommandBuffers(device, &cai, &cmd), "vkAllocateCommandBuffers");

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        record(cmd);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(device, pool, 1, &cmd);
    }

    static void ImageBarrier(VkCommandBuffer cmd, VkImage image, uint32_t levels, uint32_t layers, VkImageLayout from,
                             VkImageLayout to,
                             VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                             VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask       = srcAccess;
        b.dstAccessMask       = dstAccess;
        b.oldLayout           = from;
        b.newLayout           = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = image;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    void CreateSkybox() {
        const SkyboxImage sky   = GenerateSkybox(kSkyboxSize, kSkyboxSeed);
        const VkDeviceSize bytes = sky.texels.size() * sizeof(uint32_t);
        app::Log("skybox: %d^2 x 6, %d levels generated", sky.size, sky.levels);

        VkBuffer       staging;
        VkDeviceMemory stagingMemory;
        CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMemory);
        void* mapped = nullptr;
        Check(vkMapMemory(device, stagingMemory, 0, bytes, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, sky.texels.data(), bytes);
        vkUnmapMemory(device, stagingMemory);

        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        ici.extent        = {static_cast<uint32_t>(sky.size), static_cast<uint32_t>(sky.size), 1};
        ici.mipLevels     = static_cast<uint32_t>(sky.levels);
        ici.arrayLayers   = 6;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(vkCreateImage(device, &ici, nullptr, &skyImage), "vkCreateImage(skybox)");

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, skyImage, &req);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(device, &mai, nullptr, &skyMemory), "vkAllocateMemory(skybox)");
        Check(vkBindImageMemory(device, skyImage, skyMemory, 0), "vkBindImageMemory");

        OneShot([&](VkCommandBuffer cmd) {
            ImageBarrier(cmd, skyImage, ici.mipLevels, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            std::vector<VkBufferImageCopy> regions;
            for (int l = 0; l < sky.levels; ++l) {
                const uint32_t    s = static_cast<uint32_t>(sky.size >> l);
                VkBufferImageCopy region{};
                region.bufferOffset     = sky.levelOffset[l] * sizeof(uint32_t);
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(l), 0, 6};
                region.imageExtent      = {s, s, 1};
                regions.push_back(region);
            }
            vkCmdCopyBufferToImage(cmd, staging, skyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<uint32_t>(regions.size()), regions.data());
            ImageBarrier(cmd, skyImage, ici.mipLevels, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image            = skyImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
        vci.format           = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, ici.mipLevels, 0, 6};
        Check(vkCreateImageView(device, &vci, nullptr, &skyView), "vkCreateImageView(skybox)");

        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod       = VK_LOD_CLAMP_NONE;
        Check(vkCreateSampler(device, &sci, nullptr, &sampler), "vkCreateSampler");
    }

    void CreateDescriptors() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1;
        lci.pBindings    = &binding;
        Check(vkCreateDescriptorSetLayout(device, &lci, nullptr, &setLayout), "vkCreateDescriptorSetLayout");

        VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount         = 1;
        plci.pSetLayouts            = &setLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges    = &range;
        Check(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout), "vkCreatePipelineLayout");

        VkDescriptorPoolSize       size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets       = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes    = &size;
        Check(vkCreateDescriptorPool(device, &dpci, nullptr, &descriptorPool), "vkCreateDescriptorPool");

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = descriptorPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &setLayout;
        Check(vkAllocateDescriptorSets(device, &dsai, &descriptorSet), "vkAllocateDescriptorSets");

        VkDescriptorImageInfo image{sampler, skyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet  write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = descriptorSet;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    void CreateRenderPassAndPipeline(VkFormat format) {
        VkAttachmentDescription color{};
        color.format         = format;
        color.samples        = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
        color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription  subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments    = &color;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &subpass;
        rpci.dependencyCount = 1;
        rpci.pDependencies   = &dep;
        Check(vkCreateRenderPass(device, &rpci, nullptr, &renderPass), "vkCreateRenderPass");
        renderPassFormat = format;

        const std::vector<uint32_t> vert = ShaderWords("fullscreen.vert");
        const std::vector<uint32_t> frag = ShaderWords("blackhole.frag");
        VkShaderModule              modules[2]{};
        for (int i = 0; i < 2; ++i) {
            const auto&              code = i == 0 ? vert : frag;
            VkShaderModuleCreateInfo smci{};
            smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            smci.codeSize = code.size() * sizeof(uint32_t);
            smci.pCode    = code.data();
            Check(vkCreateShaderModule(device, &smci, nullptr, &modules[i]), "vkCreateShaderModule");
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        for (int i = 0; i < 2; ++i) {
            stages[i].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage  = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[i].module = modules[i];
            stages[i].pName  = "main";
        }

        VkPipelineVertexInputStateCreateInfo   vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments    = &blendAttachment;

        const VkDynamicState             dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamicStates;

        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount          = 2;
        gpci.pStages             = stages;
        gpci.pVertexInputState   = &vertexInput;
        gpci.pInputAssemblyState = &assembly;
        gpci.pViewportState      = &viewport;
        gpci.pRasterizationState = &raster;
        gpci.pMultisampleState   = &multisample;
        gpci.pColorBlendState    = &blend;
        gpci.pDynamicState       = &dynamic;
        gpci.layout              = pipelineLayout;
        gpci.renderPass          = renderPass;
        const VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
        for (VkShaderModule m : modules) vkDestroyShaderModule(device, m, nullptr);
        Check(r, "vkCreateGraphicsPipelines");
    }

    // -- windows -------------------------------------------------------------------------------

    VkFormat ChooseFormat(VkSurfaceKHR surface) const {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, formats.data());
        if (formats.empty()) throw VkFailure("surface reports no formats");

        auto has = [&](VkFormat f) {
            return std::any_of(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& s) {
                return s.format == f && s.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
        };
        if (renderPass) {
            if (!has(renderPassFormat)) throw VkFailure("window does not support the first window's format");
            return renderPassFormat;
        }
        for (VkFormat f : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_FORMAT_R8G8B8A8_UNORM}) {
            if (has(f)) return f;
        }
        return formats[0].format;
    }

    void Attach(HWND hwnd) {
        auto t  = std::make_unique<Target>();
        t->hwnd = hwnd;

        VkWin32SurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd      = hwnd;
        Check(vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &t->surface), "vkCreateWin32SurfaceKHR");

        try {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, family, t->surface, &supported);
            if (!supported) throw VkFailure("queue cannot present to this window");

            t->format = ChooseFormat(t->surface);
            if (!renderPass) CreateRenderPassAndPipeline(t->format);

            VkCommandBufferAllocateInfo cai{};
            cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.commandPool        = pool;
            cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            VkSemaphoreCreateInfo semInfo{};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo     fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            for (Frame& f : t->frames) {
                Check(vkAllocateCommandBuffers(device, &cai, &f.cmd), "vkAllocateCommandBuffers");
                Check(vkCreateSemaphore(device, &semInfo, nullptr, &f.imageAvailable), "vkCreateSemaphore");
                Check(vkCreateFence(device, &fenceInfo, nullptr, &f.fence), "vkCreateFence");
            }
            BuildSwapchain(*t);
        } catch (...) {
            DestroyTarget(*t);
            throw;
        }
        app::Log("attached %p: format %d, %ux%u", static_cast<void*>(hwnd), t->format, t->extent.width,
                 t->extent.height);
        targets.push_back(std::move(t));
    }

    void DestroySwapchainViews(Target& t) {
        for (VkFramebuffer fb : t.framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        for (VkImageView v : t.views) vkDestroyImageView(device, v, nullptr);
        for (VkSemaphore s : t.renderDone) vkDestroySemaphore(device, s, nullptr);
        t.framebuffers.clear();
        t.views.clear();
        t.renderDone.clear();
        t.images.clear();
    }

    void DestroyTarget(Target& t) {
        DestroySwapchainViews(t);
        for (Frame& f : t.frames) {
            if (f.cmd) vkFreeCommandBuffers(device, pool, 1, &f.cmd);
            vkDestroySemaphore(device, f.imageAvailable, nullptr);
            vkDestroyFence(device, f.fence, nullptr);
            f = Frame{};
        }
        vkDestroySwapchainKHR(device, t.swapchain, nullptr);
        vkDestroySurfaceKHR(instance, t.surface, nullptr);
        t.swapchain = VK_NULL_HANDLE;
        t.surface   = VK_NULL_HANDLE;
    }

    // Returns false while the window has no area (minimised); the caller tries again next frame.
    bool BuildSwapchain(Target& t) {
        VkSurfaceCapabilitiesKHR caps;
        Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, t.surface, &caps), "surface capabilities");

        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            RECT rc{};
            GetClientRect(t.hwnd, &rc);
            extent.width  = std::clamp(static_cast<uint32_t>(rc.right), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(rc.bottom), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) return false;

        // Rebuilds are rare (resize, monitor change), so the simplest safe fence is a full idle:
        // it covers the previous images' semaphores as well as their command buffers.
        vkDeviceWaitIdle(device);
        DestroySwapchainViews(t);

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & alpha)) {
            for (uint32_t bit = 1; bit <= caps.supportedCompositeAlpha; bit <<= 1) {
                if (caps.supportedCompositeAlpha & bit) {
                    alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                    break;
                }
            }
        }

        t.canCapture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface          = t.surface;
        sci.minImageCount    = imageCount;
        sci.imageFormat      = t.format;
        sci.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sci.imageExtent      = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (t.canCapture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform     = caps.currentTransform;
        sci.compositeAlpha   = alpha;
        sci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;  // always available, and vsync suits a screen saver
        sci.clipped          = VK_TRUE;
        sci.oldSwapchain     = t.swapchain;

        VkSwapchainKHR next = VK_NULL_HANDLE;
        Check(vkCreateSwapchainKHR(device, &sci, nullptr, &next), "vkCreateSwapchainKHR");
        vkDestroySwapchainKHR(device, t.swapchain, nullptr);
        t.swapchain = next;
        t.extent    = extent;

        uint32_t n = 0;
        vkGetSwapchainImagesKHR(device, t.swapchain, &n, nullptr);
        t.images.resize(n);
        vkGetSwapchainImagesKHR(device, t.swapchain, &n, t.images.data());

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (VkImage image : t.images) {
            VkImageViewCreateInfo vci{};
            vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vci.image            = image;
            vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            vci.format           = t.format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            Check(vkCreateImageView(device, &vci, nullptr, &view), "vkCreateImageView");
            t.views.push_back(view);

            VkFramebufferCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass      = renderPass;
            fci.attachmentCount = 1;
            fci.pAttachments    = &view;
            fci.width           = extent.width;
            fci.height          = extent.height;
            fci.layers          = 1;
            VkFramebuffer fb;
            Check(vkCreateFramebuffer(device, &fci, nullptr, &fb), "vkCreateFramebuffer");
            t.framebuffers.push_back(fb);

            VkSemaphore done;
            Check(vkCreateSemaphore(device, &semInfo, nullptr, &done), "vkCreateSemaphore");
            t.renderDone.push_back(done);
        }
        t.stale = false;
        return true;
    }

    // -- drawing -------------------------------------------------------------------------------

    void Draw(Target& t, double seconds) {
        if (t.stale && !BuildSwapchain(t)) return;

        Frame& f = t.frames[t.frameIndex];
        vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);

        uint32_t index = 0;
        VkResult r     = vkAcquireNextImageKHR(device, t.swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            t.stale = true;
            return;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) Check(r, "vkAcquireNextImageKHR");
        vkResetFences(device, 1, &f.fence);

        // A capture copies the finished image into a host-visible buffer in the same submission.
        if (captureWindow == t.hwnd && !t.canCapture) {
            app::Log("capture: this swapchain cannot be read back");
            captureWindow = nullptr;
        }
        const bool     capture       = captureWindow == t.hwnd;
        VkBuffer       captureBuffer = VK_NULL_HANDLE;
        VkDeviceMemory captureMemory = VK_NULL_HANDLE;
        const VkDeviceSize captureBytes = static_cast<VkDeviceSize>(t.extent.width) * t.extent.height * 4;
        if (capture) {
            CreateBuffer(captureBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, captureBuffer,
                         captureMemory);
        }

        const float aspect = static_cast<float>(t.extent.width) / static_cast<float>(t.extent.height);
        const PushConstants pc = MakeCamera(seconds, aspect, !IsSrgb(t.format));

        VkCommandBuffer cmd = f.cmd;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkRenderPassBeginInfo rpbi{};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass  = renderPass;
        rpbi.framebuffer = t.framebuffers[index];
        rpbi.renderArea  = {{0, 0}, t.extent};
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(t.extent.width), static_cast<float>(t.extent.height), 0.0f, 1.0f};
        VkRect2D   scissor{{0, 0}, t.extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);

        if (capture) {
            VkImage image = t.images[index];
            ImageBarrier(cmd, image, 1, 1, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent      = {t.extent.width, t.extent.height, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer, 1, &region);
            ImageBarrier(cmd, image, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        vkEndCommandBuffer(cmd);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo               submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &f.imageAvailable;
        submit.pWaitDstStageMask    = &waitStage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &t.renderDone[index];
        Check(vkQueueSubmit(queue, 1, &submit, f.fence), "vkQueueSubmit");

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &t.renderDone[index];
        present.swapchainCount     = 1;
        present.pSwapchains        = &t.swapchain;
        present.pImageIndices      = &index;
        r                          = vkQueuePresentKHR(queue, &present);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            t.stale = true;
        } else {
            Check(r, "vkQueuePresentKHR");
        }

        if (capture) {
            vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
            void* mapped = nullptr;
            if (vkMapMemory(device, captureMemory, 0, captureBytes, 0, &mapped) == VK_SUCCESS) {
                const bool rgba = t.format == VK_FORMAT_R8G8B8A8_SRGB || t.format == VK_FORMAT_R8G8B8A8_UNORM;
                if (!WriteBmp(capturePath, static_cast<const uint8_t*>(mapped), t.extent.width, t.extent.height, rgba)) {
                    app::Log("capture: could not write %s", capturePath.c_str());
                }
                vkUnmapMemory(device, captureMemory);
            }
            vkDestroyBuffer(device, captureBuffer, nullptr);
            vkFreeMemory(device, captureMemory, nullptr);
            captureWindow = nullptr;
        }

        t.frameIndex = (t.frameIndex + 1) % kFramesInFlight;
    }
};

// ---- public surface -------------------------------------------------------------------------

Renderer::Renderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Renderer::~Renderer() = default;

std::unique_ptr<Renderer> Renderer::Create() {
    try {
        auto impl = std::make_unique<Impl>();
        impl->Init();
        return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
    } catch (const std::exception& e) {
        app::Log("vulkan unavailable: %s", e.what());
        return nullptr;
    }
}

bool Renderer::AttachWindow(HWND hwnd) {
    try {
        impl_->Attach(hwnd);
        return true;
    } catch (const std::exception& e) {
        app::Log("attach %p failed: %s", static_cast<void*>(hwnd), e.what());
        return false;
    }
}

void Renderer::ResizeWindow(HWND hwnd) {
    for (auto& t : impl_->targets) {
        if (t->hwnd == hwnd) t->stale = true;
    }
}

void Renderer::DetachWindow(HWND hwnd) {
    auto& targets = impl_->targets;
    for (auto it = targets.begin(); it != targets.end(); ++it) {
        if ((*it)->hwnd == hwnd) {
            vkDeviceWaitIdle(impl_->device);
            impl_->DestroyTarget(**it);
            targets.erase(it);
            return;
        }
    }
}

void Renderer::RenderFrame(double seconds) {
    for (auto& t : impl_->targets) {
        try {
            impl_->Draw(*t, seconds);
        } catch (const std::exception& e) {
            // A lost surface or device leaves this window black rather than ending the saver.
            app::Log("draw %p failed: %s", static_cast<void*>(t->hwnd), e.what());
            t->stale = true;
        }
    }
}

void Renderer::RequestCapture(HWND hwnd, const std::string& path) {
    impl_->captureWindow = hwnd;
    impl_->capturePath   = path;
}

bool Renderer::CapturePending() const { return impl_->captureWindow != nullptr; }

void Renderer::WaitIdle() { vkDeviceWaitIdle(impl_->device); }

}  // namespace render
