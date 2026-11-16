// keyboard and mouse as a controller: the keys held, fed by the window thread, read into the console controller's report
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace input {

namespace {

// A key is its scan code (set 1, as the window messages carry it), plus 0x100 with the extended bit; the mouse's
// buttons come after them.
constexpr uint32_t kExtended = 0x100, kMouse = 0x200, kCodes = 0x208;

constexpr std::pair<const char*, uint32_t> kNames[] = {
    {"escape", 0x01}, {"esc", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07},
    {"7", 0x08}, {"8", 0x09}, {"9", 0x0A}, {"0", 0x0B}, {"minus", 0x0C}, {"equals", 0x0D}, {"backspace", 0x0E},
    {"tab", 0x0F}, {"q", 0x10}, {"w", 0x11}, {"e", 0x12}, {"r", 0x13}, {"t", 0x14}, {"y", 0x15}, {"u", 0x16},
    {"i", 0x17}, {"o", 0x18}, {"p", 0x19}, {"lbracket", 0x1A}, {"rbracket", 0x1B}, {"enter", 0x1C}, {"lctrl", 0x1D},
    {"a", 0x1E}, {"s", 0x1F}, {"d", 0x20}, {"f", 0x21}, {"g", 0x22}, {"h", 0x23}, {"j", 0x24}, {"k", 0x25},
    {"l", 0x26}, {"semicolon", 0x27}, {"apostrophe", 0x28}, {"grave", 0x29}, {"lshift", 0x2A}, {"backslash", 0x2B},
    {"z", 0x2C}, {"x", 0x2D}, {"c", 0x2E}, {"v", 0x2F}, {"b", 0x30}, {"n", 0x31}, {"m", 0x32}, {"comma", 0x33},
    {"period", 0x34}, {"slash", 0x35}, {"rshift", 0x36}, {"nummul", 0x37}, {"lalt", 0x38}, {"space", 0x39},
    {"capslock", 0x3A}, {"f1", 0x3B}, {"f2", 0x3C}, {"f3", 0x3D}, {"f4", 0x3E}, {"f5", 0x3F}, {"f6", 0x40},
    {"f7", 0x41}, {"f8", 0x42}, {"f9", 0x43}, {"f10", 0x44}, {"num7", 0x47}, {"num8", 0x48}, {"num9", 0x49},
    {"numsub", 0x4A}, {"num4", 0x4B}, {"num5", 0x4C}, {"num6", 0x4D}, {"numadd", 0x4E}, {"num1", 0x4F},
    {"num2", 0x50}, {"num3", 0x51}, {"num0", 0x52}, {"numdec", 0x53}, {"iso", 0x56}, {"f11", 0x57}, {"f12", 0x58},
    {"abnt", 0x73}, {"numenter", kExtended | 0x1C}, {"rctrl", kExtended | 0x1D}, {"numdiv", kExtended | 0x35},
    {"ralt", kExtended | 0x38}, {"home", kExtended | 0x47}, {"up", kExtended | 0x48}, {"pageup", kExtended | 0x49},
    {"left", kExtended | 0x4B}, {"right", kExtended | 0x4D}, {"end", kExtended | 0x4F}, {"down", kExtended | 0x50},
    {"pagedown", kExtended | 0x51}, {"insert", kExtended | 0x52}, {"delete", kExtended | 0x53},
    {"mouse1", kMouse | 1}, {"mouse2", kMouse | 2}, {"mouse3", kMouse | 3}, {"mouse4", kMouse | 4},
    {"mouse5", kMouse | 5}};

std::atomic<uint64_t> g_held[(kCodes + 63) / 64];
std::atomic<bool> g_taken{false};
std::atomic<int> g_dx{0}, g_dy{0};

// the layout: the codes of each of the report's 24 inputs, and whether a code is in it at all
struct Layout {
    std::array<std::vector<uint32_t>, 24> codes;
    std::array<bool, kCodes> used{};
    int port = -1;  // none: the keyboard is off
    bool mouse = false;
    float sensitivity = 1.0f;
};

const Layout& Get() {
    static const Layout layout = [] {
        Layout l;
        const app::Settings& s = app::Get();
        if (!s.keyboard) return l;
        l.port = int(s.keyboard_port) - 1;
        l.mouse = s.mouse;
        l.sensitivity = s.mouse_sensitivity / 50.0f;
        for (size_t i = 0; i < 24; i++) {
            std::istringstream words(s.keys[i]);
            for (std::string word; words >> word;) {
                std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                auto it = std::find_if(std::begin(kNames), std::end(kNames), [&](const auto& n) { return word == n.first; });
                if (it == std::end(kNames)) {
                    LOGR("input: no key is called %s", word.c_str());
                    continue;
                }
                if (it->second >= kMouse && !s.mouse) continue;
                l.codes[i].push_back(it->second);
                l.used[it->second] = true;
            }
        }
        LOGR("input: keyboard%s on port %d", l.mouse ? " and mouse" : "", l.port + 1);
        return l;
    }();
    return layout;
}

void Set(uint32_t code, bool down) {
    if (code >= kCodes) return;
    const Layout& l = Get();
    if (l.port < 0) return;
    uint64_t bit = 1ull << (code & 63);
    if (down) g_held[code >> 6].fetch_or(bit, std::memory_order_relaxed);
    else g_held[code >> 6].fetch_and(~bit, std::memory_order_relaxed);
    if (down && l.used[code] && l.mouse) g_taken.store(true, std::memory_order_relaxed);
}

bool Held(const std::vector<uint32_t>& codes) {
    for (uint32_t c : codes)
        if (g_held[c >> 6].load(std::memory_order_relaxed) >> (c & 63) & 1) return true;
    return false;
}

// The mouse as the right stick: its movement pushes the stick, which falls back to the centre by itself, so a flick
// of the mouse is a flick of the stick and a steady movement holds it tilted.
std::mutex g_stick_lock;
float g_sx = 0, g_sy = 0;
std::chrono::steady_clock::time_point g_stick_time;

void MouseStick(const Layout& l, float& x, float& y) {
    constexpr float kFall = 0.07f;      // seconds for the stick to fall to about a third
    constexpr float kCount = 1 / 90.0f; // of full tilt for each count moved, at 50% sensitivity
    std::lock_guard lock(g_stick_lock);
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_stick_time).count();
    g_stick_time = now;
    float k = dt > 0 && dt < 1 ? std::exp(-dt / kFall) : 0.0f;
    g_sx = g_sx * k + g_dx.exchange(0, std::memory_order_relaxed) * kCount * l.sensitivity;
    g_sy = g_sy * k - g_dy.exchange(0, std::memory_order_relaxed) * kCount * l.sensitivity;
    // kept a little past full tilt, so that a strong movement holds it there for a moment
    if (float m = std::sqrt(g_sx * g_sx + g_sy * g_sy); m > 1.5f) {
        g_sx *= 1.5f / m;
        g_sy *= 1.5f / m;
    }
    x = g_sx;
    y = g_sy;
}

// a direction of length 1 at most into the stick's range
void Tilt(float x, float y, int16_t& sx, int16_t& sy) {
    if (float m = std::sqrt(x * x + y * y); m > 1) {
        x /= m;
        y /= m;
    }
    sx = int16_t(std::lround(x * 32767.0f));
    sy = int16_t(std::lround(y * 32767.0f));
}

}  // namespace

