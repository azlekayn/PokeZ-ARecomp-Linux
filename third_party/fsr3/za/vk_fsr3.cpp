// AMD FSR 3.1's upscaler (third_party/fsr3, FidelityFX SDK v1.1.4) for exec.c's temporal anti-aliasing: the same inputs
// and the same place in the frame as FSR 2 (runtime/nvn/vulkan/fsr2.cpp, VkFsr2Frame). A native build, not AMD's signed
// Windows DLLs (FSR 4 only exists as those). Linked with the SDK's sources into one object whose only global symbols
// are vk_fsr3_* (the Makefile): the SDK and our FSR 2 share a lot of function names.
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../../runtime/nvn/vulkan/vk_fsr2.h"

static void LOG(const char *fmt, ...) {  // (as runtime/rt.h rt_log: one write per line)
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) fwrite(b, 1, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1, stderr);
}

// (ffx_vk.cpp's only link to the SDK's frame interpolation swapchain, not built here: exec.c has its own frame generation)
FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *) { return FFX_ERROR_BACKEND_API_ERROR; }

static FfxFsr3UpscalerContext ctx;
static bool have;
static void *scratch;
static VkDeviceContext dc;  // (the backend keeps a pointer to it)
static uint32_t cur_iw, cur_ih, cur_ow, cur_oh;
static bool failed;  // (the context couldn't be made: not tried again until the sizes change)

static void message(FfxMsgType type, const wchar_t *m) {
    char b[512];
    size_t i = 0;
    for (; m && m[i] && i < sizeof b - 1; i++) b[i] = (char)m[i];
    b[i] = 0;
    static int n;
    if (n++ < 50) LOG("[fsr3] %s: %s\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

// Three images FSR 3 leaves to its caller (ffxFsr3UpscalerGetSharedResourceDescriptions: AMD's frame interpolation reads
// them too), at the largest render size. Without them its first dispatch clears a null buffer and crashes.
enum { SH_DEPTH, SH_MV, SH_PREV, SH_N };
static const VkFormat k_shared_format[SH_N] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_UINT};
static VkImage shared[SH_N];
static VkDeviceMemory shared_mem[SH_N];
static bool shared_fresh;  // (still in their first layout: the next dispatch's command buffer moves them to GENERAL)

static void shared_free(VkDevice dev) {
    for (int i = 0; i < SH_N; i++) {
        if (shared[i]) vkDestroyImage(dev, shared[i], NULL);
        if (shared_mem[i]) vkFreeMemory(dev, shared_mem[i], NULL);
        shared[i] = VK_NULL_HANDLE, shared_mem[i] = VK_NULL_HANDLE;
    }
}

static bool shared_make(VkPhysicalDevice phys, VkDevice dev, uint32_t w, uint32_t h) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (int i = 0; i < SH_N; i++) {
        VkImageCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = k_shared_format[i];
        ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // (it clears them)
        if (vkCreateImage(dev, &ci, NULL, &shared[i]) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, shared[i], &mr);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++) {
            VkMemoryPropertyFlags fl = mp.memoryTypes[t].propertyFlags;
            if ((mr.memoryTypeBits >> t & 1) && (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                !(fl & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)))
                type = t;
        }
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(dev, &ai, NULL, &shared_mem[i]) != VK_SUCCESS) return false;
        if (vkBindImageMemory(dev, shared[i], shared_mem[i], 0) != VK_SUCCESS) return false;
    }
    shared_fresh = true;
    return true;
}

extern "C" void vk_fsr3_destroy(VkDevice dev) {
    if (!have) return;
    vkDeviceWaitIdle(dev);
    ffxFsr3UpscalerContextDestroy(&ctx);
    shared_free(dev);
    have = false;
}

// (The backend makes its own views from this: the format and the usage flags pick the view's format and aspect, and a
// depth image with stencil needs both flags or its depth view has the wrong format.)
static FfxResource resource(VkImage image, VkFormat format, uint32_t w, uint32_t h, FfxResourceStates state, bool storage) {
    FfxResourceDescription d = {};
    d.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    d.format = ffxGetSurfaceFormatVK(format);
    d.width = w, d.height = h, d.depth = 1, d.mipCount = 1;
    d.flags = FFX_RESOURCE_FLAGS_NONE;
    uint32_t usage = storage ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
    if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM) usage |= FFX_RESOURCE_USAGE_DEPTHTARGET;
    if (format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT)
        usage |= FFX_RESOURCE_USAGE_DEPTHTARGET | FFX_RESOURCE_USAGE_STENCILTARGET;
    d.usage = (FfxResourceUsage)usage;
    return ffxGetResourceVK((void *)image, d, NULL, state);
}

