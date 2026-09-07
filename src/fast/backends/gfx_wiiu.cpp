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
#include <cstdlib>
#include <chrono>
#include <thread>
#include <whb/log.h>
#include <whb/log_udp.h>
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

    // [port] init_mem1 takes its block with MEMAllocFromFrmHeapEx, and this only
    // destroyed the expanded heap living inside that block - the block itself was
    // never given back. The system waits on MEM1 being released during a
    // foreground handover, so the HOME menu hung the console. Return it, exactly
    // as gfx_wiiu_destroy_foreground does for the foreground bucket. (The heap
    // handle above was fetched and unused, which is what gave this away.)
    MEMFreeToFrmHeap(heap, MEM_FRM_HEAP_FREE_ALL);
    WHBLogPrintf("[gfx_wiiu] MEM1 returned to the frame heap");
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

extern "C" void gfx_gx2_release_mem1_surfaces(void);
extern "C" void gfx_gx2_restore_mem1_surfaces(void);
// Defined by the port: game speed comes off this cadence, so it has to stop
// while the system owns the foreground.
extern "C" void OS_SetViPaused(int paused);
// Also defined by the port: begins an orderly shutdown and wakes anything parked
// on a message queue so it can unwind.
extern "C" void OS_RequestThreadExit(void);

static bool sMem1Owned = false;
// Whether the app currently owns the foreground. Nothing may touch GX2 or MEM1
// while it does not: those surfaces have been handed back to the system.
extern "C" int gfx_wiiu_has_foreground(void) {
    return has_foreground ? 1 : 0;
}

uint32_t gWiiuVsyncWaits = 0;
uint32_t gWiiuWaitedFrames = 0;

static uint32_t gfx_wiiu_proc_callback_acquired(void* context) {
    WHBLogPrintf("[gfx_wiiu] -> init_foreground");
    bool result = gfx_wiiu_init_foreground();
    assert(result);

    // [port] MEM1 is foreground memory and was handed back on release, so it has
    // to be rebuilt before the render surfaces that live in it. Init() does this
    // for the first acquire; every later one comes through here.
    if (!sMem1Owned) {
        if (gfx_wiiu_init_mem1()) {
            sMem1Owned = true;
            gfx_gx2_restore_mem1_surfaces();
        } else {
            WHBLogPrintf("[gfx_wiiu] !! MEM1 re-init FAILED on foreground acquire");
        }
    }

    has_foreground = true;

    // [port] Each step logs, because a freeze on returning from the HOME menu
    // stopped the main loop somewhere in this callback or just after it, and the
    // last line in the log was the MEM1 restore above. This path runs once per
    // handover, so the cost does not matter.
    WHBLogPrintf("[gfx_wiiu] acquire: allocating TV scan buffer");
    tv_scan_buffer = gfx_wiiu_alloc_foreground(tv_scan_buffer_size, GX2_SCAN_BUFFER_ALIGNMENT);
    assert(tv_scan_buffer);

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, tv_scan_buffer, tv_scan_buffer_size);
    GX2SetTVBuffer(tv_scan_buffer, tv_scan_buffer_size, tv_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                   GX2_BUFFERING_MODE_DOUBLE);

    WHBLogPrintf("[gfx_wiiu] acquire: allocating DRC scan buffer");
    drc_scan_buffer = gfx_wiiu_alloc_foreground(drc_scan_buffer_size, GX2_SCAN_BUFFER_ALIGNMENT);
    assert(drc_scan_buffer);

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, drc_scan_buffer, drc_scan_buffer_size);
    GX2SetDRCBuffer(drc_scan_buffer, drc_scan_buffer_size, drc_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8,
                    GX2_BUFFERING_MODE_DOUBLE);

    // [port] Unpause last, not before the allocations above. Retraces drive the
    // game, so unpausing first let it advance and queue GPU work in the window
    // where the scan buffers had not been allocated or set yet. Unconfirmed as
    // the cause of the HOME-return freeze, but wrong on its own terms: nothing
    // should run until the surfaces it draws into exist.
    OS_SetViPaused(0);
    WHBLogPrintf("[gfx_wiiu] acquire: complete, VI resumed");

    return 0;
}

static uint32_t gfx_wiiu_proc_callback_released(void* context) {
    // Suspend the game first. Retraces kept firing across the handover while the
    // game thread could not consume them, which filled its queue (tickRetraceQ=60)
    // and deadlocked the pipeline on resume.
    OS_SetViPaused(1);

    // Stop anything drawing before the memory it draws into goes away, and let
    // the GPU finish what is already queued.
    has_foreground = false;
    GX2DrawDone();

    if (sMem1Owned) {
        gfx_gx2_release_mem1_surfaces();
        gfx_wiiu_destroy_mem1();
        sMem1Owned = false;
    }

    if (tv_scan_buffer) {
        gfx_wiiu_free_foreground(tv_scan_buffer);
        tv_scan_buffer = nullptr;
    }

    if (drc_scan_buffer) {
        gfx_wiiu_free_foreground(drc_scan_buffer);
        drc_scan_buffer = nullptr;
    }

    gfx_wiiu_destroy_foreground();

    return 0;
}

