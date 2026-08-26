#ifdef __WIIU__

#include <stdio.h>
#include <time.h>
#include <malloc.h>

#include <coreinit/time.h>
#include <coreinit/foreground.h>
#include <coreinit/memory.h>
#include <coreinit/memheap.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memfrmheap.h>

#include <gx2/state.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/swap.h>
#include <gx2/mem.h>
#include <gx2r/mem.h>

#include <whb/proc.h>
#include "ship/window/Window.h"
#include "ship/Context.h"
#include "fast/Fast3dGui.h"
#include "fast/WindowEvent.h"
#include <proc_ui/procui.h>
#include <proc_ui/memory.h>

#include <vpad/input.h>
#include <padscore/kpad.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#include "fast/backends/gfx_wiiu.h"
#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/log_console.h>
#include "fast/backends/gfx_gx2.h"
#include "fast/backends/gfx_gx2.h"
#include "fast/backends/gfx_wiiu.h"

#include <ship/port/wiiu/ImGui/imgui_impl_wiiu.h>
#include "ship/port/wiiu/WiiUImpl.h"
#include "libultraship/classes.h"

static MEMHeapHandle heap_MEM1 = nullptr;
static MEMHeapHandle heap_foreground = nullptr;

namespace Fast {

bool has_foreground = false;
static void* mem1_storage = nullptr;
static void* command_buffer_pool = nullptr;
GX2ContextState* context_state = nullptr;

static GX2TVRenderMode tv_render_mode;
static void* tv_scan_buffer = nullptr;
static uint32_t tv_scan_buffer_size = 0;
static uint32_t tv_width;
static uint32_t tv_height;

static GX2DrcRenderMode drc_render_mode;
static void* drc_scan_buffer = nullptr;
static uint32_t drc_scan_buffer_size = 0;

static int frame_divisor = 1;

// for ImGui DeltaTime
// (initialized to 1 to not trigger imguis assert on initial draw)
uint32_t frametime = 1;

bool gfx_wiiu_init_mem1(void) {
    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
    uint32_t size;
    void* base;

    size = MEMGetAllocatableSizeForFrmHeapEx(heap, 4);
    if (!size) {
        printf("%s: MEMGetAllocatableSizeForFrmHeapEx == 0", __FUNCTION__);
        return false;
    }

    base = MEMAllocFromFrmHeapEx(heap, size, 4);
    if (!base) {
        printf("%s: MEMAllocFromFrmHeapEx(heap, 0x%X, 4) failed", __FUNCTION__, size);
        return false;
    }

    heap_MEM1 = MEMCreateExpHeapEx(base, size, 0);
    if (!heap_MEM1) {
        printf("%s: MEMCreateExpHeapEx(%p, 0x%X, 0) failed", __FUNCTION__, base, size);
        return false;
    }

    return true;
}

void GfxWindowBackendWiiU::Close(void) {
}

void gfx_wiiu_destroy_mem1(void) {
    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);

    if (heap_MEM1) {
        MEMDestroyExpHeap(heap_MEM1);
        heap_MEM1 = NULL;
    }
}

bool gfx_wiiu_init_foreground(void) {
    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG);
    uint32_t size;
    void* base;

    size = MEMGetAllocatableSizeForFrmHeapEx(heap, 4);
    if (!size) {
        printf("%s: MEMAllocFromFrmHeapEx(heap, 0x%X, 4)", __FUNCTION__, size);
        return false;
    }

    base = MEMAllocFromFrmHeapEx(heap, size, 4);
    if (!base) {
        printf("%s: MEMGetAllocatableSizeForFrmHeapEx == 0", __FUNCTION__);
        return false;
    }

    heap_foreground = MEMCreateExpHeapEx(base, size, 0);
    if (!heap_foreground) {
        printf("%s: MEMCreateExpHeapEx(%p, 0x%X, 0)", __FUNCTION__, base, size);
        return false;
    }

    return true;
}

void gfx_wiiu_destroy_foreground(void) {
    MEMHeapHandle foreground = MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG);

    if (heap_foreground) {
        MEMDestroyExpHeap(heap_foreground);
        heap_foreground = NULL;
    }

    MEMFreeToFrmHeap(foreground, MEM_FRM_HEAP_FREE_ALL);
}

