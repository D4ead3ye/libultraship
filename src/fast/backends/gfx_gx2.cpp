/*  gfx_gx2.cpp - Fast3D GX2 backend for libultraship

    Created in 2022 by GaryOderNichts
*/
#ifdef __WIIU__

#include "ship/window/Window.h"
#include "fast/Fast3dWindow.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <malloc.h>

#include <map>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include "libultraship/libultra/gbi.h"
#include <libultraship/bridge/consolevariablebridge.h>

#include "fast/backends/gfx_rendering_api.h"
#include "fast/backends/gfx_gx2.h"
#include <coreinit/memheap.h>
#include <coreinit/memexpheap.h>
#include <whb/log.h>
#include "fast/backends/gfx_wiiu.h"
#include "fast/interpreter.h"

#include <gx2/texture.h>
#include <gx2/draw.h>
#include <gx2/clear.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <gx2/event.h>
#include <gx2/utils.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/display.h>
#include "fast/backends/gx2_shader_gen.h"
#include "gx2_util.h"

#include <proc_ui/procui.h>
#include <coreinit/memory.h>

#include <ship/port/wiiu/ImGui/imgui_impl_gx2.h>

namespace Fast {

#define ALIGN(x, align) (((x) + ((align)-1)) & ~((align)-1))

struct GX2TextureObj {
    GX2Texture texture;
    bool texture_uploaded;

    GX2Sampler sampler;
    bool sampler_set;

    // For ImGui rendering
    ImGui_ImplGX2_Texture imtex;

    // [port] The parameters this framebuffer was last built with. The foreground
    // handover frees these surfaces, but the interpreter only calls
    // UpdateFramebufferParameters when the size it wants *changes* - and it does
    // not change across a handover, so the call never comes and the surface stays
    // freed. Keeping the parameters lets the restore rebuild them itself.
    bool paramsValid;
    uint32_t lastWidth;
    uint32_t lastHeight;
    uint32_t lastMsaa;
    bool lastInvertY;
    bool lastRenderTarget;
    bool lastHasDepth;
    bool lastCanExtractDepth;
};

struct Framebuffer {
    GX2ColorBuffer color_buffer;
    bool colorBufferMem1;
    GX2DepthBuffer depth_buffer;
    bool depthBufferMem1;

    GX2Texture texture;
    GX2Sampler sampler;

    // For ImGui rendering
    ImGui_ImplGX2_Texture imtex;

    // [port] The parameters this framebuffer was last built with. The foreground
    // handover frees these surfaces, but the interpreter only calls
    // UpdateFramebufferParameters when the size it wants *changes* - and it does
    // not change across a handover, so the call never comes and the surface stays
    // freed. Keeping the parameters lets the restore rebuild them itself.
    bool paramsValid;
    uint32_t lastWidth;
    uint32_t lastHeight;
    uint32_t lastMsaa;
    bool lastInvertY;
    bool lastRenderTarget;
    bool lastHasDepth;
    bool lastCanExtractDepth;
};

// [port] The MEM1 restore hook has C linkage and no instance of its own, but it
// needs to rebuild framebuffers through the normal path. Init records it here.
static GfxRenderingAPIGX2* sActiveApi = nullptr;

static std::array<Framebuffer, 100> framebuffers;
static std::size_t used_framebuffers;
static std::size_t current_framebuffer;
static GX2DepthBuffer depthReadBuffer;

static std::map<std::pair<uint64_t, uint64_t>, struct ShaderProgram> shader_program_pool;
static struct ShaderProgram* current_shader_program;

static struct GX2TextureObj* current_texture;
// Per-tile, because a texture binding is only valid for the shader that was
// current when it was made. Fast3D re-selects a texture only when the texture
// changes, so a draw that switches shader while keeping the same texture would
// otherwise leave it bound to the previous shader's sampler slot.
static struct GX2TextureObj* current_textures[SHADER_MAX_TEXTURES];

// [port] A framebuffer bound as a texture does not live in current_textures -
// SelectTextureFb hands GX2 the framebuffer's own texture object directly. Track
// it separately so that a LoadShader arriving after SelectTextureFb rebinds the
// framebuffer rather than clobbering sampler 0 with the last ordinary texture.
// [port] MSAA on the main framebuffer only. Latte shades once per pixel and
// multi-samples coverage, so this costs bandwidth rather than shading - much
// cheaper than raising the internal resolution, which was measured at ~22fps at
// 1080p and rejected. MEM1 has 26 MB free after the framebuffers; 2x needs about
// 7 MB plus an aux buffer and fits, 4x needs ~28 MB and does not.
//
// An AA colour buffer cannot be sampled as a texture or handed to the scan
// buffer directly - both need a resolve first - so a plain 1x surface sits
// beside it and everything downstream reads that.
static uint32_t sMsaaSamples = 0;          // 0 = off, else 2
static GX2Surface sResolveSurface = {};
static bool sResolveValid = false;

static GX2AAMode gfx_gx2_aa_mode() {
    return sMsaaSamples >= 2 ? GX2_AA_MODE2X : GX2_AA_MODE1X;
}

static GX2Texture* current_fb_texture = nullptr;
static GX2Sampler* current_fb_sampler = nullptr;
static uint32_t sTexLive = 0;   // GX2 textures allocated and not yet freed
static uint32_t sTexBytes = 0;  // and the bytes they hold
static uint32_t sTexUploads = 0; // UploadTexture calls this second
static uint32_t sTexReallocs = 0; // of those, ones that had to reallocate
static OSTime sCpuTicks = 0;      // time inside DrawTriangles submission
static OSTime sPeriodStart = 0;
static OSTime sFrameTicks = 0;   // StartFrame..EndFrame: the whole in-frame CPU cost
static OSTime sFrameT0 = 0;
static int current_tile;

// 96 Mb (should be more than enough to draw everything without waiting for the GPU)
#define DRAW_BUFFER_SIZE 0x1000000 // 16 MB
static uint8_t* draw_buffer = nullptr;
static uint8_t* draw_ptr = nullptr;

static uint32_t frame_count;
static float current_noise_scale;
static FilteringMode current_filter_mode = FILTER_LINEAR;

static BOOL current_depth_test = TRUE;
static BOOL current_depth_write = TRUE;
static GX2CompareFunction current_depth_compare_function = GX2_COMPARE_FUNC_LESS;

static float current_viewport_x = 0.0f;
static float current_viewport_y = 0.0f;
static float current_viewport_width = WIIU_DEFAULT_FB_WIDTH;
static float current_viewport_height = WIIU_DEFAULT_FB_HEIGHT;

static uint32_t current_scissor_x = 0;
static uint32_t current_scissor_y = 0;
static uint32_t current_scissor_width = WIIU_DEFAULT_FB_WIDTH;
static uint32_t current_scissor_height = WIIU_DEFAULT_FB_HEIGHT;

static bool current_zmode_decal = false;
static bool current_SSDB = -2.0f;
static bool current_use_alpha = false;

static inline GX2SamplerVar* GX2GetPixelSamplerVar(const GX2PixelShader* shader, const char* name) {
    for (uint32_t i = 0; i < shader->samplerVarCount; ++i) {
        if (strcmp(name, shader->samplerVars[i].name) == 0) {
            return &shader->samplerVars[i];
        }
    }

    return nullptr;
}

static inline int32_t GX2GetPixelSamplerVarLocation(const GX2PixelShader* shader, const char* name) {
    GX2SamplerVar* sampler = GX2GetPixelSamplerVar(shader, name);
    return sampler ? sampler->location : -1;
}

static inline int32_t GX2GetPixelUniformVarOffset(const GX2PixelShader* shader, const char* name) {
    GX2UniformVar* uniform = GX2GetPixelUniformVar(shader, name);
    return uniform ? uniform->offset : -1;
}

const char* GfxRenderingAPIGX2::GetName() {
    return "GX2";
}

int GfxRenderingAPIGX2::GetMaxTextureSize() {
    // TODO: This should be a define from the Wii U toolchain, but there isn't one yet
    return 8192;
}

// The aux buffer holds the extra samples; without it an AA colour buffer draws
// nothing. Sized by GX2 itself, since the layout is not ours to guess.
static bool gfx_gx2_alloc_aa_aux(GX2ColorBuffer* cb) {
    cb->aaBuffer = nullptr;
    cb->aaSize = 0;
    if (cb->surface.aa == GX2_AA_MODE1X) {
        return true;
    }
    uint32_t size = 0, align = 0;
    GX2CalcColorBufferAuxInfo(cb, &size, &align);
    if (size == 0) {
        return true;
    }
    cb->aaBuffer = gfx_wiiu_alloc_mem1(size, align);
    if (cb->aaBuffer == nullptr) {
        WHBLogPrintf("[gfx_gx2] !! MSAA aux alloc failed (%u bytes) - falling back to 1x", (unsigned)size);
        return false;
    }
    cb->aaSize = size;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, cb->aaBuffer, size);
    WHBLogPrintf("[gfx_gx2] MSAA aux %u bytes, mem1free=%u", (unsigned)size, (unsigned)gfx_wiiu_mem1_free());
    return true;
}