extern "C" bool vk_fsr3_record(VkPhysicalDevice phys, VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f) {
    bool sizes = f->iw != cur_iw || f->ih != cur_ih || f->ow != cur_ow || f->oh != cur_oh;
    if (failed && !sizes) return false;
    if (!have || sizes) {
        vk_fsr3_destroy(dev);
        cur_iw = f->iw, cur_ih = f->ih, cur_ow = f->ow, cur_oh = f->oh;
        failed = true;
        size_t sz = ffxGetScratchMemorySizeVK(phys, FFX_FSR3UPSCALER_CONTEXT_COUNT);
        free(scratch);
        scratch = calloc(1, sz);
        dc = {dev, phys, vkGetDeviceProcAddr};
        FfxFsr3UpscalerContextDescription d = {};
        if (!scratch || ffxGetInterfaceVK(&d.backendInterface, ffxGetDeviceVK(&dc), scratch, sz, FFX_FSR3UPSCALER_CONTEXT_COUNT) != FFX_OK) {
            LOG("[fsr3] no backend interface\n");
            return false;
        }
        // (the picture is tone-mapped already, as for FSR 2: LDR input, no exposure texture)
        d.flags = FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;
        d.maxRenderSize = {f->iw, f->ih};
        d.maxUpscaleSize = {f->ow, f->oh};
        d.fpMessage = message;
        FfxErrorCode e = ffxFsr3UpscalerContextCreate(&ctx, &d);
        if (e != FFX_OK) { LOG("[fsr3] context failed: %d\n", (int)e); return false; }
        if (!shared_make(phys, dev, f->iw, f->ih)) {
            LOG("[fsr3] no memory for its images\n");
            ffxFsr3UpscalerContextDestroy(&ctx);
            shared_free(dev);
            return false;
        }
        have = true, failed = false;
        LOG("[fsr3] context up to %ux%u -> %ux%u\n", f->iw, f->ih, f->ow, f->oh);
    }
    if (shared_fresh) {
        shared_fresh = false;
        for (int i = 0; i < SH_N; i++) {
            VkImageMemoryBarrier b = {};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = shared[i];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        }
    }
    FfxFsr3UpscalerDispatchDescription d = {};
    d.commandList = ffxGetCommandListVK(cmd);
    d.dilatedDepth = resource(shared[SH_DEPTH], k_shared_format[SH_DEPTH], f->iw, f->ih, FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
    d.dilatedMotionVectors = resource(shared[SH_MV], k_shared_format[SH_MV], f->iw, f->ih, FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
    d.reconstructedPrevNearestDepth = resource(shared[SH_PREV], k_shared_format[SH_PREV], f->iw, f->ih, FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
    d.color = resource(f->color, f->color_format, f->iw, f->ih, FFX_RESOURCE_STATE_COMPUTE_READ, false);
    d.depth = resource(f->depth, f->depth_format, f->iw, f->ih, FFX_RESOURCE_STATE_COMPUTE_READ, false);
    d.motionVectors = resource(f->mv, VK_FORMAT_R16G16_SFLOAT, f->w, f->h, FFX_RESOURCE_STATE_COMPUTE_READ, false);
    d.reactive = resource(f->reactive, VK_FORMAT_R8_UNORM, f->w, f->h, FFX_RESOURCE_STATE_COMPUTE_READ, false);
    d.output = resource(f->out, f->out_format, f->ow, f->oh, FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
    d.jitterOffset = {f->jx, f->jy};
    d.motionVectorScale = {1, 1};  // (pixels: it divides by the render size)
    d.renderSize = {f->w, f->h};
    d.upscaleSize = {f->ow, f->oh};
    d.enableSharpening = f->sharpness > 0;
    d.sharpness = f->sharpness;
    d.frameTimeDelta = f->dt_ms;
    d.preExposure = 1;
    d.reset = f->reset;
    d.cameraNear = f->near_z;
    d.cameraFar = FLT_MAX;
    d.cameraFovAngleVertical = f->fov_y;
    d.viewSpaceToMetersFactor = 1;
    FfxErrorCode e = ffxFsr3UpscalerContextDispatch(&ctx, &d);
    if (e != FFX_OK) {
        static int n;
        if (n++ < 10) LOG("[fsr3] dispatch failed: %d\n", (int)e);
        return false;
    }
    return true;
}