uint32_t gfx_wiiu_mem1_free(void) {
    if (!heap_MEM1) {
        return 0;
    }
    return MEMGetTotalFreeSizeForExpHeap(heap_MEM1);
}

void* gfx_wiiu_alloc_mem1(uint32_t size, uint32_t alignment) {
    void* block;

    if (!heap_MEM1) {
        return NULL;
    }

    if (alignment < 4) {
        alignment = 4;
    }

    block = MEMAllocFromExpHeapEx(heap_MEM1, size, alignment);
    return block;
}

void gfx_wiiu_free_mem1(void* block) {
    if (!heap_MEM1) {
        return;
    }

    MEMFreeToExpHeap(heap_MEM1, block);
}

void* gfx_wiiu_alloc_foreground(uint32_t size, uint32_t alignment) {
    void* block;

    if (!heap_foreground) {
        return NULL;
    }

    if (alignment < 4) {
        alignment = 4;
    }

    block = MEMAllocFromExpHeapEx(heap_foreground, size, alignment);
    return block;
}

void gfx_wiiu_free_foreground(void* block) {
    if (!heap_foreground) {
        return;
    }

    MEMFreeToExpHeap(heap_foreground, block);
}

static uint32_t gfx_wiiu_proc_callback_acquired(void* context) {
    has_foreground = true;

    WHBLogConsoleDraw(), WHBLogPrintf("[gfx_wiiu] -> init_foreground");
    bool result = gfx_wiiu_init_foreground();
    assert(result);

    tv_scan_buffer = gfx_wiiu_alloc_foreground(tv_scan_buffer_size, GX2_SCAN_BUFFER_ALIGNMENT);
    assert(tv_scan_buffer);

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, tv_scan_buffer, tv_scan_buffer_size);
    GX2SetTVBuffer(tv_scan_buffer, tv_scan_buffer_size, tv_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                   GX2_BUFFERING_MODE_DOUBLE);

    drc_scan_buffer = gfx_wiiu_alloc_foreground(drc_scan_buffer_size, GX2_SCAN_BUFFER_ALIGNMENT);
    assert(drc_scan_buffer);

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, drc_scan_buffer, drc_scan_buffer_size);
    GX2SetDRCBuffer(drc_scan_buffer, drc_scan_buffer_size, drc_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                    GX2_BUFFERING_MODE_DOUBLE);

    return 0;
}

static uint32_t gfx_wiiu_proc_callback_released(void* context) {
    if (tv_scan_buffer) {
        gfx_wiiu_free_foreground(tv_scan_buffer);
        tv_scan_buffer = nullptr;
    }

    if (drc_scan_buffer) {
        gfx_wiiu_free_foreground(drc_scan_buffer);
        drc_scan_buffer = nullptr;
    }

    gfx_wiiu_destroy_foreground();

    has_foreground = false;

    return 0;
}