// A 1x surface the AA buffer resolves into, because neither the scan buffer nor
// a texture unit can read multi-sampled data.
static bool gfx_gx2_alloc_resolve(uint32_t width, uint32_t height) {
    sResolveValid = false;
    if (sMsaaSamples < 2) {
        return true;
    }
    memset(&sResolveSurface, 0, sizeof(sResolveSurface));
    sResolveSurface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    sResolveSurface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    sResolveSurface.width = width;
    sResolveSurface.height = height;
    sResolveSurface.depth = 1;
    sResolveSurface.mipLevels = 1;
    sResolveSurface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    sResolveSurface.aa = GX2_AA_MODE1X;
    sResolveSurface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&sResolveSurface);
    sResolveSurface.image = gfx_wiiu_alloc_mem1(sResolveSurface.imageSize, sResolveSurface.alignment);
    if (sResolveSurface.image == nullptr) {
        WHBLogPrintf("[gfx_gx2] !! MSAA resolve surface alloc failed (%u bytes)",
                     (unsigned)sResolveSurface.imageSize);
        return false;
    }
    sResolveValid = true;
    WHBLogPrintf("[gfx_gx2] MSAA resolve surface %u bytes, mem1free=%u",
                 (unsigned)sResolveSurface.imageSize, (unsigned)gfx_wiiu_mem1_free());
    return true;
}

static void gfx_gx2_init_framebuffer(struct Framebuffer* buffer, uint32_t width, uint32_t height) {
    memset(&buffer->color_buffer, 0, sizeof(GX2ColorBuffer));
    buffer->color_buffer.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    buffer->color_buffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    buffer->color_buffer.surface.width = width;
    buffer->color_buffer.surface.height = height;
    buffer->color_buffer.surface.depth = 1;
    buffer->color_buffer.surface.mipLevels = 1;
    buffer->color_buffer.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    // Only framebuffer 0 is multi-sampled: the others are the pause-screen
    // captures, which are read back as textures and would need their own
    // resolve for no visible gain.
    buffer->color_buffer.surface.aa = (buffer == &framebuffers[0]) ? gfx_gx2_aa_mode() : GX2_AA_MODE1X;
    // A multisampled target has to be tiled: linear forces the GPU down a slow
    // path, measured at ~2fps with an otherwise working 2x setup. The resolve
    // surface stays linear, since that is the one read back and presented.
    buffer->color_buffer.surface.tileMode = (buffer->color_buffer.surface.aa == GX2_AA_MODE1X)
                                                ? GX2_TILE_MODE_LINEAR_ALIGNED
                                                : GX2_TILE_MODE_DEFAULT;
    buffer->color_buffer.viewNumSlices = 1;

    memset(&buffer->depth_buffer, 0, sizeof(GX2DepthBuffer));
    buffer->depth_buffer.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER | GX2_SURFACE_USE_TEXTURE;
    buffer->depth_buffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    buffer->depth_buffer.surface.width = width;
    buffer->depth_buffer.surface.height = height;
    buffer->depth_buffer.surface.depth = 1;
    buffer->depth_buffer.surface.mipLevels = 1;
    buffer->depth_buffer.surface.format = GX2_SURFACE_FORMAT_FLOAT_R32;
    buffer->depth_buffer.surface.aa = (buffer == &framebuffers[0]) ? gfx_gx2_aa_mode() : GX2_AA_MODE1X;
    buffer->depth_buffer.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    buffer->depth_buffer.viewNumSlices = 1;
    buffer->depth_buffer.depthClear = 1.0f;
}

// [port] MEM1 is foreground memory. When the system takes the foreground for the
// HOME menu it reclaims MEM1, but the colour, depth and depth-read surfaces live
// there, so anything that keeps drawing writes into memory the app no longer
// owns - which is the hard freeze. ProcUI's release callback has to give these
// back, and its acquire callback has to build them again.
extern "C" void gfx_gx2_release_mem1_surfaces(void) {
    // Framebuffer 0 owns its colour and depth surfaces directly. The secondary
    // ones (the pause background capture) are managed by
    // UpdateFramebufferParameters and may be in MEM1 or the normal heap, so free
    // them the way that function does and clear the recorded size, which is what
    // makes it rebuild them instead of early-returning on a matching size.
    Framebuffer& main_fb = framebuffers[0];
    if (main_fb.color_buffer.surface.image) {
        gfx_wiiu_free_mem1(main_fb.color_buffer.surface.image);
        main_fb.color_buffer.surface.image = nullptr;
    }
    if (main_fb.depth_buffer.surface.image) {
        gfx_wiiu_free_mem1(main_fb.depth_buffer.surface.image);
        main_fb.depth_buffer.surface.image = nullptr;
    }

    for (std::size_t i = 1; i < used_framebuffers; i++) {
        Framebuffer& fb = framebuffers[i];
        if (fb.texture.surface.image) {
            if (fb.colorBufferMem1) {
                gfx_wiiu_free_mem1(fb.texture.surface.image);
            } else {
                free(fb.texture.surface.image);
            }
            fb.texture.surface.image = nullptr;
        }
        if (fb.depth_buffer.surface.image) {
            if (fb.depthBufferMem1) {
                gfx_wiiu_free_mem1(fb.depth_buffer.surface.image);
            } else {
                free(fb.depth_buffer.surface.image);
            }
            fb.depth_buffer.surface.image = nullptr;
        }
        fb.color_buffer.surface.image = nullptr;
        fb.texture.surface.width = 0;
        fb.texture.surface.height = 0;
    }
    if (depthReadBuffer.surface.image) {
        gfx_wiiu_free_mem1(depthReadBuffer.surface.image);
        depthReadBuffer.surface.image = nullptr;
    }
    WHBLogPrintf("[gfx_gx2] released MEM1 surfaces for foreground handover");
}

