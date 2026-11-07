// AMD FSR 3.1's upscaler shaders (third_party/fsr3, FidelityFX SDK v1.1.4) for its Vulkan backend: the replacement for
// the SDK's src/backends/shared/ffx_shader_blobs.cpp. As for FSR 2 (runtime/nvn/vulkan/fsr2_shaders.cpp): AMD ships
// precompiled permutations from its own shader compiler; here the GLSL sources are built in (fsr3_shaders.inc) and each
// permutation a pass asks for is compiled once with shaderc and kept. The bindings the backend needs by name come from
// the SPIR-V itself (spv_reflect). Unlike FSR 2's, this backend keeps everything in descriptor set 0.
#include <shaderc/shaderc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <ffx_shader_blobs.h>
#include "../src/components/fsr3upscaler/ffx_fsr3upscaler_private.h"

static void LOG(const char *fmt, ...) {  // (as runtime/rt.h rt_log: one write per line)
    char b[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) fwrite(b, 1, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1, stderr);
}

static const struct { const char *name, *text; } k_sources[] = {
#include "fsr3_shaders.inc"
};

// (the sources are listed by file name, all different: "fsr3upscaler/ffx_x.h" and "ffx_x.h" are the same one)
static shaderc_include_result *include_resolve(void *, const char *requested, int, const char *, size_t) {
    shaderc_include_result *r = (shaderc_include_result *)calloc(1, sizeof *r);
    const char *base = strrchr(requested, '/');
    base = base ? base + 1 : requested;
    for (const auto &s : k_sources)
        if (!strcmp(s.name, base)) {
            r->source_name = s.name, r->source_name_length = strlen(s.name);
            r->content = s.text, r->content_length = strlen(s.text);
            return r;
        }
    static const char missing[] = "fsr3_shaders: no such include";
    r->content = missing, r->content_length = sizeof missing - 1;  // (an empty name: shaderc reports the error)
    return r;
}
static void include_release(void *, shaderc_include_result *r) { free(r); }

struct Blob {
    uint32_t pass, options;
    std::vector<uint32_t> spv;
    std::vector<std::string> names[3];  // storage images, sampled images, uniform buffers
    std::vector<const char *> cnames[3];
    std::vector<uint32_t> bindings[3], counts[3], spaces[3];
};