void GfxWindowBackendWiiU::Init(const char* game_name, const char* gfx_api_name, bool start_in_fullscreen, uint32_t width,
                          uint32_t height, int32_t posX, int32_t posY) {
    WHBLogConsoleDraw(), WHBLogPrintf("[gfx_wiiu] -> WHBProcInit");
    WHBProcInit();

    uint32_t mem1_addr, mem1_size;
    OSGetMemBound(OS_MEM1, &mem1_addr, &mem1_size);
    mem1_storage = memalign(0x40, mem1_size);
    assert(mem1_storage);

    ProcUISetMEM1Storage(mem1_storage, mem1_size);

    WHBLogConsoleDraw(), WHBLogPrintf("[gfx_wiiu] -> init_mem1");
    bool result = gfx_wiiu_init_mem1();
    assert(result);

    command_buffer_pool = memalign(GX2_COMMAND_BUFFER_ALIGNMENT, 0x400000);
    assert(command_buffer_pool);

    uint32_t initAttribs[] = { GX2_INIT_CMD_BUF_BASE,
                               (uintptr_t)command_buffer_pool,
                               GX2_INIT_CMD_BUF_POOL_SIZE,
                               0x400000,
                               GX2_INIT_ARGC,
                               0,
                               GX2_INIT_ARGV,
                               0,
                               GX2_INIT_END };
    WHBLogConsoleDraw(), WHBLogPrintf("[gfx_wiiu] -> GX2Init (screen log ends here)");
    WHBLogConsoleDraw();
    // GX2 and OSScreen cannot both own the scan buffers
    WHBLogConsoleFree();
    GX2Init(initAttribs);

    switch (GX2GetSystemTVScanMode()) {
        case GX2_TV_SCAN_MODE_480I:
        case GX2_TV_SCAN_MODE_480P:
            tv_render_mode = GX2_TV_RENDER_MODE_WIDE_480P;
            tv_width = 854;
            tv_height = 480;
            break;
        case GX2_TV_SCAN_MODE_1080I:
        case GX2_TV_SCAN_MODE_1080P:
            tv_render_mode = GX2_TV_RENDER_MODE_WIDE_1080P;
            tv_width = 1920;
            tv_height = 1080;
            break;
        case GX2_TV_SCAN_MODE_720P:
        default:
            tv_render_mode = GX2_TV_RENDER_MODE_WIDE_720P;
            tv_width = 1280;
            tv_height = 720;
            break;
    }

    drc_render_mode = GX2GetSystemDRCScanMode();

    uint32_t unk;
    GX2CalcTVSize(tv_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE, &tv_scan_buffer_size,
                  &unk);
    GX2CalcDRCSize(drc_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE,
                   &drc_scan_buffer_size, &unk);

    WHBLogConsoleDraw(), WHBLogPrintf("[gfx_wiiu] -> ProcUI callbacks");
    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, gfx_wiiu_proc_callback_acquired, nullptr, 100);
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, gfx_wiiu_proc_callback_released, nullptr, 100);

    gfx_wiiu_proc_callback_acquired(nullptr);

    context_state = (GX2ContextState*)memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState));
    assert(context_state);

    GX2SetupContextStateEx(context_state, TRUE);
    WHBLogConsoleDraw(), WHBLogPrintf("[gfx_wiiu] -> GX2SetContextState");
    GX2SetContextState(context_state);

    GX2SetTVScale(WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT);
    GX2SetDRCScale(WIIU_DEFAULT_FB_WIDTH, WIIU_DEFAULT_FB_HEIGHT);

    GX2SetSwapInterval(frame_divisor);

    GuiWindowInitData window_impl;
    window_impl.Gx2.Width = WIIU_DEFAULT_FB_WIDTH;
    window_impl.Gx2.Height = WIIU_DEFAULT_FB_HEIGHT;
    window_impl.Backend = WindowBackend::FAST3D_WIIU_GX2;

    WHBLogConsoleDraw(), WHBLogPrintf("[gfx_wiiu] -> Gui::Init");
    std::dynamic_pointer_cast<Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())
        ->Init(window_impl);
}

static void gfx_wiiu_shutdown(void) {
    if (has_foreground) {
        gfx_wiiu_proc_callback_released(nullptr);
        gfx_wiiu_destroy_mem1();
    }

    GX2Shutdown();

    if (context_state) {
        free(context_state);
        context_state = nullptr;
    }

    if (command_buffer_pool) {
        free(command_buffer_pool);
        command_buffer_pool = nullptr;
    }

    ProcUISetMEM1Storage(nullptr, 0);
    free(mem1_storage);
}

void gfx_wiiu_set_context_state(void) {
    GX2SetContextState(context_state);
}

void GfxWindowBackendWiiU::SetFullscreenChangedCallback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
}

void GfxWindowBackendWiiU::SetFullscreen(bool enable) {
}

void GfxWindowBackendWiiU::GetActiveWindowRefreshRate(uint32_t* refresh_rate) {
    *refresh_rate = 60;
}

void GfxWindowBackendWiiU::SetCursorVisibility(bool hide) {
}

void GfxWindowBackendWiiU::SetMousePos(int32_t x, int32_t y) {
}