extern "C" void gfx_gx2_restore_mem1_surfaces(void) {
    // Only framebuffer 0 is rebuilt here; the rest come back through
    // UpdateFramebufferParameters the next time the game asks for them.
    Framebuffer& fb = framebuffers[0];

    GX2CalcSurfaceSizeAndAlignment(&fb.color_buffer.surface);
    GX2InitColorBufferRegs(&fb.color_buffer);
    fb.color_buffer.surface.image =
        gfx_wiiu_alloc_mem1(fb.color_buffer.surface.imageSize, fb.color_buffer.surface.alignment);

    GX2CalcSurfaceSizeAndAlignment(&fb.depth_buffer.surface);
    GX2InitDepthBufferRegs(&fb.depth_buffer);
    fb.depth_buffer.surface.image =
        gfx_wiiu_alloc_mem1(fb.depth_buffer.surface.imageSize, fb.depth_buffer.surface.alignment);

    if (!fb.color_buffer.surface.image || !fb.depth_buffer.surface.image) {
        WHBLogPrintf("[gfx_gx2] !! MEM1 restore FAILED for the main framebuffer");
        return;
    }

    GX2CalcSurfaceSizeAndAlignment(&depthReadBuffer.surface);
    depthReadBuffer.surface.image =
        gfx_wiiu_alloc_mem1(depthReadBuffer.surface.imageSize, depthReadBuffer.surface.alignment);
    if (depthReadBuffer.surface.image) {
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_DEPTH_BUFFER, depthReadBuffer.surface.image,
                      depthReadBuffer.surface.imageSize);
    }

    // [port] Rebuild the secondary framebuffers (pause capture, transition) here
    // too. Their dimensions were zeroed on release so this will not early-return.
    // Leaving them for the interpreter meant they came back with no surface at
    // all, which is why the pause background and the puzzle-piece transition
    // turned black after a handover.
    for (std::size_t i = 1; sActiveApi != nullptr && i < used_framebuffers; i++) {
        Framebuffer& sec = framebuffers[i];
        if (!sec.paramsValid || sec.lastWidth == 0 || sec.lastHeight == 0) {
            continue;
        }
        sActiveApi->UpdateFramebufferParameters(static_cast<int>(i), sec.lastWidth, sec.lastHeight, sec.lastMsaa,
                                                sec.lastInvertY, sec.lastRenderTarget, sec.lastHasDepth,
                                                sec.lastCanExtractDepth);
    }

    GX2SetColorBuffer(&framebuffers[0].color_buffer, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(&framebuffers[0].depth_buffer);
    current_framebuffer = 0;
    WHBLogPrintf("[gfx_gx2] restored MEM1 surfaces after regaining foreground");
}

struct GfxClipParameters GfxRenderingAPIGX2::GetClipParameters(void) {
    // Latte clips to the D3D-style 0 <= z <= w, and the viewport is set with a
    // 0..1 depth range to match, so the interpreter has to emit z in that space.
    //
    // This was reverted to false once before. On its own it changes nothing
    // visible, because z clipping was disabled in StartFrame, so the earlier
    // test could not have shown a difference. It is only meaningful paired with
    // GX2SetRasterizerClipControl(TRUE, TRUE).
    return { true, false };
}

void GfxRenderingAPIGX2::SetUniforms(struct ShaderProgram* prg) {
    float window_params_array[4] = { current_noise_scale, (float)frame_count, mCurrentPrimDepth, 0.0f };
    mPrimDepthDirty = false;

    GX2SetPixelUniformReg(prg->window_params_offset, 4, window_params_array);
}

void GfxRenderingAPIGX2::UnloadShader(struct ShaderProgram* old_prg) {
    current_shader_program = nullptr;
}

// [port] A shader may sample a texture unit that the interpreter never bound
// anything to (a two-cycle combiner using TEXEL1 while only tile 0 was loaded).
// A desktop GL driver returns black for an unbound sampler, but GX2 leaves
// whatever texture descriptor was last written to that unit, so the GPU fetches
// from arbitrary memory and paints saturated garbage across the triangle. Bind a
// known 1x1 opaque white texel instead, which is also the combiner identity.
static GX2Texture dummy_texture;
static GX2Sampler dummy_sampler;
static bool dummy_texture_ready = false;

static void gfx_gx2_init_dummy_texture(void) {
    if (dummy_texture_ready) {
        return;
    }

    memset(&dummy_texture, 0, sizeof(dummy_texture));
    dummy_texture.surface.use = GX2_SURFACE_USE_TEXTURE;
    dummy_texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    dummy_texture.surface.width = 1;
    dummy_texture.surface.height = 1;
    dummy_texture.surface.depth = 1;
    dummy_texture.surface.mipLevels = 1;
    dummy_texture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    dummy_texture.surface.aa = GX2_AA_MODE1X;
    dummy_texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    dummy_texture.viewFirstMip = 0;
    dummy_texture.viewNumMips = 1;
    dummy_texture.viewFirstSlice = 0;
    dummy_texture.viewNumSlices = 1;
    dummy_texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

    GX2CalcSurfaceSizeAndAlignment(&dummy_texture.surface);
    GX2InitTextureRegs(&dummy_texture);

    dummy_texture.surface.image = memalign(dummy_texture.surface.alignment, dummy_texture.surface.imageSize);
    if (dummy_texture.surface.image == nullptr) {
        WHBLogPrintf("[gfx_gx2] !! dummy texture alloc FAILED size=%u", (unsigned)dummy_texture.surface.imageSize);
        return;
    }
    memset(dummy_texture.surface.image, 0xFF, dummy_texture.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, dummy_texture.surface.image, dummy_texture.surface.imageSize);

    // Linear: this backs a 1x1 white texel where filtering is irrelevant, and it
    // is also the fallback for any unbound unit, where point sampling would be a
    // silent downgrade.
    GX2InitSampler(&dummy_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);

    dummy_texture_ready = true;
    WHBLogPrintf("[gfx_gx2] dummy 1x1 texture ready (size=%u)", (unsigned)dummy_texture.surface.imageSize);
}

static void gfx_gx2_bind_textures(struct ShaderProgram* prg) {
    if (prg == nullptr) {
        return;
    }
    for (int tile = 0; tile < SHADER_MAX_TEXTURES; tile++) {
        struct GX2TextureObj* tex = current_textures[tile];
        const int32_t location = prg->samplers_location[tile];
        if (location == -1) {
            continue;
        }
        if (tile == 0 && current_fb_texture != nullptr) {
            GX2SetPixelTexture(current_fb_texture, location);
            GX2SetPixelSampler(current_fb_sampler, location);
            continue;
        }
        // The shader samples this unit. Never leave it holding a stale descriptor.
        if (tex == nullptr || !tex->texture_uploaded) {
            if (dummy_texture_ready) {
                GX2SetPixelTexture(&dummy_texture, location);
                GX2SetPixelSampler(&dummy_sampler, location);
            }
            continue;
        }
        GX2SetPixelTexture(&tex->texture, location);
        if (tex->sampler_set) {
            GX2SetPixelSampler(&tex->sampler, location);
        } else {
            GX2SetPixelSampler(&dummy_sampler, location);
        }
    }
}

void GfxRenderingAPIGX2::LoadShader(struct ShaderProgram* new_prg) {
    current_shader_program = new_prg;

    GX2SetFetchShader(&new_prg->group.fetchShader);
    GX2SetVertexShader(&new_prg->group.vertexShader);
    GX2SetPixelShader(&new_prg->group.pixelShader);

    SetUniforms(new_prg);
    gfx_gx2_bind_textures(new_prg);
}

struct ShaderProgram* GfxRenderingAPIGX2::CreateAndLoadNewShader(uint64_t shader_id0, uint64_t shader_id1) {
    struct CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    struct ShaderProgram* prg = &shader_program_pool[std::make_pair(shader_id0, shader_id1)];

    printf("Generating shader: %016llx-%016llx\n", (unsigned long long)shader_id0, (unsigned long long)shader_id1);
    WHBLogPrintf("[gfx_gx2] shader gen id0=%016llx id1=%016llx", (unsigned long long)shader_id0, (unsigned long long)shader_id1);
    if (gx2GenerateShaderGroup(&prg->group, &cc_features) != 0) {
        printf("Failed to generate shader\n");
        current_shader_program = nullptr;
        return nullptr;
    }

    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    prg->numInputs = cc_features.numInputs;
    prg->usedTextures[0] = cc_features.usedTextures[0];
    prg->usedTextures[1] = cc_features.usedTextures[1];

    LoadShader(prg);

    prg->window_params_offset = GX2GetPixelUniformVarOffset(&prg->group.pixelShader, "window_params");
    prg->samplers_location[0] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTex0");
    prg->samplers_location[1] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTex1");
    prg->samplers_location[2] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTexMask0");
    prg->samplers_location[3] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTexMask1");
    prg->samplers_location[4] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTexBlend0");
    prg->samplers_location[5] = GX2GetPixelSamplerVarLocation(&prg->group.pixelShader, "uTexBlend1");

