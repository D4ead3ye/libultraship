#ifdef __WIIU__
#pragma once

#include <vpad/input.h>
#include <padscore/kpad.h>

#include "gfx_window_manager_api.h"

// make the default fb always 1080p to not mess with scaling
#define WIIU_DEFAULT_FB_WIDTH 1920
#define WIIU_DEFAULT_FB_HEIGHT 1080

namespace Fast {

extern bool has_foreground;
extern uint32_t frametime;

bool gfx_wiiu_init_mem1(void);
void gfx_wiiu_destroy_mem1(void);
bool gfx_wiiu_init_foreground(void);
void gfx_wiiu_destroy_foreground(void);
uint32_t gfx_wiiu_mem1_free(void);
void* gfx_wiiu_alloc_mem1(uint32_t size, uint32_t alignment);
void gfx_wiiu_free_mem1(void* block);
void* gfx_wiiu_alloc_foreground(uint32_t size, uint32_t alignment);
void gfx_wiiu_free_foreground(void* block);
void gfx_wiiu_set_context_state(void);

class GfxWindowBackendWiiU final : public GfxWindowBackend {
  public:
    GfxWindowBackendWiiU() = default;
    ~GfxWindowBackendWiiU() override = default;

    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override;
    void Close() override;
    void SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                              void (*onAllKeysUp)()) override;
    void SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) override;
    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) override;
    void SetFullscreen(bool fullscreen) override;
    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override;
    void SetCursorVisibility(bool visability) override;
    void SetMousePos(int32_t posX, int32_t posY) override;
    void GetMousePos(int32_t* x, int32_t* y) override;
    void GetMouseDelta(int32_t* x, int32_t* y) override;
    void GetMouseWheel(float* x, float* y) override;
    bool GetMouseState(uint32_t btn) override;
    void SetMouseCapture(bool capture) override;
    bool IsMouseCaptured() override;
    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override;
    void SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) override;
    Ship::WindowRect GetPrimaryMonitorRect() override;
    void HandleEvents() override;
    bool IsFrameReady() override;
    void SwapBuffersBegin() override;
    void SwapBuffersEnd() override;
    double GetTime() override;
    int GetTargetFps() override;
    void SetTargetFps(int fps) override;
    void SetMaxFrameLatency(int latency) override;
    const char* GetKeyName(int scancode) override;
    bool CanDisableVsync() override;
    bool IsRunning() override;
    void Destroy() override;
    bool IsFullscreen() override;
};

} // namespace Fast

#endif