void GfxWindowBackendWiiU::GetMousePos(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendWiiU::GetMouseDelta(int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

void GfxWindowBackendWiiU::GetMouseWheel(float* x, float* y) {
    *x = 0;
    *y = 0;
}

bool GfxWindowBackendWiiU::GetMouseState(uint32_t btn) {
    return false;
}

void GfxWindowBackendWiiU::SetMouseCapture(bool capture) {
}

bool GfxWindowBackendWiiU::IsMouseCaptured() {
    return false;
}

void GfxWindowBackendWiiU::SetKeyboardCallbacks(bool (*on_key_down)(int scancode), bool (*on_key_up)(int scancode),
                                            void (*on_all_keys_up)(void)) {
}

void GfxWindowBackendWiiU::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    *width = WIIU_DEFAULT_FB_WIDTH;
    *height = WIIU_DEFAULT_FB_HEIGHT;
    *posX = 0;
    *posY = 0;
}

void GfxWindowBackendWiiU::HandleEvents(void) {
    Ship::WiiU::Update();

    ImGui_ImplWiiU_ControllerInput input{};

    VPADReadError vpad_error;
    input.vpad = Ship::WiiU::GetVPADStatus(&vpad_error);
    if (vpad_error != VPAD_READ_SUCCESS) {
        input.vpad = nullptr;
    }

    KPADError kpad_error;
    for (int i = 0; i < 4; i++) {
        input.kpad[i] = Ship::WiiU::GetKPADStatus((WPADChan)i, &kpad_error);
        if (kpad_error != KPAD_ERROR_OK) {
            input.kpad[i] = nullptr;
        }
    }

    WindowEvent event_impl;
    event_impl.Gx2.Input = &input;
    auto fast3dGui = std::dynamic_pointer_cast<Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui());
    if (fast3dGui) {
        fast3dGui->HandleWindowEvents(event_impl);
    }
}

bool GfxWindowBackendWiiU::IsFrameReady(void) {
    uint32_t swap_count, flip_count;
    OSTime last_flip, last_vsync;
    uint32_t wait_count = 0;

    while (true) {
        GX2GetSwapStatus(&swap_count, &flip_count, &last_flip, &last_vsync);

        if (flip_count >= swap_count) {
            break;
        }

        if (wait_count >= 10) {
            // GPU timed out, drop frame
            return false;
        }

        wait_count++;
        GX2WaitForVsync();
    }

    return true;
}

void GfxWindowBackendWiiU::SwapBuffersBegin(void) {
    GX2SwapScanBuffers();
    GX2Flush();

    gfx_wiiu_set_context_state();

    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
}

void GfxWindowBackendWiiU::SwapBuffersEnd(void) {
    static OSTick tick = 0;
    frametime = OSTicksToMicroseconds(OSGetSystemTick() - tick);
    tick = OSGetSystemTick();
}

double GfxWindowBackendWiiU::GetTime(void) {
    return 0.0;
}

void GfxWindowBackendWiiU::SetTargetFps(int fps) {
    // use the nearest divisor
    int divisor = 60 / fps;
    if (divisor < 1) {
        divisor = 1;
    }

    if (frame_divisor != divisor) {
        GX2SetSwapInterval(divisor);
        frame_divisor = divisor;
    }
}

void GfxWindowBackendWiiU::SetMaxFrameLatency(int latency) {
}

const char* GfxWindowBackendWiiU::GetKeyName(int scancode) {
    return "";
}

bool GfxWindowBackendWiiU::CanDisableVsync() {
    return false;
}

bool GfxWindowBackendWiiU::IsRunning(void) {
    return WHBProcIsRunning();
}

void GfxWindowBackendWiiU::Destroy(void) {
    Ship::WiiU::Exit();

    gfx_gx2_shutdown();
    gfx_wiiu_shutdown();
    WHBProcShutdown();
}

bool GfxWindowBackendWiiU::IsFullscreen(void) {
    return true;
}



void GfxWindowBackendWiiU::SetMouseCallbacks(bool (*on_mouse_button_down)(int btn),
                                             bool (*on_mouse_button_up)(int btn)) {
    // No pointer device is wired up; DRC touch is handled as a controller input.
}

void GfxWindowBackendWiiU::SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    // The scan buffers are fixed at boot, so the window cannot be resized.
}

Ship::WindowRect GfxWindowBackendWiiU::GetPrimaryMonitorRect() {
    return { 0, 0, (int32_t)WIIU_DEFAULT_FB_WIDTH, (int32_t)WIIU_DEFAULT_FB_HEIGHT };
}

int GfxWindowBackendWiiU::GetTargetFps() {
    return 60 / frame_divisor;
}

} // namespace Fast

#endif