    prg->used_noise = cc_features.opt_alpha && cc_features.opt_noise;

    printf("Generated and loaded shader\n");

    return prg;
}

struct ShaderProgram* GfxRenderingAPIGX2::LookupShader(uint64_t shader_id0, uint64_t shader_id1) {
    auto it = shader_program_pool.find(std::make_pair(shader_id0, shader_id1));
    return it == shader_program_pool.end() ? nullptr : &it->second;
}

void GfxRenderingAPIGX2::ShaderGetInfo(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = prg->numInputs;
    used_textures[0] = prg->usedTextures[0];
    used_textures[1] = prg->usedTextures[1];
}

uint32_t GfxRenderingAPIGX2::NewTexture(void) {
    struct GX2TextureObj* tex = (struct GX2TextureObj*)calloc(1, sizeof(struct GX2TextureObj));
    if (tex == nullptr) {
        WHBLogPrintf("[gfx_gx2] !! NewTexture calloc failed");
        return 0;
    }

    tex->imtex.Texture = &tex->texture;
    tex->imtex.Sampler = &tex->sampler;

    // [port] calloc leaves sampler_set false, and the unbound-sampler fallback
    // then binds the point-sampled dummy on every LoadShader - overriding the
    // bilinear the display list asked for. Give every texture a real sampler up
    // front; SetSamplerParameters refines it per draw.
    GX2InitSampler(&tex->sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    tex->sampler_set = true;

    // some 32-bit trickery :P
    return (uint32_t)tex;
}

void GfxRenderingAPIGX2::DeleteTexture(uint32_t texture_id) {
    struct GX2TextureObj* tex = (struct GX2TextureObj*)texture_id;

    for (int tile = 0; tile < SHADER_MAX_TEXTURES; tile++) {
        if (current_textures[tile] == tex) {
            current_textures[tile] = nullptr;
        }
    }
    if (current_texture == tex) {
        current_texture = nullptr;
    }

    if (tex->texture.surface.image) {
        free(tex->texture.surface.image);
    }

    free((void*)tex);
}

void GfxRenderingAPIGX2::SelectTexture(int tile, uint32_t texture_id) {
    struct GX2TextureObj* tex = (struct GX2TextureObj*)texture_id;
    current_texture = tex;
    current_tile = tile;
    if (tile == 0) {
        // An ordinary texture reclaims sampler 0 from any framebuffer binding.
        current_fb_texture = nullptr;
        current_fb_sampler = nullptr;
    }
    if (tile >= 0 && tile < SHADER_MAX_TEXTURES) {
        current_textures[tile] = tex;
    }

    if (current_shader_program) {
        int32_t sampler_location = current_shader_program->samplers_location[tile];
        if (sampler_location != -1) {
            if (tex->texture_uploaded) {
                GX2SetPixelTexture(&tex->texture, sampler_location);
            }

            if (tex->sampler_set) {
                GX2SetPixelSampler(&tex->sampler, sampler_location);
            }
        }
    }
}

void GfxRenderingAPIGX2::UploadTexture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    struct GX2TextureObj* tex = current_texture;
    assert(tex);

    if ((tex->texture.surface.width != width) || (tex->texture.surface.height != height) ||
        !tex->texture.surface.image) {

        if (tex->texture.surface.image) {
            free(tex->texture.surface.image);
            tex->texture.surface.image = nullptr;
        }

        memset(&tex->texture, 0, sizeof(GX2Texture));
        tex->texture.surface.use = GX2_SURFACE_USE_TEXTURE;
        tex->texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
        tex->texture.surface.width = width;
        tex->texture.surface.height = height;
        tex->texture.surface.depth = 1;
        tex->texture.surface.mipLevels = 1;
        tex->texture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
        tex->texture.surface.aa = GX2_AA_MODE1X;
        tex->texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
        tex->texture.viewFirstMip = 0;
        tex->texture.viewNumMips = 1;
        tex->texture.viewFirstSlice = 0;
        tex->texture.viewNumSlices = 1;
        tex->texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

        GX2CalcSurfaceSizeAndAlignment(&tex->texture.surface);
        GX2InitTextureRegs(&tex->texture);

        tex->texture.surface.image = memalign(tex->texture.surface.alignment, tex->texture.surface.imageSize);

        if (tex->texture.surface.image == nullptr) {
            WHBLogPrintf("[gfx_gx2] !! texture alloc FAILED %ux%u size=%u", (unsigned)width, (unsigned)height,
                         (unsigned)tex->texture.surface.imageSize);
            return;
        }
        // Writing pitch*4 bytes per row for `height` rows has to fit inside the
        // surface GX2 sized for us, or the copy below walks off the allocation
        // and corrupts the heap.
        const uint32_t needed = tex->texture.surface.pitch * 4 * height;
        if (needed > tex->texture.surface.imageSize) {
            WHBLogPrintf("[gfx_gx2] !! texture OVERFLOW %ux%u pitch=%u needs=%u have=%u", (unsigned)width,
                         (unsigned)height, (unsigned)tex->texture.surface.pitch, (unsigned)needed,
                         (unsigned)tex->texture.surface.imageSize);
        }
    }

    uint8_t* buf = (uint8_t*)tex->texture.surface.image;
    assert(buf);

    // Clamp to what GX2 actually sized the surface for. If the two ever
    // disagree, lose the bottom of a texture rather than the heap.
    const uint32_t rowStride = tex->texture.surface.pitch * 4;
    const uint32_t maxRows = rowStride ? (tex->texture.surface.imageSize / rowStride) : 0;
    const uint32_t rows = height < maxRows ? height : maxRows;
    const uint32_t rowBytes = (width * 4) < rowStride ? (width * 4) : rowStride;
    for (uint32_t y = 0; y < rows; ++y) {
        memcpy(buf + (y * rowStride), rgba32_buf + (y * width * 4), rowBytes);
    }

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, tex->texture.surface.image, tex->texture.surface.imageSize);

    if (current_shader_program && current_shader_program->samplers_location[current_tile] != -1) {
        GX2SetPixelTexture(&tex->texture, current_shader_program->samplers_location[current_tile]);
    }

    tex->texture_uploaded = true;
}

static GX2TexClampMode gfx_cm_to_gx2(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return GX2_TEX_CLAMP_MODE_CLAMP;
        case G_TX_MIRROR | G_TX_WRAP:
            return GX2_TEX_CLAMP_MODE_MIRROR;
        case G_TX_MIRROR | G_TX_CLAMP:
            return GX2_TEX_CLAMP_MODE_MIRROR_ONCE;
        case G_TX_NOMIRROR | G_TX_WRAP:
            return GX2_TEX_CLAMP_MODE_WRAP;
    }

    return GX2_TEX_CLAMP_MODE_WRAP;
}

void GfxRenderingAPIGX2::SetSamplerParameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    // [port] This used current_texture - whichever was selected last - so on a
    // two-tile draw the filter landed on the wrong texture object and the tile
    // that asked for it kept its old sampler.
    struct GX2TextureObj* tex =
        (tile >= 0 && tile < SHADER_MAX_TEXTURES && current_textures[tile] != nullptr) ? current_textures[tile]
                                                                                      : current_texture;
    assert(tex);

    current_tile = tile;

    GX2InitSampler(&tex->sampler, GX2_TEX_CLAMP_MODE_CLAMP,
                   (linear_filter && current_filter_mode == FILTER_LINEAR) ? GX2_TEX_XY_FILTER_MODE_LINEAR
                                                                           : GX2_TEX_XY_FILTER_MODE_POINT);

    GX2InitSamplerClamping(&tex->sampler, gfx_cm_to_gx2(cms), gfx_cm_to_gx2(cmt), GX2_TEX_CLAMP_MODE_WRAP);


    if (current_shader_program && current_shader_program->samplers_location[tile] != -1) {
        GX2SetPixelSampler(&tex->sampler, current_shader_program->samplers_location[tile]);
    }

    tex->sampler_set = true;
}