void GfxWindowBackendWiiU::Init(const char* game_name, const char* gfx_api_name, bool start_in_fullscreen, uint32_t width,
                          uint32_t height, int32_t posX, int32_t posY) {
    WHBLogPrintf("[gfx_wiiu] -> WHBProcInit");
    WHBProcInit();

    uint32_t mem1_addr, mem1_size;
    OSGetMemBound(OS_MEM1, &mem1_addr, &mem1_size);
    mem1_storage = memalign(0x40, mem1_size);
    assert(mem1_storage);

    ProcUISetMEM1Storage(mem1_storage, mem1_size);

    WHBLogPrintf("[gfx_wiiu] -> init_mem1");
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
    WHBLogPrintf("[gfx_wiiu] -> GX2Init");
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

    WHBLogPrintf("[gfx_wiiu] -> ProcUI callbacks");
    sMem1Owned = true; // Init() already built MEM1 before registering these
    // [port] Exit arrives as a release, which pauses the retrace cadence - and
    // then shutdown deadlocks, because vimgr and rumble park waiting for
    // retraces that will never come (the log shows both STALLED with every
    // retrace queue at 0). Resume the cadence on the way out so those threads
    // drain and the app can actually close.
    ProcUIRegisterCallback(
        PROCUI_CALLBACK_EXIT, [](void*) -> uint32_t {
            // Stop the retrace cadence so nothing new piles up, then ask the
            // game threads to exit. That request also wakes anything parked on a
            // message queue, which is what used to sleep through shutdown and
            // hang the console: pausing the cadence stalled vimgr, and resuming
            // it just overfilled the same queue.
            OS_SetViPaused(1);
            OS_RequestThreadExit();
            WHBLogPrintf("[gfx_wiiu] exit: cadence stopped, threads asked to unwind");

            // Backstop only. With the queue waits now breaking on the exit
            // request this should never fire; if it does, the log says so and
            // the shutdown path still needs work.
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::seconds(6));
                WHBLogPrintf("[gfx_wiiu] exit: STILL RUNNING after 6s, terminating - shutdown path is wrong");
                _Exit(0);
            }).detach();
            return 0;
        },
        nullptr, 100);
    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, gfx_wiiu_proc_callback_acquired, nullptr, 100);
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, gfx_wiiu_proc_callback_released, nullptr, 100);

    gfx_wiiu_proc_callback_acquired(nullptr);

    context_state = (GX2ContextState*)memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState));
    assert(context_state);

    GX2SetupContextStateEx(context_state, TRUE);
    WHBLogPrintf("[gfx_wiiu] -> GX2SetContextState");
    GX2SetContextState(context_state);

    // [port] These set how much of each scan buffer is presented, so they must
    // match the display's own mode - not the internal render size. Passing the
    // framebuffer constant put the image in a corner of a 1080p scan buffer once
    // the internal resolution stopped coincidentally being 1920x1080.
    // GX2CopyColorBufferToScanBuffer already scales our buffer to fit.
    GX2SetTVScale(tv_width, tv_height);
    GX2SetDRCScale(WIIU_DRC_WIDTH, WIIU_DRC_HEIGHT);
    WHBLogPrintf("[gfx_wiiu] tv scale %ux%u, drc scale %ux%u (render %ux%u)", (unsigned)tv_width, (unsigned)tv_height,
                 (unsigned)WIIU_DRC_WIDTH, (unsigned)WIIU_DRC_HEIGHT, (unsigned)WIIU_DEFAULT_FB_WIDTH,
                 (unsigned)WIIU_DEFAULT_FB_HEIGHT);

    GX2SetSwapInterval(frame_divisor);

    GuiWindowInitData window_impl;
    window_impl.Gx2.Width = WIIU_DEFAULT_FB_WIDTH;
    window_impl.Gx2.Height = WIIU_DEFAULT_FB_HEIGHT;
    window_impl.Backend = WindowBackend::FAST3D_WIIU_GX2;

    WHBLogPrintf("[gfx_wiiu] -> Gui::Init");
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
    // Always poll the pad: the game reads its input from here every iteration.
    Ship::WiiU::Update();

    // [port] The main loop runs far faster than the display - the idle counter
    // shows ~2500 iterations a second - and everything below is per-frame state.
    // Feeding the system keyboard 40 times per displayed frame stepped its cursor
    // 40 times too, which made typing an address essentially impossible. Hand the
    // GUI one sample per frame; the pad poll above is untouched.
    {
        static uint64_t lastUs = 0;
        const uint64_t nowUs = OSTicksToMicroseconds(OSGetSystemTime());
        if (lastUs != 0 && (nowUs - lastUs) < 16000) {
            return;
        }
        lastUs = nowUs;
    }

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
    // [port] While the system holds the foreground, the render surfaces have been
    // handed back and GX2 must not be touched. This gates the whole draw path, so
    // returning false here is what keeps the HOME menu from freezing the console.
    if (!has_foreground) {
        return false;
    }

    uint32_t swap_count, flip_count;
    OSTime last_flip, last_vsync;
    uint32_t wait_count = 0;
    // [port] Frame cost is climbing while the workload is flat. Counting how
    // often we sit here waiting on the GPU separates "GPU cannot keep up" from
    // "the CPU side got slower", which need opposite fixes.
    extern uint32_t gWiiuVsyncWaits;
    extern uint32_t gWiiuWaitedFrames;

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
        gWiiuVsyncWaits++;
        GX2WaitForVsync();
    }

    if (wait_count > 0) {
        gWiiuWaitedFrames++;
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
    static uint32_t swapCount = 0;
    if (swapCount < 3 || swapCount == 60 || swapCount == 300) {
        WHBLogPrintf("[gfx_wiiu] swap %u", swapCount);
    }
    swapCount++;
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
