// the host: window, message loop, crash dumps; the game is described by app::kGame
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cwchar>
#include <thread>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace app {

namespace {

HWND g_hwnd = nullptr;
constexpr UINT kApplyDisplay = WM_APP + 1, kOpenWindow = WM_APP + 2;
bool g_mode_changed = false;
bool g_apply_posted = false;  // a display apply is waiting in the queue or running

// "FIFAStreet2.exe +1A2B3C": the module and offset of a code address, to look up in that module's map
void LogCodeAddress(const char* what, const void* a) {
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(a), &mod))
        GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* name = std::wcsrchr(path, L'\\') ? std::wcsrchr(path, L'\\') + 1 : path;
    LOGE("  %s %ls +%llX", what, mod ? name : L"?",
         (unsigned long long)(reinterpret_cast<uintptr_t>(a) - reinterpret_cast<uintptr_t>(mod)));
}

// crash dump in CrashDumps beside the exe
LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* e) {
    // the dump holds no guest memory: the guest registers and return addresses go to the log, from any thread (the
    // game's, or the port's own: the renderer, the audio)
    if (e) {
        auto* r = e->ExceptionRecord;
        kernel::Thread* t = kernel::Current();
        if (t) LOGE("exception %08lX at %p on guest thread %X", r->ExceptionCode, r->ExceptionAddress, t->id);
        else LOGE("exception %08lX at %p on host thread %lu", r->ExceptionCode, r->ExceptionAddress, GetCurrentThreadId());
        LogCodeAddress("in", r->ExceptionAddress);
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
            uint64_t va = r->ExceptionInformation[1], g = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
            LOGE("  %s %llX, guest %08llX", r->ExceptionInformation[0] ? "write" : "read", (unsigned long long)va,
                 (unsigned long long)(va - g));
        }
        // the host calls that led here (this filter runs on the faulting thread, below the exception dispatcher)
        void* frames[48];
        USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < n; i++) LogCodeAddress("from", frames[i]);
        if (t && t->ctx) {
            kernel::LogGuestStack(*t->ctx, 256);
            kernel::LogGuestMemory(*t->ctx);
        }
        d3d::LogPush();
    }
    auto dir = ExeDir() / "CrashDumps";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"%ls.%04u.%02u.%02u_%02u.%02u.%02u.dmp", kGame.name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    HANDLE f = CreateFileW((dir / name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), e, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MINIDUMP_TYPE(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory),
                      e ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    logx::Flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
}

// window thread only
void ApplyNow() {
    const Settings& s = Get();
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    if (g_mode_changed) {
        ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
        g_mode_changed = false;
    }
    Mode out = Output();
    LONG_PTR visible = IsWindowVisible(g_hwnd) ? WS_VISIBLE : 0;  // a window held back for a splash or handover stays so
    UINT activate = TestWindow() ? SWP_NOACTIVATE : 0;
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &desktop);
    // the desktop's own size is no mode change: switching to it anyway blanks the screen for nothing
    if (s.display_mode == DisplayMode::Fullscreen && s.width && s.height &&
        (s.width != desktop.dmPelsWidth || s.height != desktop.dmPelsHeight)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = s.width;
        dm.dmPelsHeight = s.height;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        g_mode_changed = ChangeDisplaySettingsExW(mi.szDevice, &dm, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL;
        GetMonitorInfoW(mon, &mi);
    }
    if (s.display_mode == DisplayMode::Windowed) {
        RECT r{0, 0, LONG(out.width), LONG(out.height)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top;
        const RECT& a = mi.rcWork;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, a.left + (a.right - a.left - w) / 2, a.top + (a.bottom - a.top - h) / 2, w, h,
                     SWP_FRAMECHANGED | activate);
    } else {
        const RECT& m = mi.rcMonitor;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | visible);
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | activate);
    }
}

// Keyboard and mouse as a controller: the keys and buttons held and the mouse's movement go to input, and while the
// mouse plays the cursor is hidden and stays inside the window. true takes the message.
constexpr UINT kTestMouse = WM_APP + 3;  // development: a mouse movement posted to a test window, x in wp, y in lp
bool g_clipped = false;

void ClipToWindow(HWND hwnd) {
    RECT r;
    GetClientRect(hwnd, &r);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    g_clipped = ClipCursor(&r) != 0;
}