void GfxRenderingAPIGX2::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    current_depth_test = depth_test || z_upd;
    current_depth_write = z_upd;
    current_depth_compare_function = depth_test ? GX2_COMPARE_FUNC_LEQUAL : GX2_COMPARE_FUNC_ALWAYS;

    GX2SetDepthOnlyControl(current_depth_test, current_depth_write, current_depth_compare_function);
}

void GfxRenderingAPIGX2::SetZmodeDecal(bool zmode_decal) {
    current_zmode_decal = zmode_decal;
    if (zmode_decal) {
        // SSDB = SlopeScaledDepthBias 120 leads to -2 at 240p which is the same as N64 mode which has very little
        // fighting
        const int n64modeFactor = 120;
        const int noVanishFactor = 100;
        float SSDB = -2.0f;
        switch (CVarGetInteger("gZFightingMode", 0)) {
            // scaled z-fighting (N64 mode like)
            case 1:
                if (current_framebuffer < used_framebuffers) {
                    SSDB = -1.0f * (float)framebuffers[current_framebuffer].color_buffer.surface.height / n64modeFactor;
                }
                break;
            // no vanishing paths
            case 2:
                if (current_framebuffer < used_framebuffers) {
                    SSDB = -1.0f * (float)framebuffers[current_framebuffer].color_buffer.surface.height / noVanishFactor;
                }
                break;
            // disabled
            case 0:
            default:
                SSDB = -2.0f;
        }

        current_SSDB = SSDB;
        GX2SetPolygonOffset(SSDB, SSDB, SSDB, SSDB, 0.0f);
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, TRUE, GX2_POLYGON_MODE_TRIANGLE,
                             GX2_POLYGON_MODE_TRIANGLE, TRUE, TRUE, FALSE);
    } else {
        GX2SetPolygonOffset(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
                             GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
    }
}

void GfxRenderingAPIGX2::SetViewport(int x, int y, int width, int height) {
    Framebuffer& buffer = framebuffers[current_framebuffer];
    uint32_t buffer_height = buffer.color_buffer.surface.height;

    current_viewport_x = x;
    current_viewport_y = buffer_height - y - height;
    current_viewport_width = width;
    current_viewport_height = height;


    GX2SetViewport(current_viewport_x, current_viewport_y, current_viewport_width, current_viewport_height, 0.0f, 1.0f);
}

void GfxRenderingAPIGX2::SetScissor(int x, int y, int width, int height) {
    Framebuffer& buffer = framebuffers[current_framebuffer];
    uint32_t buffer_height = buffer.color_buffer.surface.height;
    uint32_t buffer_width = buffer.color_buffer.surface.width;

    // [port] GX2's origin is top-left, so y flips exactly as it does in
    // SetViewport. The previous code clamped x against width and y against
    // height, which is not a bounds check: any scissor shorter than its flipped
    // y position was placed at the wrong height, and any rect whose x exceeded
    // its own width was pulled back to the width. Clamp against the buffer.
    int32_t sx = x;
    int32_t sy = (int32_t)buffer_height - y - height;
    int32_t sw = width;
    int32_t sh = height;

    if (sx < 0) {
        sw += sx;
        sx = 0;
    }
    if (sy < 0) {
        sh += sy;
        sy = 0;
    }
    if (sw > (int32_t)buffer_width - sx) {
        sw = (int32_t)buffer_width - sx;
    }
    if (sh > (int32_t)buffer_height - sy) {
        sh = (int32_t)buffer_height - sy;
    }
    if (sw < 0) {
        sw = 0;
    }
    if (sh < 0) {
        sh = 0;
    }

    current_scissor_x = (uint32_t)sx;
    current_scissor_y = (uint32_t)sy;
    current_scissor_width = (uint32_t)sw;
    current_scissor_height = (uint32_t)sh;


    GX2SetScissor(current_scissor_x, current_scissor_y, current_scissor_width, current_scissor_height);
}

void GfxRenderingAPIGX2::SetUseAlpha(bool use_alpha) {
    current_use_alpha = use_alpha;
    GX2SetColorControl(GX2_LOGIC_OP_COPY, use_alpha ? 0xff : 0, FALSE, TRUE);
}

static uint32_t sDrawCalls = 0;
static uint32_t sDrawTris = 0;
static uint32_t sDrawWraps = 0;

void GfxRenderingAPIGX2::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    sDrawCalls++;
    sDrawTris += (uint32_t)buf_vbo_num_tris;

    if (!current_shader_program) {
        return;
    }

    size_t vbo_len = sizeof(float) * buf_vbo_len;

    // [port] The interpreter packs a variable number of floats per vertex
    // depending on the shader variant, and the attribute layout generated for
    // that variant must agree exactly. If it does not, the GPU reads positions
    // and colors out of the wrong floats, which draws screen-sized triangles in
    // clamped primary colors. Verify rather than assume.
    if (buf_vbo_num_tris > 0) {
        const size_t packedStride = (buf_vbo_len / (3 * buf_vbo_num_tris)) * sizeof(float);
        if (packedStride != current_shader_program->group.stride) {
            static uint32_t reported = 0;
            if (reported++ < 8) {
                WHBLogPrintf("[gfx_gx2] STRIDE MISMATCH id0=%016llx id1=%016llx: packed %u bytes/vtx, layout says %u",
                             (unsigned long long)current_shader_program->shader_id0,
                             (unsigned long long)current_shader_program->shader_id1, (unsigned)packedStride,
                             (unsigned)current_shader_program->group.stride);
            }
        }
    }

    if (draw_ptr + vbo_len >= draw_buffer + DRAW_BUFFER_SIZE) {
        // Genuinely out of room: the GPU must finish before we reuse the ring.
        sDrawWraps++;
        GX2DrawDone();
        draw_ptr = draw_buffer;
    }

    float* new_vbo = (float*)draw_ptr;
    draw_ptr += ALIGN(vbo_len, GX2_VERTEX_BUFFER_ALIGNMENT);

    OSBlockMove(new_vbo, buf_vbo, vbo_len, FALSE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, new_vbo, vbo_len);

    GX2SetAttribBuffer(0, vbo_len, current_shader_program->group.stride, new_vbo);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3 * buf_vbo_num_tris, 0, 1);
}

