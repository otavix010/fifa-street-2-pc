// the windows' icon (title bar, taskbar, Alt+Tab): the game's own, the title image its xbe carries for the console's
// dashboard ($$XTIMAGE, an XPR0 texture in DXT), so the windows have it however the game is started
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "app/app.h"

namespace app {

namespace {

HICON g_icon_big = nullptr, g_icon_small = nullptr;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

void Rgb565(uint16_t v, uint8_t* c) {
    c[0] = uint8_t((v >> 11 & 31) * 255 / 31);
    c[1] = uint8_t((v >> 5 & 63) * 255 / 63);
    c[2] = uint8_t((v & 31) * 255 / 31);
}

// DXT1, DXT3 or DXT5 blocks into RGBA
bool Dxt(const uint8_t* p, size_t avail, int w, int h, int kind, std::vector<uint8_t>& rgba) {
    size_t block = kind == 1 ? 8 : 16;
    if (size_t((w + 3) / 4) * size_t((h + 3) / 4) * block > avail) return false;
    rgba.assign(size_t(w) * h * 4, 255);
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            uint8_t alpha[16];
            std::memset(alpha, 255, sizeof alpha);
            if (kind == 3) {
                for (int i = 0; i < 16; i++) alpha[i] = uint8_t(((p[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                p += 8;
            } else if (kind == 5) {
                uint8_t a0 = p[0], a1 = p[1], al[8] = {a0, a1};
                if (a0 > a1) {
                    for (int i = 0; i < 6; i++) al[2 + i] = uint8_t(((6 - i) * a0 + (i + 1) * a1) / 7);
                } else {
                    for (int i = 0; i < 4; i++) al[2 + i] = uint8_t(((4 - i) * a0 + (i + 1) * a1) / 5);
                    al[6] = 0;
                    al[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(p[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = al[(bits >> (3 * i)) & 7];
                p += 8;
            }
            uint16_t c0 = Le16(p), c1 = Le16(p + 2);
            uint32_t bits = Le32(p + 4);
            p += 8;
            uint8_t col[4][3];
            Rgb565(c0, col[0]);
            Rgb565(c1, col[1]);
            bool four = c0 > c1 || kind != 1;
            for (int k = 0; k < 3; k++) {
                col[2][k] = uint8_t(four ? (2 * col[0][k] + col[1][k]) / 3 : (col[0][k] + col[1][k]) / 2);
                col[3][k] = uint8_t(four ? (col[0][k] + 2 * col[1][k]) / 3 : 0);
            }
            for (int i = 0; i < 16; i++) {
                int x = bx + i % 4, y = by + i / 4;
                if (x >= w || y >= h) continue;
                uint32_t pick = bits >> (2 * i) & 3;
                uint8_t* d = &rgba[(size_t(y) * w + x) * 4];
                d[0] = col[pick][0];
                d[1] = col[pick][1];
                d[2] = col[pick][2];
                d[3] = !four && pick == 3 ? 0 : alpha[i];
            }
        }
    return true;
}

// the xbe's title image as RGBA; false when it has none or in a format not met yet
bool TitleImage(const std::vector<uint8_t>& xbe, int& w, int& h, std::vector<uint8_t>& rgba) {
    if (xbe.size() < 0x180 || std::memcmp(xbe.data(), "XBEH", 4) != 0) return false;
    uint32_t base = Le32(&xbe[0x104]), count = Le32(&xbe[0x11C]), headers = Le32(&xbe[0x120]) - base;
    for (uint32_t i = 0; i < count; i++) {
        size_t s = size_t(headers) + size_t(i) * 56;
        if (s + 56 > xbe.size()) return false;
        uint32_t raw = Le32(&xbe[s + 12]), size = Le32(&xbe[s + 16]), name = Le32(&xbe[s + 20]) - base;
        if (size_t(name) + 10 > xbe.size() || std::memcmp(&xbe[name], "$$XTIMAGE", 10) != 0) continue;
        if (size_t(raw) + size > xbe.size() || size < 32) return false;
        // XPR0: the resource's header (common, data, lock, format, size) after the file's, its texels after the headers
        const uint8_t* x = &xbe[raw];
        if (std::memcmp(x, "XPR0", 4) != 0) return false;
        uint32_t header = Le32(x + 8), data = Le32(x + 16), format = Le32(x + 24);
        uint32_t f = format >> 8 & 0xFF;
        int kind = f == 0x0C ? 1 : f == 0x0E ? 3 : f == 0x0F ? 5 : 0;
        w = 1 << (format >> 20 & 15);
        h = 1 << (format >> 24 & 15);
        if (!kind || size_t(header) + data >= size) return false;
        return Dxt(x + header + data, size - header - data, w, h, kind, rgba);
    }
    return false;
}

// the picture at size x size, each pixel the mean of the source pixels it covers
HICON MakeIcon(const std::vector<uint8_t>& rgba, int w, int h, int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    auto* d = static_cast<uint8_t*>(bits);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int x0 = x * w / size, x1 = (x + 1) * w / size, y0 = y * h / size, y1 = (y + 1) * h / size;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            uint32_t sum[4] = {}, n = 0;
            for (int sy = y0; sy < y1 && sy < h; sy++)
                for (int sx = x0; sx < x1 && sx < w; sx++, n++)
                    for (int k = 0; k < 4; k++) sum[k] += rgba[(size_t(sy) * w + sx) * 4 + k];
            if (!n) n = 1;
            uint8_t* o = d + (size_t(y) * size + x) * 4;
            o[0] = uint8_t(sum[2] / n);  // BGRA
            o[1] = uint8_t(sum[1] / n);
            o[2] = uint8_t(sum[0] / n);
            o[3] = uint8_t(sum[3] / n);
        }
    std::vector<uint8_t> zeros(size_t((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

// the picture at size x size as an icon image of an .ico or of an executable's resources: a 32-bit bitmap, bottom-up,
// its height doubled in the header for the (empty) mask that follows
std::vector<uint8_t> IconImage(const std::vector<uint8_t>& rgba, int w, int h, int size) {
    size_t mask_row = size_t((size + 31) / 32) * 4;
    std::vector<uint8_t> out(sizeof(BITMAPINFOHEADER) + size_t(size) * size * 4 + mask_row * size, 0);
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof bi;
    bi.biWidth = size;
    bi.biHeight = size * 2;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biSizeImage = DWORD(size_t(size) * size * 4 + mask_row * size);
    std::memcpy(out.data(), &bi, sizeof bi);
    uint8_t* d = out.data() + sizeof bi;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int x0 = x * w / size, x1 = (x + 1) * w / size, y0 = y * h / size, y1 = (y + 1) * h / size;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            uint32_t sum[4] = {}, n = 0;
            for (int sy = y0; sy < y1 && sy < h; sy++)
                for (int sx = x0; sx < x1 && sx < w; sx++, n++)
                    for (int k = 0; k < 4; k++) sum[k] += rgba[(size_t(sy) * w + sx) * 4 + k];
            if (!n) n = 1;
            uint8_t* o = d + (size_t(size - 1 - y) * size + x) * 4;
            o[0] = uint8_t(sum[2] / n);  // BGRA
            o[1] = uint8_t(sum[1] / n);
            o[2] = uint8_t(sum[0] / n);
            o[3] = uint8_t(sum[3] / n);
        }
    return out;
}

// The game's icon in the executable file itself (Explorer, shortcuts), written on the player's PC from their copy of
// the game: the port ships without it, since the picture is the game's. A running executable cannot be written, but
// it can be renamed: a copy gets the icon and takes the name, the running one stays aside until the next start.
void StampExecutable(const std::vector<uint8_t>& rgba, int w, int h) {
    HMODULE self = GetModuleHandleW(nullptr);
    wchar_t path[MAX_PATH * 2];
    DWORD len = GetModuleFileNameW(self, path, DWORD(std::size(path)));
    if (!len || len >= std::size(path)) return;
    std::wstring exe = path, aside = exe + L".antigo", fresh = exe + L".novo";
    DeleteFileW(aside.c_str());  // the one left by the start that wrote the icon
    DeleteFileW(fresh.c_str());
    if (FindResourceW(self, MAKEINTRESOURCEW(1), RT_GROUP_ICON) || Instance() > 1) return;
    // not in the folder it was built in (build.ninja beside it): the package is made from that executable, and it ships
    // without anything of the game
    std::wstring ninja = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"build.ninja";
    if (GetFileAttributesW(ninja.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (!CopyFileW(exe.c_str(), fresh.c_str(), FALSE)) return;
    bool ok = false;
    if (HANDLE update = BeginUpdateResourceW(fresh.c_str(), FALSE)) {
        const int sizes[] = {16, 32, 48, 64, 128};
        std::vector<uint8_t> group(6);
        group[2] = 1;  // an icon
        WORD count = 0;
        ok = true;
        for (int size : sizes) {
            if (size > w && size > 48) continue;  // nothing above the picture's own size, but the usual three
            std::vector<uint8_t> image = IconImage(rgba, w, h, size);
            WORD id = WORD(count + 1);
            ok = ok && UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(id), 0x0409, image.data(), DWORD(image.size()));
            uint8_t entry[14] = {uint8_t(size), uint8_t(size), 0, 0, 1, 0, 32, 0};
            DWORD bytes = DWORD(image.size());
            std::memcpy(entry + 8, &bytes, 4);
            std::memcpy(entry + 12, &id, 2);
            group.insert(group.end(), entry, entry + 14);
            count++;
        }
        std::memcpy(group.data() + 4, &count, 2);
        ok = ok && UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(1), 0x0409, group.data(), DWORD(group.size()));
        ok = EndUpdateResourceW(update, !ok) && ok;
    }
    if (ok && MoveFileExW(exe.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (MoveFileExW(fresh.c_str(), exe.c_str(), 0)) return;  // the icon is in; the old file goes at the next start
        MoveFileExW(aside.c_str(), exe.c_str(), 0);
    }
    DeleteFileW(fresh.c_str());
}

}  // namespace

void MakeWindowIcons(const std::vector<uint8_t>& xbe) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!TitleImage(xbe, w, h, rgba) || w <= 0 || h <= 0) return;
    StampExecutable(rgba, w, h);
    g_icon_big = MakeIcon(rgba, w, h, GetSystemMetrics(SM_CXICON));
    g_icon_small = MakeIcon(rgba, w, h, GetSystemMetrics(SM_CXSMICON));
}

void* WindowIcon(bool little) {
    HICON icon = little ? g_icon_small : g_icon_big;
    // an icon in the executable itself, when a build has one
    return icon ? icon : LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
}

}  // namespace app
