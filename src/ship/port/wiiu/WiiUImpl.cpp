#ifdef __WIIU__
#include "WiiUImpl.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/iosupport.h>

#include <whb/log.h>
#include <whb/log_udp.h>
#include <coreinit/debug.h>

#include <SDL2/SDL.h>

#include <map>
#include "ship/Context.h"

namespace Ship {
namespace WiiU {

static bool updateControllers;
static std::map<int, SDL_GameController*> controllers;

static bool hasVpad = false;
static VPADReadError vpadError;
static VPADStatus vpadStatus;

static bool hasKpad[4] = { false };
static KPADError kpadError[4] = { KPAD_ERROR_OK };
static KPADStatus kpadStatus[4];

extern "C" {
#ifdef _DEBUG
void __wrap_abort() {
    printf("Abort called.\n");
    // force a stack trace
    *(uint32_t*)0xdeadc0de = 0xcafebabe;
    while (1)
        ;
}

#endif

static ssize_t wiiu_log_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%*.*s", len, len, ptr);
    OSReport(buf);
    WHBLogWritef("%*.*s", len, len, ptr);
    return len;
}

static const devoptab_t dotab_stdout = {
    .name = "stdout_whb",
    .write_r = wiiu_log_write,
};
};

void Init(const std::string& shortName) {
    // Not debug-only: without a device behind stdout, the first printf or
    // spdlog write in a Release build calls through a null write_r and jumps
    // into nothing. Point it at UDP logging instead.
    WHBLogUdpInit();

    devoptab_list[STD_OUT] = &dotab_stdout;
    devoptab_list[STD_ERR] = &dotab_stdout;

    // make sure the required folders exist
    mkdir("/vol/external01/wiiu/", 0755);
    mkdir("/vol/external01/wiiu/apps/", 0755);
    mkdir(("/vol/external01/wiiu/apps/" + shortName + "/").c_str(), 0755);

    chdir(("/vol/external01/wiiu/apps/" + shortName + "/").c_str());

    // We construct or input based on SDL
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    updateControllers = true;
}

void Exit() {
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);

    WHBLogUdpDeinit();
}

void ThrowMissingOTR(const char* otrPath) {
    // TODO handle this better in the future
    OSFatal("Main OTR file not found!");
}

void ThrowInvalidOTR() {
    OSFatal("Invalid OTR files! Try regenerating them!");
}

static void UpdateKPADProButton(KPADStatus* status, SDL_GameController* controller, WPADProButton button, SDL_GameControllerButton sdl_button)
{
    if (SDL_GameControllerGetButton(controller, sdl_button) != 0) {
        // Set the trigger bit if it wasn't held before
        if (!(status->pro.hold & button)) {
            status->pro.trigger |= button;
        } else {
            status->pro.trigger &= ~button;
        }

        status->pro.hold |= button;
        status->pro.release &= ~button;
    } else {
        // Set the release bit if it was held before
        if (status->pro.hold & button) {
            status->pro.release |= button;
        } else {
            status->pro.release &= ~button;
        }

        status->pro.hold &= ~button;
        status->pro.trigger &= ~button;
    }
}