void GfxRenderingAPIGX2::Init(void) {
    sActiveApi = this;
    WHBLogPrintf("[gfx_gx2] Init entered");
    // Init the default framebuffer
    used_framebuffers = 1;
    Framebuffer& main_framebuffer = framebuffers[0];

    // Read once at init: the surfaces are built here and changing samples later
    // would mean rebuilding all of MEM1, so the menu toggle takes effect on the
    // next launch and says so.
    sMsaaSamples = (uint32_t)CVarGetInteger(CVAR_PREFIX_SETTING ".Graphics.MSAA", 0);
    if (sMsaaSamples != 0 && sMsaaSamples != 2) {
        sMsaaSamples = 2;   // only 2x fits MEM1 at 720p
    }
    WHBLogPrintf("[gfx_gx2] MSAA %ux", (unsigned)(sMsaaSamples ? sMsaaSamples : 1));

    WHBLogPrintf("[gfx_gx2] -> main framebuffer");
    gfx_gx2_init_framebuffer(&main_framebuffer, WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT);

    GX2CalcSurfaceSizeAndAlignment(&main_framebuffer.color_buffer.surface);
    GX2InitColorBufferRegs(&main_framebuffer.color_buffer);

    WHBLogPrintf("[gfx_gx2] color: size=%u align=%u mem1free=%u", (unsigned)main_framebuffer.color_buffer.surface.imageSize,
                 (unsigned)main_framebuffer.color_buffer.surface.alignment, (unsigned)gfx_wiiu_mem1_free());
    main_framebuffer.color_buffer.surface.image = gfx_wiiu_alloc_mem1(main_framebuffer.color_buffer.surface.imageSize,
                                                                      main_framebuffer.color_buffer.surface.alignment);
    WHBLogPrintf("[gfx_gx2] color image=%p", main_framebuffer.color_buffer.surface.image);
    assert(main_framebuffer.color_buffer.surface.image);

    // If either MSAA allocation fails, fall back rather than render nothing.
    if (sMsaaSamples >= 2) {
        if (!gfx_gx2_alloc_aa_aux(&main_framebuffer.color_buffer) ||
            !gfx_gx2_alloc_resolve(WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT)) {
            sMsaaSamples = 0;
            main_framebuffer.color_buffer.surface.aa = GX2_AA_MODE1X;
            main_framebuffer.depth_buffer.surface.aa = GX2_AA_MODE1X;
            main_framebuffer.color_buffer.aaBuffer = nullptr;
            main_framebuffer.color_buffer.aaSize = 0;
            GX2CalcSurfaceSizeAndAlignment(&main_framebuffer.color_buffer.surface);
            GX2InitColorBufferRegs(&main_framebuffer.color_buffer);
        }
    }

    GX2CalcSurfaceSizeAndAlignment(&main_framebuffer.depth_buffer.surface);
    GX2InitDepthBufferRegs(&main_framebuffer.depth_buffer);

    WHBLogPrintf("[gfx_gx2] depth: size=%u align=%u mem1free=%u", (unsigned)main_framebuffer.depth_buffer.surface.imageSize,
                 (unsigned)main_framebuffer.depth_buffer.surface.alignment, (unsigned)gfx_wiiu_mem1_free());
    main_framebuffer.depth_buffer.surface.image = gfx_wiiu_alloc_mem1(main_framebuffer.depth_buffer.surface.imageSize,
                                                                      main_framebuffer.depth_buffer.surface.alignment);
    WHBLogPrintf("[gfx_gx2] depth image=%p", main_framebuffer.depth_buffer.surface.image);
    assert(main_framebuffer.depth_buffer.surface.image);

    main_framebuffer.imtex.Texture = &main_framebuffer.texture;
    main_framebuffer.imtex.Sampler = &main_framebuffer.sampler;

    // create a linear aligned copy of the depth buffer to read pixels to
    memcpy(&depthReadBuffer, &main_framebuffer.depth_buffer, sizeof(GX2DepthBuffer));

    depthReadBuffer.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    depthReadBuffer.surface.width = 32;
    depthReadBuffer.surface.height = 1;

    GX2CalcSurfaceSizeAndAlignment(&depthReadBuffer.surface);

    WHBLogPrintf("[gfx_gx2] depthread: size=%u align=%u mem1free=%u", (unsigned)depthReadBuffer.surface.imageSize,
                 (unsigned)depthReadBuffer.surface.alignment, (unsigned)gfx_wiiu_mem1_free());
    depthReadBuffer.surface.image =
        gfx_wiiu_alloc_mem1(depthReadBuffer.surface.imageSize, depthReadBuffer.surface.alignment);
    WHBLogPrintf("[gfx_gx2] depthread image=%p", depthReadBuffer.surface.image);
    assert(depthReadBuffer.surface.image);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_DEPTH_BUFFER, depthReadBuffer.surface.image,
                  depthReadBuffer.surface.imageSize);

    WHBLogPrintf("[gfx_gx2] -> set color/depth buffer");
    GX2SetColorBuffer(&main_framebuffer.color_buffer, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(&main_framebuffer.depth_buffer);

    current_framebuffer = 0;

    // allocate draw buffer
    WHBLogPrintf("[gfx_gx2] -> draw buffer alloc");
    draw_buffer = (uint8_t*)memalign(GX2_VERTEX_BUFFER_ALIGNMENT, DRAW_BUFFER_SIZE);
    WHBLogPrintf("[gfx_gx2] draw_buffer=%p size=%u", draw_buffer, (unsigned)DRAW_BUFFER_SIZE);
    gfx_gx2_init_dummy_texture();
    assert(draw_buffer);
    draw_ptr = draw_buffer;

    WHBLogPrintf("[gfx_gx2] -> rasterizer clip control (z clip ENABLED)");
    // [port] Z clipping was disabled here. Fast3D deliberately does not clip
    // triangles that straddle the near plane, it leaves that to the GPU, so with
    // this off such a triangle is rasterised from its extrapolated coordinates
    // and smears across the whole screen. Vertices arriving with w=5 against a
    // ~311 translation baseline are exactly that case.
    //
    // This only works together with the 0..1 clip space reported by
    // GetClipParameters(): enabling clipping while the interpreter emits
    // OpenGL-convention z in [-w,w] would clip away everything in the near half
    // of the range, 2D elements at z=-1 included. Change the two as a pair.
    GX2SetRasterizerClipControl(TRUE, TRUE);

    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD);

    WHBLogPrintf("[gfx_gx2] -> GX2Util::Init");
    GX2Util::Init();
    WHBLogPrintf("[gfx_gx2] -> set context state");
    gfx_wiiu_set_context_state();
    WHBLogPrintf("[gfx_gx2] Init complete");
}

void gfx_gx2_shutdown(void) {
    if (has_foreground) {
        GX2DrawDone();

        if (depthReadBuffer.surface.image) {
            gfx_wiiu_free_mem1(depthReadBuffer.surface.image);
            depthReadBuffer.surface.image = nullptr;
        }

        for (auto& buffer : framebuffers) {
            if (buffer.texture.surface.image) {
                if (buffer.colorBufferMem1) {
                    gfx_wiiu_free_mem1(buffer.texture.surface.image);
                } else {
                    free(buffer.texture.surface.image);
                }
                buffer.texture.surface.image = nullptr;
            }

            if (buffer.depth_buffer.surface.image) {
                if (buffer.depthBufferMem1) {
                    gfx_wiiu_free_mem1(buffer.depth_buffer.surface.image);
                } else {
                    free(buffer.depth_buffer.surface.image);
                }
                buffer.depth_buffer.surface.image = nullptr;
            }
        }
    }

    if (draw_buffer) {
        free(draw_buffer);
        draw_buffer = nullptr;
        draw_ptr = nullptr;
    }

    GX2Util::Shutdown();
}

void GfxRenderingAPIGX2::OnResize(void) {
}

void GfxRenderingAPIGX2::StartFrame(void) {
    // Restore state since ImGui modified it when rendering
    GX2SetViewport(current_viewport_x, current_viewport_y, current_viewport_width, current_viewport_height, 0.0f, 1.0f);
    GX2SetScissor(current_scissor_x, current_scissor_y, current_scissor_width, current_scissor_height);

    GX2SetColorControl(GX2_LOGIC_OP_COPY, current_use_alpha ? 0xff : 0, FALSE, TRUE);

    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                       GX2_BLEND_COMBINE_MODE_ADD, FALSE, GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ZERO,
                       GX2_BLEND_COMBINE_MODE_ADD);

    GX2SetDepthOnlyControl(current_depth_test, current_depth_write, current_depth_compare_function);

    if (current_zmode_decal) {
        GX2SetPolygonOffset(current_SSDB, current_SSDB, current_SSDB, current_SSDB, 0.0f);
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, TRUE, GX2_POLYGON_MODE_TRIANGLE,
                             GX2_POLYGON_MODE_TRIANGLE, TRUE, TRUE, FALSE);
    } else {
        GX2SetPolygonOffset(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
                             GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
    }

    frame_count++;
}