bool InputMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    constexpr WPARAM kButtons = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;
    int button = 0;
    bool down = false;
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            // the keys come raw (WM_INPUT), as these messages lose a held Shift when a numpad key goes down with
            // Num Lock on; a test window, which never has the keyboard, takes them from the messages posted to it
            if (!TestWindow()) return false;
            input::Key(uint32_t(lp >> 16) & 0xFF, (lp >> 24) & 1, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            break;
        case WM_LBUTTONDOWN: button = 1, down = true; break;
        case WM_LBUTTONUP: button = 1; break;
        case WM_RBUTTONDOWN: button = 2, down = true; break;
        case WM_RBUTTONUP: button = 2; break;
        case WM_MBUTTONDOWN: button = 3, down = true; break;
        case WM_MBUTTONUP: button = 3; break;
        case WM_XBUTTONDOWN: button = HIWORD(wp) == XBUTTON1 ? 4 : 5, down = true; break;
        case WM_XBUTTONUP: button = HIWORD(wp) == XBUTTON1 ? 4 : 5; break;
        case WM_INPUT: {
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
                return false;
            if (raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                input::MouseMove(raw.data.mouse.lLastX, raw.data.mouse.lLastY);
                return false;
            }
            const RAWKEYBOARD& key = raw.data.keyboard;
            if (raw.header.dwType != RIM_TYPEKEYBOARD || (key.Flags & RI_KEY_E1) || TestWindow()) return false;
            input::Key(key.MakeCode, (key.Flags & RI_KEY_E0) != 0, !(key.Flags & RI_KEY_BREAK));
            break;
        }
        case kTestMouse:
            if (TestWindow()) input::MouseMove(int(wp), int(lp));
            return true;
        case WM_KILLFOCUS:
            input::ReleaseAll();
            if (g_clipped) ClipCursor(nullptr);
            g_clipped = false;
            return false;
        case WM_WINDOWPOSCHANGED:
            if (g_clipped) ClipToWindow(hwnd);
            return false;
        case WM_SETCURSOR:
            if (!g_clipped || LOWORD(lp) != HTCLIENT) return false;
            SetCursor(nullptr);
            result = TRUE;
            return true;
        case WM_SYSCOMMAND:
            // Alt on its own would open the window menu, which a game window does not have, and hold the keys back
            if ((wp & 0xFFF0) != SC_KEYMENU || lp != 0) return false;
            result = 0;
            return true;
        default:
            return false;
    }
    if (button) {
        input::MouseButton(button, down);
        // every release reaches the window, also outside it
        if (down) SetCapture(hwnd);
        else if (!(wp & kButtons)) ReleaseCapture();
    }
    if (input::MouseTaken() && !g_clipped && !TestWindow() && GetForegroundWindow() == hwnd) {
        ClipToWindow(hwnd);
        SetCursor(nullptr);
    }
    return false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (LRESULT result = 0; InputMessage(hwnd, msg, wp, lp, result)) return result;
    {
        std::lock_guard lock(UiLock());
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    }
    if (intptr_t result = 0; kGame.message && kGame.message(hwnd, msg, wp, lp, &result)) return result;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_F3) DumpThreads();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                // a changed display mode steps aside for the desktop, minimized, and comes back on return; not while
                // handing over to the next instance, which runs in the same mode
                if (g_mode_changed && !kernel::g_relaunching) {
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
                    ChangeDisplaySettingsExW(mi.szDevice, nullptr, nullptr, 0, nullptr);
                    g_mode_changed = false;
                    ShowWindow(hwnd, SW_MINIMIZE);
                }
            } else if (Get().display_mode == DisplayMode::Fullscreen && Get().width && Get().height && !g_mode_changed &&
                       IsWindowVisible(hwnd) && !g_apply_posted) {
                // once: restoring the window activates it again from inside the apply, and a second apply switched
                // the mode back and forth, after which presents were not taken for a while (the game stood still
                // until the next alt-tab)
                g_apply_posted = true;
                PostMessageW(hwnd, kApplyDisplay, 0, 0);
            }
            break;
        case kApplyDisplay:
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            ApplyNow();
            g_apply_posted = false;
            return 0;
        case kOpenWindow:
            CloseSplash();
            if (wp) ApplyNow();
            ShowWindow(hwnd, TestWindow() ? SW_SHOWNOACTIVATE : SW_SHOW);
            if (!TestWindow()) SetForegroundWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool TestWindow(int* x, int* y) {
    static int px = 0, py = 0;
    static const bool on = [] {
        const char* v = std::getenv("FS2_TEST_WINDOW");
        return v && std::sscanf(v, "%d,%d", &px, &py) == 2;
    }();
    if (on && x) *x = px;
    if (on && y) *y = py;
    return on;
}

std::filesystem::path ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}

int Instance() {
    static const int n = [] {
        const wchar_t* p = std::wcsstr(GetCommandLineW(), L"--instance");
        return p ? std::max(0, std::min(8, _wtoi(p + 10))) : 0;
    }();
    return n;
}

