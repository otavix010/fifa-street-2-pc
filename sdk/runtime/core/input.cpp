// controllers through SDL: Xbox, PlayStation and Switch pads read into the console controller's report
#include "core/input.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGR("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        LOGR("input: controller %d unplugged", i + 1);
    }
}

bool AnyPad() {
    for (SDL_Gamepad* pad : g_pads)
        if (pad) return true;
    return false;
}

// A generic pad Windows lists but SDL has no mapping for (a plain DirectInput pad): the usual layout of those, if it has
// the buttons and axes for it, so that it works instead of being ignored
void MapGenericPads() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) return;
    for (int i = 0; i < count; i++) {
        SDL_JoystickID id = ids[i];
        const char* name = SDL_GetJoystickNameForID(id);
        SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
        char text[64];
        SDL_GUIDToString(guid, text, sizeof(text));
        bool mapped = SDL_IsGamepad(id);
        LOGR("input: joystick %s (vid %04x pid %04x): %s", name ? name : "?", SDL_GetJoystickVendorForID(id),
             SDL_GetJoystickProductForID(id), mapped ? "has a mapping" : "no mapping");
        if (mapped) continue;
        SDL_Joystick* joy = SDL_OpenJoystick(id);
        if (!joy) continue;
        int buttons = SDL_GetNumJoystickButtons(joy), axes = SDL_GetNumJoystickAxes(joy);
        SDL_CloseJoystick(joy);
        if (buttons < 10 || axes < 4) continue;
        std::string map = std::string(text) + "," + (name ? name : "Generic pad") +
                          ",a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b8,start:b9,"
                          "leftstick:b10,rightstick:b11,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,"
                          "rightx:a2,righty:a3";
        if (SDL_AddGamepadMapping(map.c_str()) >= 0) {
            LOGR("input: generic mapping for %s", name ? name : "?");
            AddPad(id);
        }
    }
    SDL_free(ids);
}

// No pad taken a few seconds in: DirectInput and the raw input are turned on and the list is made again, so that a
// generic pad shows up (they stay off while a pad works, two readers of one Xbox pad would be two players)
void RetryWithDirectInput() {
    static bool tried = false;
    static uint64_t since = SDL_GetTicks();
    if (tried || AnyPad() || SDL_GetTicks() - since < 6000) return;
    tried = true;
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        LOGR("input: SDL did not start again: %s", SDL_GetError());
        return;
    }
    LOGR("input: no controller after 6 s, looking again with DirectInput");
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
    MapGenericPads();
}

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
    RetryWithDirectInput();
}

}  // namespace

void Init() {
    if (g_sdl) return;  // started already for the game code window
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL failed to start, no controllers: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGR("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        if (SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr) {
            p.connected = true;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // each console button reads the pad button or trigger the settings map to it, 0 to 255
            static const std::array<std::pair<SDL_GamepadButton, SDL_GamepadAxis>, 16> sources = [] {
                std::array<std::pair<SDL_GamepadButton, SDL_GamepadAxis>, 16> out{};
                for (size_t i = 0; i < 16; i++) {
                    const char* name = app::Get().buttons[i].c_str();
                    out[i] = {SDL_GetGamepadButtonFromString(name), SDL_GetGamepadAxisFromString(name)};
                }
                return out;
            }();
            auto level = [&](size_t i) -> uint8_t {
                auto [b, a] = sources[i];
                if (b != SDL_GAMEPAD_BUTTON_INVALID) return SDL_GetGamepadButton(pad, b) ? 0xFF : 0;
                if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                    return uint8_t(std::max(axis(a), 0) >> 7);  // 0 to 32767 into 0 to 255
                return 0;
            };
            for (size_t i = 0; i < 8; i++) p.buttons |= uint16_t((level(i) >= 0x80) << i);
            for (size_t i = 0; i < 8; i++) p.analog[i] = level(8 + i);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
            // radial dead zone, the rest rescaled to full range
            if (uint32_t dz = app::Get().deadzone) {
                auto zone = [&](int16_t& x, int16_t& y) {
                    float fx = x / 32767.0f, fy = y / 32767.0f, m = std::sqrt(fx * fx + fy * fy), d = dz / 100.0f;
                    float k = m <= d ? 0.0f : std::min(1.0f, (m - d) / (1.0f - d)) / m;
                    x = int16_t(std::clamp(fx * k * 32767.0f, -32768.0f, 32767.0f));
                    y = int16_t(std::clamp(fy * k * 32767.0f, -32768.0f, 32767.0f));
                };
                zone(p.lx, p.ly);
                zone(p.rx, p.ry);
            }
        }
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

uint16_t HeldOnAnyController() {
    constexpr std::pair<SDL_GamepadButton, uint16_t> kFaceButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},  {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
    std::lock_guard lock(g_lock);
    UpdatePads();
    uint16_t held = 0;
    for (SDL_Gamepad* pad : g_pads)
        for (auto [button, mask] : kFaceButtons)
            if (pad && SDL_GetGamepadButton(pad, button)) held |= mask;
    return held;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