void GfxRenderingAPIGX2::EndFrame(void) {
    sDrawCalls = 0;
    sDrawTris = 0;

    // [port] This used to rewind draw_ptr to the start of the buffer every
    // frame with no GPU synchronisation, so the CPU began overwriting vertex
    // data the GPU was still reading for the previous frame. The interpreter
    // output is correct - every triangle it submits sits inside [-1,1] - but
    // the GPU saw half of it replaced mid-draw, which produced the triangles
    // sprawling across the screen, and is why the image became correct the
    // instant the app stopped drawing at shutdown.
    //
    // Use the allocation as a real ring: wrap only when genuinely full, where
    // DrawTriangles already waits on GX2DrawDone(). At ~200KB a frame a 16MB
    // buffer wraps roughly every 80 frames, so the stall is rare.

    Framebuffer& main_framebuffer = framebuffers[0];

    // Multi-sampled data cannot go straight to a scan buffer: resolve it down
    // first and present that instead.
    const GX2ColorBuffer* present = &main_framebuffer.color_buffer;
    GX2ColorBuffer resolved;
    if (sMsaaSamples >= 2 && sResolveValid) {
        GX2ResolveAAColorBuffer(&main_framebuffer.color_buffer, &sResolveSurface, 0, 0);
        resolved = main_framebuffer.color_buffer;
        resolved.surface = sResolveSurface;
        resolved.aaBuffer = nullptr;
        resolved.aaSize = 0;
        GX2InitColorBufferRegs(&resolved);
        present = &resolved;
    }
    GX2CopyColorBufferToScanBuffer(present, GX2_SCAN_TARGET_TV);
    GX2CopyColorBufferToScanBuffer(present, GX2_SCAN_TARGET_DRC);
}

void GfxRenderingAPIGX2::FinishRender(void) {
}

int GfxRenderingAPIGX2::CreateFramebuffer(void) {
    assert(used_framebuffers < framebuffers.size());

    std::size_t i = used_framebuffers;
    used_framebuffers++;

    Framebuffer& buffer = framebuffers[i];

    GX2InitSampler(&buffer.sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_LINEAR);

    buffer.imtex.Texture = &buffer.texture;
    buffer.imtex.Sampler = &buffer.sampler;

    return i;
}

void GfxRenderingAPIGX2::UpdateFramebufferParameters(int fb, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                  bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                                  bool can_extract_depth) {
    // we don't support updating the main buffer (fb 0)
    if (fb == 0) {
        return;
    }

    Framebuffer& buffer = framebuffers[fb];

    buffer.paramsValid = true;
    buffer.lastWidth = width;
    buffer.lastHeight = height;
    buffer.lastMsaa = msaa_level;
    buffer.lastInvertY = opengl_invert_y;
    buffer.lastRenderTarget = render_target;
    buffer.lastHasDepth = has_depth_buffer;
    buffer.lastCanExtractDepth = can_extract_depth;

    if (buffer.texture.surface.width == width && buffer.texture.surface.height == height) {
        return;
    }

    // [port] The pause menu draws the frozen scene from one of these. It is
    // arriving blocky, so report the size the game actually asks for against the
    // main framebuffer it is captured from.
    WHBLogPrintf("[fb] update fb%d -> %ux%u (main is %ux%u) msaa=%u depth=%d", fb, (unsigned)width, (unsigned)height,
                 (unsigned)framebuffers[0].color_buffer.surface.width,
                 (unsigned)framebuffers[0].color_buffer.surface.height, (unsigned)msaa_level, (int)has_depth_buffer);

    // make sure the GPU no longer writes to the buffer
    GX2DrawDone();

    if (buffer.texture.surface.image) {
        if (buffer.colorBufferMem1) {
            gfx_wiiu_free_mem1(buffer.texture.surface.image);
        } else {
            free(buffer.texture.surface.image);
        }
        buffer.texture.surface.image = nullptr;
    }

    if (buffer.depth_buffer.surface.image) {
        if (buffer.depthBufferMem1) {
            gfx_wiiu_free_mem1(buffer.depth_buffer.surface.image);
        } else {
            free(buffer.depth_buffer.surface.image);
        }
        buffer.depth_buffer.surface.image = nullptr;
    }

    gfx_gx2_init_framebuffer(&buffer, width, height);

    GX2CalcSurfaceSizeAndAlignment(&buffer.depth_buffer.surface);
    GX2InitDepthBufferRegs(&buffer.depth_buffer);

    buffer.depth_buffer.surface.image =
        gfx_wiiu_alloc_mem1(buffer.depth_buffer.surface.imageSize, buffer.depth_buffer.surface.alignment);
    // fall back to mem2
    if (!buffer.depth_buffer.surface.image) {
        buffer.depth_buffer.surface.image =
            memalign(buffer.depth_buffer.surface.alignment, buffer.depth_buffer.surface.imageSize);
        buffer.depthBufferMem1 = false;
    } else {
        buffer.depthBufferMem1 = true;
    }
    assert(buffer.depth_buffer.surface.image);

    GX2CalcSurfaceSizeAndAlignment(&buffer.color_buffer.surface);
    GX2InitColorBufferRegs(&buffer.color_buffer);

    memset(&buffer.texture, 0, sizeof(GX2Texture));
    buffer.texture.surface.use = GX2_SURFACE_USE_TEXTURE;
    buffer.texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    buffer.texture.surface.width = width;
    buffer.texture.surface.height = height;
    buffer.texture.surface.depth = 1;
    buffer.texture.surface.mipLevels = 1;
    buffer.texture.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    buffer.texture.surface.aa = GX2_AA_MODE1X;
    buffer.texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    buffer.texture.viewFirstMip = 0;
    buffer.texture.viewNumMips = 1;
    buffer.texture.viewFirstSlice = 0;
    buffer.texture.viewNumSlices = 1;
    buffer.texture.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);

    GX2CalcSurfaceSizeAndAlignment(&buffer.texture.surface);
    GX2InitTextureRegs(&buffer.texture);

    // the texture and color buffer share a buffer
    assert(buffer.color_buffer.surface.imageSize == buffer.texture.surface.imageSize);

    buffer.texture.surface.image =
        gfx_wiiu_alloc_mem1(buffer.texture.surface.imageSize, buffer.texture.surface.alignment);
    // fall back to mem2
    if (!buffer.texture.surface.image) {
        buffer.texture.surface.image = memalign(buffer.texture.surface.alignment, buffer.texture.surface.imageSize);
        buffer.colorBufferMem1 = false;
    } else {
        buffer.colorBufferMem1 = true;
    }
    assert(buffer.texture.surface.image);

    buffer.color_buffer.surface.image = buffer.texture.surface.image;
}