void* Window() { return g_hwnd; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() { PostMessageW(g_hwnd, kApplyDisplay, 0, 0); }
void OpenGameWindow(bool apply) { SendMessageW(g_hwnd, kOpenWindow, apply, 0); }

namespace {

// the game window's monitor's refresh rate, read again each second (a mode change, a move to another monitor); 0
// when unreadable
uint32_t RefreshRate() {
    static uint32_t rate = 0;
    static ULONGLONG next = 0;
    if (ULONGLONG now = GetTickCount64(); now >= next) {
        next = now + 1000;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        rate = GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi) &&
                       EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
                   ? mode.dmDisplayFrequency
                   : 0;
    }
    return rate;
}

}  // namespace

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    // a few under the refresh rate, so a variable refresh display stays in its range and vsync never waits a whole
    // refresh
    constexpr uint32_t kRefreshMargin = 3;
    if (cap == kFpsCapRefresh) cap = RefreshRate() > kRefreshMargin ? RefreshRate() - kRefreshMargin : 0;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) Sleep(1);
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(app::WriteCrashDump);
    std::signal(SIGABRT, [](int) { app::WriteCrashDump(nullptr); });
    timeBeginPeriod(1);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // one log a session: a relaunch into another image carries on its predecessor's
    bool relaunched = std::wcsstr(GetCommandLineW(), L"--relaunched") != nullptr;
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init((app::ExeDir() / "logs").string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    app::GameFiles game;
    if (!app::FindGame(game)) return 1;
    app::MakeWindowIcons(game.xbe);
    if (!app::PrepareGameCode(game)) return 1;

    bool splash = app::OpenSplash(), handover = app::StartHandover();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = app::WndProc;
    wc.hInstance = inst;
    wc.hIcon = static_cast<HICON>(app::WindowIcon(false));
    wc.hIconSm = static_cast<HICON>(app::WindowIcon(true));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = app::kGame.name;
    RegisterClassExW(&wc);
    int at_x = 640, at_y = 360;  // the window's centre
    app::TestWindow(&at_x, &at_y);
    app::g_hwnd = CreateWindowExW(0, wc.lpszClassName, app::kGame.title, WS_POPUP, at_x - 640, at_y - 360, 1280, 720,
                                  nullptr, nullptr, inst, nullptr);
    // behind a splash the window stays hidden until the game's first frame; handed over, it is set up hidden
    if (handover) {
        app::ApplyNow();
    } else if (!splash) {
        app::ApplyNow();
        ShowWindow(app::g_hwnd, app::TestWindow() ? SW_SHOWNOACTIVATE : SW_SHOW);
    }

    // the keys as the keyboard sends them and the mouse's movement in counts, while the window has the keyboard
    if (app::Get().keyboard) {
        RAWINPUTDEVICE devices[2] = {{0x01, 0x06, 0, app::g_hwnd}, {0x01, 0x02, 0, app::g_hwnd}};  // keyboard, mouse
        RegisterRawInputDevices(devices, app::Get().mouse ? 2 : 1, sizeof(RAWINPUTDEVICE));
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplWin32_Init(app::g_hwnd);

    input::Init();
    if (app::kGame.window) app::kGame.window(app::g_hwnd);
    std::wstring hdd = app::Instance() > 1 ? L"hdd" + std::to_wstring(app::Instance()) : L"hdd";
    if (app::Instance())
        SetWindowTextW(app::g_hwnd, (std::wstring(app::kGame.title) + L" [" + std::to_wstring(app::Instance()) + L"]").c_str());
    // mods: the folders listed in mods/ativos.txt, first wins; OTAVIX_MODS=0 starts the game without them
    {
        std::vector<std::string> mods;
        const char* off = std::getenv("OTAVIX_MODS");
        std::ifstream list(app::ExeDir() / "mods" / "ativos.txt");
        std::string line;
        while (!(off && std::string(off) == "0") && std::getline(list, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            std::filesystem::path d = app::ExeDir() / "mods" / std::filesystem::u8path(line);
            std::error_code ec;
            if (std::filesystem::is_directory(d, ec)) mods.push_back(reinterpret_cast<const char*>(d.u8string().c_str()));
        }
        kernel::MountMods(mods);
    }
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        MessageBoxW(app::g_hwnd, L"The game failed to start, see the log in the logs folder.", app::kGame.short_title,
                    MB_ICONERROR);
        return 1;
    }

    // FS2_DUMP=N: every guest thread's stack in the log every N seconds (development)
    if (const char* d = std::getenv("FS2_DUMP"); d && std::atoi(d) > 0) {
        int secs = std::atoi(d);
        std::thread([secs] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(secs));
                LOGE("thread dump");
                app::DumpThreads();
                logx::Flush();
            }
        }).detach();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (app::g_mode_changed) ChangeDisplaySettingsW(nullptr, 0);
    logx::Flush();
    kernel::Shutdown(0);
}