// The set 0 resources a SPIR-V module's entry point can reach. Unoptimized, it still has every function its includes
// define, called or not, and the resources those touch: the autogen reactive pass's rw_output_autocomposition is in none
// of AMD's name tables (context creation fails with "invalid argument"), and nothing would be bound to it. AMD's compiler
// drops those, so this walks the calls from the entry point. (The samplers, bindings 1000 up, are the backend's own.)
static void spv_reflect(Blob &b) {
    const std::vector<uint32_t> &w = b.spv;
    size_t n = w.size();
    if (n < 5) return;
    uint32_t bound = w[3];
    std::vector<std::string> name(bound);
    std::vector<int32_t> binding(bound, -1), set(bound, -1);
    std::vector<uint32_t> op(bound), a1(bound), a2(bound), a3(bound), konst(bound);
    std::vector<uint8_t> used(bound), seen(bound);
    std::vector<std::vector<uint32_t>> calls(bound), refs(bound);
    uint32_t entry = 0, cur = 0;
    struct Var { uint32_t id, type; };
    std::vector<Var> vars;
    for (size_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16, code = w[i] & 0xFFFF;
        if (!len || i + len > n) break;
        const uint32_t *o = &w[i + 1];
        switch (code) {
        case 15:  // OpEntryPoint: model, id, ...
            entry = o[1];
            break;
        case 54:  // OpFunction: type, result, ...
            cur = o[1];
            break;
        case 56:  // OpFunctionEnd
            cur = 0;
            break;
        case 57:  // OpFunctionCall: type, result, function, arguments (variables can be passed)
            if (cur < bound && o[2] < bound) calls[cur].push_back(o[2]);
            for (size_t k = 3; k < (size_t)len - 1; k++)
                if (cur < bound) refs[cur].push_back(o[k]);
            break;
        case 60:  // OpImageTexelPointer: type, result, image
        case 61:  // OpLoad: type, result, pointer
        case 65:  // OpAccessChain: type, result, base
        case 83:  // OpCopyObject: type, result, operand
            if (cur < bound) refs[cur].push_back(o[2]);
            break;
        case 62:  // OpStore: pointer, object
            if (cur < bound) refs[cur].push_back(o[0]);
            break;
        case 5:  // OpName
            if (o[0] < bound) name[o[0]] = (const char *)&o[1];
            break;
        case 71:  // OpDecorate
            if (o[0] < bound && o[1] == 33) binding[o[0]] = (int32_t)o[2];
            if (o[0] < bound && o[1] == 34) set[o[0]] = (int32_t)o[2];
            break;
        case 25:  // OpTypeImage: result, sampled type, dim, depth, arrayed, ms, sampled (1 sampled, 2 storage), format
            if (o[0] < bound) op[o[0]] = 25, a1[o[0]] = o[6];
            break;
        case 28:  // OpTypeArray: result, element type, length (a constant's id)
            if (o[0] < bound) op[o[0]] = 28, a1[o[0]] = o[1], a2[o[0]] = o[2];
            break;
        case 30:  // OpTypeStruct
            if (o[0] < bound) op[o[0]] = 30;
            break;
        case 32:  // OpTypePointer: result, storage class, type
            if (o[0] < bound) op[o[0]] = 32, a2[o[0]] = o[1], a3[o[0]] = o[2];
            break;
        case 43:  // OpConstant: type, result, value
            if (o[1] < bound) konst[o[1]] = o[2];
            break;
        case 59:  // OpVariable: result type, result, storage class
            vars.push_back({o[1], o[0]});
            break;
        }
        i += len;
    }
    std::vector<uint32_t> todo = {entry};
    while (!todo.empty()) {
        uint32_t fn = todo.back();
        todo.pop_back();
        if (fn >= bound || seen[fn]) continue;
        seen[fn] = 1;
        for (uint32_t r : refs[fn]) if (r < bound) used[r] = 1;
        for (uint32_t c : calls[fn]) todo.push_back(c);
    }
    for (const Var &v : vars) {
        if (v.id >= bound || !used[v.id] || set[v.id] != 0 || binding[v.id] < 0 || binding[v.id] >= 1000 || v.type >= bound || op[v.type] != 32) continue;
        uint32_t t = a3[v.type], count = 1;
        while (t < bound && op[t] == 28) count *= a2[t] < bound ? konst[a2[t]] : 1, t = a1[t];
        int kind = t >= bound ? -1 : op[t] == 25 ? (a1[t] == 2 ? 0 : 1) : op[t] == 30 ? 2 : -1;
        if (kind < 0) continue;
        b.names[kind].push_back(name[v.id]);
        b.bindings[kind].push_back((uint32_t)binding[v.id]);
        b.counts[kind].push_back(count);
        b.spaces[kind].push_back(0);
    }
    for (int k = 0; k < 3; k++)
        for (const std::string &s : b.names[k]) b.cnames[k].push_back(s.c_str());
}

static const char *pass_source(uint32_t pass) {
    switch (pass) {
    case FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS: return "ffx_fsr3upscaler_prepare_inputs_pass.glsl";
    case FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID: return "ffx_fsr3upscaler_luma_pyramid_pass.glsl";
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID: return "ffx_fsr3upscaler_shading_change_pyramid_pass.glsl";
    case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE: return "ffx_fsr3upscaler_shading_change_pass.glsl";
    case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY: return "ffx_fsr3upscaler_prepare_reactivity_pass.glsl";
    case FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY: return "ffx_fsr3upscaler_luma_instability_pass.glsl";
    case FFX_FSR3UPSCALER_PASS_ACCUMULATE:
    case FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN: return "ffx_fsr3upscaler_accumulate_pass.glsl";
    case FFX_FSR3UPSCALER_PASS_RCAS: return "ffx_fsr3upscaler_rcas_pass.glsl";
    case FFX_FSR3UPSCALER_PASS_DEBUG_VIEW: return "ffx_fsr3upscaler_debug_view_pass.glsl";
    case FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE: return "ffx_fsr3upscaler_autogen_reactive_pass.glsl";
    default: return NULL;
    }
}

extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool &isWave64) {  // (no wave64 table: the same SPIR-V)
    isWave64 = false;
    return FFX_OK;
}

extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage, uint32_t options, FfxShaderBlob *out) {
    static std::vector<Blob *> cache;
    if (effect != FFX_EFFECT_FSR3UPSCALER) return FFX_ERROR_BACKEND_API_ERROR;
    // (16-bit types: the device is made without shaderFloat16 / shaderInt16 - vulkan/device.c - so never those)
    options &= ~(uint32_t)(FSR3UPSCALER_SHADER_PERMUTATION_FORCE_WAVE64 | FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16);
    Blob *b = NULL;
    for (Blob *c : cache)
        if (c->pass == (uint32_t)pass && c->options == options) b = c;
    if (!b) {
        const char *file = pass_source((uint32_t)pass), *text = NULL;
        for (const auto &s : k_sources)
            if (file && !strcmp(s.name, file)) text = s.text;
        if (!text) { LOG("[fsr3] no shader for pass %d\n", (int)pass); return FFX_ERROR_BACKEND_API_ERROR; }
        shaderc_compiler_t comp = shaderc_compiler_initialize();
        shaderc_compile_options_t o = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        // (unoptimized: shaderc's optimizer strips the names the backend binds by; the driver optimizes anyway)
        shaderc_compile_options_set_optimization_level(o, shaderc_optimization_level_zero);
        shaderc_compile_options_set_include_callbacks(o, include_resolve, include_release, NULL);
        struct { const char *d; const char *v; } fixed[] = {  // (AMD's CMakeCompileFSR3UpscalerShaders.txt)
            {"FFX_GLSL", "1"}, {"FFX_GPU", "1"}, {"FFX_HALF", "0"},
            {"FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF", "0"},
            {"FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF", "0"},
            {"FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF", "1"},
            {"FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF", "0"},
            {"FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE", "2"}};
        for (const auto &d : fixed) shaderc_compile_options_add_macro_definition(o, d.d, strlen(d.d), d.v, strlen(d.v));
        struct { const char *d; uint32_t flag; } defs[] = {
            {"FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE", FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE},
            {"FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT", FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT},
            {"FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS", FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS},
            {"FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS", FSR3UPSCALER_SHADER_PERMUTATION_JITTER_MOTION_VECTORS},
            {"FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH", FSR3UPSCALER_SHADER_PERMUTATION_DEPTH_INVERTED},
            {"FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING", FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING}};
        for (const auto &d : defs) shaderc_compile_options_add_macro_definition(o, d.d, strlen(d.d), options & d.flag ? "1" : "0", 1);
        shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, text, strlen(text), shaderc_compute_shader, file, "main", o);
        if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
            b = new Blob;
            b->pass = (uint32_t)pass, b->options = options;
            size_t bytes = shaderc_result_get_length(r);
            b->spv.assign((const uint32_t *)shaderc_result_get_bytes(r), (const uint32_t *)shaderc_result_get_bytes(r) + bytes / 4);
            spv_reflect(*b);
            cache.push_back(b);
            if (getenv("LARECOMP_FSR3_REFLECT"))
                for (int k = 0; k < 3; k++)
                    for (size_t i = 0; i < b->names[k].size(); i++)
                        LOG("[fsr3]   %c %u x%u %s\n", "sic"[k], b->bindings[k][i], b->counts[k][i], b->names[k][i].c_str());
            LOG("[fsr3] %s (options %#x): %zu bytes, %zu storage / %zu sampled images, %zu buffers\n", file, options, bytes,
                b->names[0].size(), b->names[1].size(), b->names[2].size());
        } else {
            LOG("[fsr3] %s (options %#x) failed to compile:\n%s\n", file, options, shaderc_result_get_error_message(r));
        }
        shaderc_result_release(r);
        shaderc_compile_options_release(o);
        shaderc_compiler_release(comp);
        if (!b) return FFX_ERROR_BACKEND_API_ERROR;
    }
    FfxShaderBlob blob = {(const uint8_t *)b->spv.data(), (uint32_t)(b->spv.size() * 4),
        (uint32_t)b->names[2].size(), (uint32_t)b->names[1].size(), (uint32_t)b->names[0].size(), 0, 0, 0, 0,
        b->cnames[2].data(), b->bindings[2].data(), b->counts[2].data(), b->spaces[2].data(),
        b->cnames[1].data(), b->bindings[1].data(), b->counts[1].data(), b->spaces[1].data(),
        b->cnames[0].data(), b->bindings[0].data(), b->counts[0].data(), b->spaces[0].data()};
    memcpy(out, &blob, sizeof blob);  // (its members are const)
    return FFX_OK;
}