void Update() {
    SDL_PumpEvents();

    // [port] Nothing here ever dequeued SDL events. The GamePad's sticks emit
    // axis motion continuously, so the queue grew without bound: ~50MB of heap
    // over a couple of minutes, and SDL_PumpEvents walking an ever longer queue
    // took this function from 0.004ms to 10ms a call - which is the frame rate
    // decaying and never recovering. Drain everything except the
    // controller-device range, which libultraship's own handler consumes to
    // register the pad. gfx_sdl2 does exactly this; the Wii U path never did.
    {
        SDL_Event drained;
        while (SDL_PeepEvents(&drained, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_CONTROLLERDEVICEADDED - 1) > 0) {
        }
        while (SDL_PeepEvents(&drained, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEREMOVED + 1, SDL_LASTEVENT) > 0) {
        }
    }

    // [port] This used to declare a local of the same name, shadowing the file
    // scope flag that Init() sets. The enumeration Init() asked for therefore
    // never ran, and controllers were only picked up if an SDL device-added event
    // arrived afterwards - but the GamePad is already connected before the event
    // loop starts, so that event had come and gone and no pad was ever opened.
    // Peek, never consume: libultraship's SDLAddRemoveDeviceEventHandler needs
    // these same events to register the physical device and build its default
    // mappings, and this runs first each frame. Taking them with SDL_GETEVENT
    // meant the pad opened here while the control deck never learned it existed,
    // so nothing reached the game. gfx_sdl2 steps around this range for the same
    // reason.
    SDL_Event event;
    if (SDL_PeepEvents(&event, 1, SDL_PEEKEVENT, SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEREMOVED) > 0) {
        updateControllers = true;
    }

    if (updateControllers) {
        for (auto& [index, controller] : controllers) {
            SDL_GameControllerClose(controller);
        }
        controllers.clear();
        hasVpad = false;

        int numJoysticks = SDL_NumJoysticks();
        for (int i = 0; i < numJoysticks; i++) {
            if (SDL_IsGameController(i)) {
                SDL_GameController* controller = SDL_GameControllerOpen(i);
                if (controller) {
                    int playerIndex = SDL_GameControllerGetPlayerIndex(controller);
                    if (playerIndex < 0) {
                        // The Wii U SDL port does not always assign a player index.
                        // Index 0 is the GamePad, so fall back to insertion order.
                        playerIndex = (int)controllers.size();
                    }
                    if (playerIndex == 0) {
                        hasVpad = true;
                    }

                    controllers.emplace(playerIndex, controller);
                }
            }
        }

        // Name each device and its player index: that is what decides which
        // virtual port it drives, and it is the first thing worth knowing when a
        // Pro Controller or a Wiimote does not respond.
        WHBLogPrintf("[input] %d joysticks, %u opened, vpad=%d", numJoysticks, (unsigned)controllers.size(),
                     (int)hasVpad);
        for (int i = 0; i < numJoysticks; i++) {
            const char* jname = SDL_JoystickNameForIndex(i);
            if (SDL_IsGameController(i)) {
                SDL_GameController* c = SDL_GameControllerFromInstanceID(
                    SDL_JoystickGetDeviceInstanceID(i));
                WHBLogPrintf("[input]   %d: '%s' controller='%s' player=%d", i, jname ? jname : "?",
                             c ? (SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "?") : "not open",
                             c ? SDL_GameControllerGetPlayerIndex(c) : -1);
            } else {
                WHBLogPrintf("[input]   %d: '%s' (not a game controller - needs a mapping)", i,
                             jname ? jname : "?");
            }
        }

        // Keep retrying while nothing has been opened yet: at startup SDL may not
        // have enumerated the pad, and without a retry we would never look again.
        updateControllers = controllers.empty();
    }

    // [port] Rebuild the VPAD button state from SDL. This is what feeds ImGui:
    // the Wii U ImGui backend reads vpadStatus.hold, and with this disabled it saw
    // a controller on which no button was ever pressed - which is why the
    // enhancements menu could not be opened on console at all. Synthesising from
    // SDL is safe; the warning above is about calling VPADRead a second time,
    // which would take the sample away from SDL. Nothing here touches VPADRead.
    //
    // Every pad feeds it, not only player 0. The ImGui backend does have Wiimote,
    // Classic and Pro paths, but they read KPAD, and nothing fills that here -
    // hasKpad is never set, so GetKPADStatus always returns null and those button
    // words stay zero. This synthesised VPAD is the only route to the menu, so
    // keying it to player 0 left a Pro Controller unable to open it whenever the
    // GamePad was also on and holding that index.
    {
        static const struct {
            VPADButtons vpad;
            SDL_GameControllerButton sdl;
        } kButtonMap[] = {
            { VPAD_BUTTON_A, SDL_CONTROLLER_BUTTON_A },
            { VPAD_BUTTON_B, SDL_CONTROLLER_BUTTON_B },
            { VPAD_BUTTON_X, SDL_CONTROLLER_BUTTON_X },
            { VPAD_BUTTON_Y, SDL_CONTROLLER_BUTTON_Y },
            { VPAD_BUTTON_PLUS, SDL_CONTROLLER_BUTTON_START },
            { VPAD_BUTTON_MINUS, SDL_CONTROLLER_BUTTON_BACK },
            { VPAD_BUTTON_UP, SDL_CONTROLLER_BUTTON_DPAD_UP },
            { VPAD_BUTTON_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
            { VPAD_BUTTON_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT },
            { VPAD_BUTTON_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
            { VPAD_BUTTON_L, SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
            { VPAD_BUTTON_R, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
        };

        // Whichever pad is pushed furthest wins the stick, so an idle one cannot
        // cancel out the pad actually being used.
        const auto furthest = [](float current, float candidate) {
            const float a = (candidate < 0.0f) ? -candidate : candidate;
            const float b = (current < 0.0f) ? -current : current;
            return (a > b) ? candidate : current;
        };

        uint32_t hold = 0;
        float leftX = 0.0f, leftY = 0.0f, rightX = 0.0f, rightY = 0.0f;

        for (auto& [index, controller] : controllers) {
            if (controller == nullptr) {
                continue;
            }
            for (const auto& entry : kButtonMap) {
                if (SDL_GameControllerGetButton(controller, entry.sdl) != 0) {
                    hold |= entry.vpad;
                }
            }

            // Sticks, so menu navigation works without the d-pad.
            leftX = furthest(leftX, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f);
            leftY = furthest(leftY, -SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f);
            rightX = furthest(rightX, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f);
            rightY = furthest(rightY, -SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f);
        }

        // trigger and release are edges against the previous frame, so they have to
        // be derived once from the combined state. Done per pad, an idle one would
        // clear the bit the active one had just set.
        const uint32_t previous = vpadStatus.hold;
        vpadStatus.hold = hold;
        vpadStatus.trigger = hold & ~previous;
        vpadStatus.release = previous & ~hold;

        vpadStatus.leftStick.x = leftX;
        vpadStatus.leftStick.y = leftY;
        vpadStatus.rightStick.x = rightX;
        vpadStatus.rightStick.y = rightY;
    }

    if (hasVpad) {
        vpadStatus.tpNormal.touched = false;

        int numTouchDevices = SDL_GetNumTouchDevices();
        if (numTouchDevices > 0) {
            SDL_TouchID touchId = SDL_GetTouchDevice(0);
            int numFingers = SDL_GetNumTouchFingers(touchId);
            if (numFingers > 0) {
                SDL_Finger *finger = SDL_GetTouchFinger(touchId, 0);
                if (finger) {
                    vpadStatus.tpNormal.touched = true;
                    vpadStatus.tpNormal.validity = VPAD_VALID;
                    // [port] tpNormal holds RAW panel coordinates on hardware, and
                    // consumers calibrate it. Writing screen pixels here worked for
                    // the ImGui path only because that path skipped calibration;
                    // the system keyboard calls VPADGetTPCalibratedPoint itself, so
                    // it read 0..1280 as a raw value and every tap collapsed into
                    // one corner. Supply the raw range and let each consumer
                    // calibrate, as on real hardware.
                    // The panel's raw Y runs the opposite way to screen Y - its
                    // origin is at the bottom - and VPADGetTPCalibratedPoint flips
                    // it back. Feeding SDL's downward-increasing Y straight in came
                    // out upside down: taps at the top registered at the bottom.
                    vpadStatus.tpNormal.x = (uint16_t)(finger->x * 4096.0f);
                    vpadStatus.tpNormal.y = (uint16_t)((1.0f - finger->y) * 4096.0f);

                    { // [touchdiag] report only new extremes, so four corner taps
                      // give the usable range rather than 30 samples of one press
                        static float minX = 9.0f, maxX = -9.0f, minY = 9.0f, maxY = -9.0f;
                        bool grew = false;
                        if (finger->x < minX) { minX = finger->x; grew = true; }
                        if (finger->x > maxX) { maxX = finger->x; grew = true; }
                        if (finger->y < minY) { minY = finger->y; grew = true; }
                        if (finger->y > maxY) { maxY = finger->y; grew = true; }
                        if (grew) {
                            WHBLogPrintf("[touchdiag] range x %.4f..%.4f  y %.4f..%.4f"
                                         "  (this %.4f,%.4f -> tp=%u,%u)",
                                         minX, maxX, minY, maxY, finger->x, finger->y,
                                         (unsigned)vpadStatus.tpNormal.x,
                                         (unsigned)vpadStatus.tpNormal.y);
                        }
                    }
                }
            }
        }
    }
}

VPADStatus* GetVPADStatus(VPADReadError* error) {
    *error = vpadError;
    return hasVpad ? &vpadStatus : nullptr;
}

KPADStatus* GetKPADStatus(WPADChan chan, KPADError* error) {
    *error = kpadError[chan];
    return hasKpad[chan] ? &kpadStatus[chan] : nullptr;
}

}; // namespace WiiU
}; // namespace Ship

#endif