void Key(uint32_t scan, bool extended, bool down) {
    if (scan == 0 || scan >= 0x100) return;
    Set(scan | (extended ? kExtended : 0), down);
}

void MouseButton(int button, bool down) {
    if (button >= 1 && button <= 5) Set(kMouse | uint32_t(button), down);
}

void MouseMove(int dx, int dy) {
    if (!g_taken.load(std::memory_order_relaxed)) return;
    g_dx.fetch_add(dx, std::memory_order_relaxed);
    g_dy.fetch_add(dy, std::memory_order_relaxed);
}

void ReleaseAll() {
    for (auto& word : g_held) word.store(0, std::memory_order_relaxed);
    g_taken.store(false, std::memory_order_relaxed);
    g_dx.store(0, std::memory_order_relaxed);
    g_dy.store(0, std::memory_order_relaxed);
}

bool MouseTaken() { return g_taken.load(std::memory_order_relaxed); }

void Merge(int port, Pad& pad) {
    const Layout& l = Get();
    if (port != l.port) return;
    pad.connected = true;
    for (size_t i = 0; i < 8; i++)
        if (Held(l.codes[i])) pad.buttons |= uint16_t(1 << i);
    for (size_t i = 0; i < 8; i++)
        if (Held(l.codes[8 + i])) pad.analog[i] = 0xFF;
    // a stick the keys or the mouse move is theirs, else the pad's
    auto dir = [&](size_t i) { return float(Held(l.codes[i + 3])) - float(Held(l.codes[i + 2])); };
    float lx = dir(16), ly = float(Held(l.codes[16])) - float(Held(l.codes[17]));
    if (lx != 0 || ly != 0) Tilt(lx, ly, pad.lx, pad.ly);
    float rx = dir(20), ry = float(Held(l.codes[20])) - float(Held(l.codes[21]));
    if (rx == 0 && ry == 0 && l.mouse) {
        MouseStick(l, rx, ry);
        if (std::sqrt(rx * rx + ry * ry) < 0.12f) rx = ry = 0;
    }
    if (rx != 0 || ry != 0) Tilt(rx, ry, pad.rx, pad.ry);
    // development (FS2_LOG): the report each time the keys change it, the sticks in eighths
    static std::atomic<uint64_t> last{0};
    uint64_t now = pad.buttons;
    for (size_t i = 0; i < 8; i++) now |= uint64_t(pad.analog[i] >= 0x80) << (8 + i);
    for (int16_t v : {pad.lx, pad.ly, pad.rx, pad.ry}) now = now << 5 | uint64_t((v + 32768) >> 12);
    if (last.exchange(now, std::memory_order_relaxed) != now)
        LOGI("input: keys %02X analog %02X left %d %d right %d %d", pad.buttons, unsigned(now >> 28 & 0xFF), pad.lx, pad.ly,
             pad.rx, pad.ry);
}

}  // namespace input
