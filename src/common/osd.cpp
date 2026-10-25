// The performance meter, the Otavix ports' standard (otavix_osd.c, the same in every port: one line in a window of its
// own over the game's, sized for 1080 lines whatever the internal resolution). OTAVIX_OSD=1 (the launcher's Settings >
// Performance) turns it on, F10 shows or hides it:
//   "FPS 120.0 8.3 ms   CPU 31% 65°C   GPU 40% 60°C   VRAM 1.2/16.0 GB   RAM 15.4/31.9 GB"
// The frame rate is the runtime's count of presents, the in-between frames included; a temperature Windows does not
// give is left out.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "osd_stats.h"
#include "otavix_osd.h"

namespace game {

namespace {

std::atomic<bool> g_shown{true};
std::atomic<bool> g_toggled{false};

bool Wanted() {
    static const bool on = [] {
        const char* v = std::getenv("OTAVIX_OSD");
        return v && v[0] == '1';
    }();
    return on;
}

}  // namespace

// window thread; F10 is a system key, and its default handling would put the window in menu mode, so its messages
// stop here
bool OsdMessage(void*, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (!Wanted() || wp != VK_F10) return false;
    if (msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP && msg != WM_KEYDOWN && msg != WM_KEYUP) return false;
    if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && !(lp & (1 << 30))) {  // bit 30: auto-repeat
        g_shown = !g_shown;
        g_toggled = true;
    }
    *result = 0;
    return true;
}

// renderer, each frame
void OsdUi(double fps) {
    if (!Wanted()) return;
    using Clock = std::chrono::steady_clock;
    static const bool started = [] {
        otx_osd_iniciar();
        return true;
    }();
    (void)started;
    static Clock::time_point last{};
    bool shown = g_shown;
    port::osd::Stats stats = port::osd::Latest(shown);
    if (g_toggled.exchange(false)) {
        last = {};
        if (!shown) otx_osd_texto(nullptr);
    }
    auto now = Clock::now();
    if (!shown || now - last < std::chrono::milliseconds(500)) return;
    last = now;
    char line[192];
    int n = fps > 0 ? std::snprintf(line, sizeof(line), "FPS %.1f %.1f ms", fps, 1000.0 / fps)
                    : std::snprintf(line, sizeof(line), "FPS --");
    auto part = [&](const char* name, float use, float temp) {
        if (use < 0) return;
        n += std::snprintf(line + n, sizeof(line) - size_t(n), "   %s %d%%", name, int(use + 0.5f));
        if (temp >= 0) n += std::snprintf(line + n, sizeof(line) - size_t(n), " %d\xC2\xB0" "C", int(temp + 0.5f));
    };
    part("CPU", stats.cpu, stats.cpu_temp);
    part("GPU", stats.gpu, stats.gpu_temp);
    if (stats.vram_used_mb >= 0)
        n += std::snprintf(line + n, sizeof(line) - size_t(n), "   VRAM %.1f/%.1f GB", stats.vram_used_mb / 1024.0,
                           stats.vram_budget_mb / 1024.0);
    if (stats.ram_used_mb >= 0)
        n += std::snprintf(line + n, sizeof(line) - size_t(n), "   RAM %.1f/%.1f GB", stats.ram_used_mb / 1024.0,
                           stats.ram_total_mb / 1024.0);
    otx_osd_texto(line);
}

}  // namespace game