void GfxRenderingAPIGX2::StartDrawToFramebuffer(int fb, float noise_scale) {
    Framebuffer& buffer = framebuffers[fb];

    if (noise_scale != 0.0f) {
        current_noise_scale = 1.0f / noise_scale;
    }

    GX2SetColorBuffer(&buffer.color_buffer, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(&buffer.depth_buffer);

    current_framebuffer = fb;
}

void GfxRenderingAPIGX2::ClearFramebuffer(bool color, bool depth) {
    Framebuffer& buffer = framebuffers[current_framebuffer];

    if (color) {
        GX2ClearColor(&buffer.color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
    }

    if (depth) {
        GX2ClearDepthStencilEx(&buffer.depth_buffer, buffer.depth_buffer.depthClear, buffer.depth_buffer.stencilClear,
                            GX2_CLEAR_FLAGS_BOTH);
    }

    gfx_wiiu_set_context_state();
}

void GfxRenderingAPIGX2::ResolveMSAAColorBuffer(int fb_id_target, int fb_id_source) {
    Framebuffer& src_buffer = framebuffers[fb_id_source];
    Framebuffer& target_buffer = framebuffers[fb_id_target];

    if (src_buffer.color_buffer.surface.aa == GX2_AA_MODE1X) {
        GX2CopySurface(&src_buffer.color_buffer.surface, src_buffer.color_buffer.viewMip,
                       src_buffer.color_buffer.viewFirstSlice, &target_buffer.color_buffer.surface,
                       target_buffer.color_buffer.viewMip, target_buffer.color_buffer.viewFirstSlice);
    } else {
        GX2ResolveAAColorBuffer(&src_buffer.color_buffer, &target_buffer.color_buffer.surface,
                                target_buffer.color_buffer.viewMip, target_buffer.color_buffer.viewFirstSlice);
    }
}

void* GfxRenderingAPIGX2::GetFramebufferTextureId(int fb_id) {
    Framebuffer& buffer = framebuffers[fb_id];

    return &buffer.imtex;
}

void GfxRenderingAPIGX2::SelectTextureFb(int fb) {
    Framebuffer& buffer = framebuffers[fb];

    assert(current_shader_program);

    // [port] The texture and the colour buffer share one allocation, but the GPU
    // wrote it through the colour path and is about to read it through the
    // texture path. Without invalidating, the texture unit keeps serving what it
    // cached earlier - which is why the pause and game over screens showed a
    // stale, blocky image instead of the scene that was just rendered.
    if (buffer.texture.surface.image != nullptr) {
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE, buffer.texture.surface.image,
                      buffer.texture.surface.imageSize);
    }

    current_fb_texture = &buffer.texture;
    current_fb_sampler = &buffer.sampler;

    uint32_t location = current_shader_program->samplers_location[0];
    GX2SetPixelTexture(&buffer.texture, location);
    GX2SetPixelSampler(&buffer.sampler, location);
}

void GfxRenderingAPIGX2::CopyFramebuffer(int fb_dst_id, int fb_src_id, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0,
                              int dstY0, int dstX1, int dstY1) {
    if (fb_dst_id >= used_framebuffers || fb_src_id >= used_framebuffers) {
        return;
    }

    Framebuffer& dst_buffer = framebuffers[fb_dst_id];
    Framebuffer& src_buffer = framebuffers[fb_src_id];

    int32_t fb_width = src_buffer.color_buffer.surface.width;
    int32_t fb_height = src_buffer.color_buffer.surface.height;
    srcX0 = std::clamp(srcX0, 0, fb_width);
    srcX1 = std::clamp(srcX1, 0, fb_width);
    srcY0 = std::clamp(srcY0, 0, fb_height);
    srcY1 = std::clamp(srcY1, 0, fb_height);

    GX2Rect src = { srcX0, srcY0, srcX1, srcY1 };
    GX2Point dst = { dstX0, dstY0 };
    GX2CopySurfaceEx(&src_buffer.color_buffer.surface, 0, 0, &dst_buffer.color_buffer.surface, 0, 0, 1, &src, &dst);

    // The destination is sampled as a texture straight after this, so the copy
    // has to be visible to the texture unit rather than sitting in the colour
    // cache.
    if (dst_buffer.texture.surface.image != nullptr) {
        GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE,
                      dst_buffer.texture.surface.image, dst_buffer.texture.surface.imageSize);
    }

    gfx_wiiu_set_context_state();
}

void GfxRenderingAPIGX2::ReadFramebufferToCPU(int fb_id, uint32_t width, uint32_t height, uint16_t* rgba16_buf) {
    if (fb_id >= used_framebuffers) {
        return;
    }

    Framebuffer& buffer = framebuffers[fb_id];

    // Create a temporary linear surface in the correct format
    GX2Surface surface;
    memset(&surface, 0, sizeof(GX2Surface));
    // [port] ConvertSurface renders into this surface, so it has to be usable as
    // a colour buffer as well - asking for USE_TEXTURE alone sizes and aligns it
    // as a plain texture while the GPU writes it as a render target.
    surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER);
    surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    surface.width = width;
    surface.height = height;
    surface.depth = 1;
    surface.mipLevels = 1;
    // [port] The caller only byteswaps the result on little-endian hosts, so on
    // Wii U this has to come out of the GPU already in N64 RGBA16 order, red in
    // the high bits. A1_B5_G5_R5 is the little-endian spelling and lands alpha
    // and blue there instead, which is what tinted the captured screen blue.
    surface.format = GX2_SURFACE_FORMAT_UNORM_R5_G5_B5_A1;
    surface.aa = GX2_AA_MODE1X;
    surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&surface);

    surface.image = memalign(surface.alignment, surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, surface.image, surface.imageSize);

    GX2Util::ConvertSurface(&buffer.color_buffer.surface, &surface);
    GX2DrawDone();

    // [port] The GPU has just written this buffer; the CPU is about to read it.
    // Without invalidating here the CPU serves stale cache lines for the block
    // memalign handed back, so the capture is whatever previously occupied that
    // memory rather than the frame that was just drawn.
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, surface.image, surface.imageSize);

    gfx_wiiu_set_context_state();

    for (int y = 0; y < height; y++) {
        memcpy(rgba16_buf + y * width, ((uint16_t*) surface.image) + y * surface.pitch, width * 2);
    }

    free(surface.image);
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIGX2::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    Framebuffer& buffer = framebuffers[fb_id];

    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;
    GX2Rect srcRects[25];
    GX2Point dstPoints[25];
    size_t num_coordinates = coordinates.size();
    while (num_coordinates > 0) {
        size_t numRects = 25;
        if (num_coordinates < numRects) {
            numRects = num_coordinates;
        }
        num_coordinates -= numRects;

        // initialize rects and points
        for (size_t i = 0; i < numRects; ++i) {
            const auto& c = *std::next(coordinates.begin(), num_coordinates + i);
            const int32_t x = (int32_t)std::clamp(c.first, 0.0f, (float)(buffer.depth_buffer.surface.width - 1));
            const int32_t y = (int32_t)std::clamp(c.second, 0.0f, (float)(buffer.depth_buffer.surface.height - 1));

            srcRects[i] = GX2Rect{ x, (int32_t)buffer.depth_buffer.surface.height - y, x + 1,
                                   (int32_t)(buffer.depth_buffer.surface.height - y) + 1 };

            // dst points will be spread over the x-axis of the buffer
            dstPoints[i] = GX2Point{ i, 0 };
        }

        // Invalidate the buffer first
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_DEPTH_BUFFER, depthReadBuffer.surface.image,
                      depthReadBuffer.surface.imageSize);

        // Perform the copy
        GX2CopySurfaceEx(&buffer.depth_buffer.surface, 0, 0, &depthReadBuffer.surface, 0, 0, numRects, srcRects,
                         dstPoints);

        // Wait for draws to be done and restore context, in case GPU was used
        GX2DrawDone();
        gfx_wiiu_set_context_state();

        // read the pixels from the depthReadBuffer
        for (size_t i = 0; i < numRects; ++i) {
            uint32_t tmp = __builtin_bswap32(*((uint32_t*)depthReadBuffer.surface.image + i));
            float val = std::bit_cast<float>(tmp);

            const auto& c = *std::next(coordinates.begin(), num_coordinates + i);
            res.emplace(c, val * 65532.0f);
        }
    }

    return res;
}

void GfxRenderingAPIGX2::SetTextureFilter(FilteringMode mode) {
    // three-point is not implemented in the shaders yet
    if (mode == FILTER_THREE_POINT) {
        mode = FILTER_LINEAR;
    }

    current_filter_mode = mode;
    gfx_texture_cache_clear();
}

FilteringMode GfxRenderingAPIGX2::GetTextureFilter(void) {
    return current_filter_mode;
}

ImGui_ImplGX2_Texture* gfx_gx2_texture_for_imgui(uint32_t texture_id) {
    struct GX2TextureObj* tex = (struct GX2TextureObj*)texture_id;
    return &tex->imtex;
}

void GfxRenderingAPIGX2::SetSrgbMode() {
}

void GfxRenderingAPIGX2::ClearShaderCache() {
    for (auto& entry : shader_program_pool) {
        gx2FreeShaderGroup(&entry.second.group);
    }
    shader_program_pool.clear();
    current_shader_program = nullptr;
}

void GfxRenderingAPIGX2::SetCurrentPrimDepth(float depth) {
    if (depth != mCurrentPrimDepth) {
        mCurrentPrimDepth = depth;
        mPrimDepthDirty = true;
    }
}

ImTextureID GfxRenderingAPIGX2::GetTextureById(int id) {
    struct GX2TextureObj* tex = (struct GX2TextureObj*)id;
    return (ImTextureID)&tex->imtex;
}


} // namespace Fast

#endif
