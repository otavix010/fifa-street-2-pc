// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.interp = t["frame_interpolation"].value_or(s.interp);
    s.language = t["language"].value_or(s.language);
    s.volume = std::min<uint32_t>(t["volume"].value_or(s.volume), 100);
    s.upscaler = t["upscaler"].value_or(s.upscaler);
    s.upscaler_mode = t["upscaler_mode"].value_or(s.upscaler_mode);
    s.upscaler_sharpness = std::min<uint32_t>(t["upscaler_sharpness"].value_or(s.upscaler_sharpness), 100);
    s.deadzone = std::min<uint32_t>(t["deadzone"].value_or(s.deadzone), 90);
    s.game_path = t["game_path"].value_or(s.game_path);
    if (const toml::table* b = t["buttons"].as_table()) {
        static const char* kNames[16] = {"up", "down", "left", "right", "start", "back", "ls", "rs",
                                         "a", "b", "x", "y", "black", "white", "lt", "rt"};
        for (size_t i = 0; i < 16; i++) s.buttons[i] = (*b)[kNames[i]].value_or(s.buttons[i]);
    }
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"frame_interpolation", s.interp},
                  {"language", int64_t(s.language)},
                  {"volume", int64_t(s.volume)},
                  {"upscaler", s.upscaler},
                  {"upscaler_mode", s.upscaler_mode},
                  {"upscaler_sharpness", int64_t(s.upscaler_sharpness)},
                  {"deadzone", int64_t(s.deadzone)},
                  {"game_path", s.game_path},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

bool Widescreen() {
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<bool> wide{true};
    const Settings& s = Get();
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    if (key.exchange(k) != k) {
        Mode m = Output();
        wide = m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide;
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    // with temporal anti-aliasing the mode sets the render size: the output's times its ratio
    const std::string& up = Get().upscaler;
    if (!up.empty() && up != "off") {
        const std::string& m = Get().upscaler_mode;
        float ratio = m == "quality" ? 1 / 1.5f : m == "balanced" ? 1 / 1.7f : m == "performance" ? 0.5f : 1.0f;
        h = Output().height * ratio;
    }
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
