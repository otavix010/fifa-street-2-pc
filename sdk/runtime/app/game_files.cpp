// The game in assets: its files, or a disc image (.iso) with this executable's xbe on it. The xbe must be the one
// this executable was recompiled from; another release's would run its own data under this code.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

namespace app {

namespace {

namespace fs = std::filesystem;

// the title in an xbe's certificate (UTF-16, 40 characters at +0x0C), empty when the header does not hold one
std::wstring XbeTitle(const std::vector<uint8_t>& xbe)
{
    auto u32 = [&](size_t at) {
        return at + 4 <= xbe.size() ? uint32_t(xbe[at]) | uint32_t(xbe[at + 1]) << 8 | uint32_t(xbe[at + 2]) << 16 |
                                          uint32_t(xbe[at + 3]) << 24
                                    : 0u;
    };
    if (xbe.size() < 0x178 || u32(0) != 0x48454258u) return {};  // "XBEH"
    const uint32_t base = u32(0x104), cert = u32(0x118);
    if (cert < base) return {};
    const size_t at = size_t(cert - base) + 0x0C;
    if (at + 80 > xbe.size()) return {};
    std::wstring t;
    for (size_t i = 0; i < 40; i++) {
        const wchar_t c = wchar_t(xbe[at + i * 2] | xbe[at + i * 2 + 1] << 8);
        if (!c) break;
        t += c;
    }
    return t;
}

// two titles of the same game: compared on letters and digits only, in lower case ("FIFA Street 2" and "FIFA STREET 2")
bool SameTitle(const std::wstring& a, const std::wstring& b)
{
    auto squash = [](const std::wstring& s) {
        std::wstring o;
        for (wchar_t c : s)
            if (std::iswalnum(c)) o += wchar_t(std::towlower(c));
        return o;
    };
    return squash(a) == squash(b);
}

// the settings' game_path (a folder, or a disc image's folder), else assets beside the exe or in any folder above it
// (the build output)
fs::path FindAssets() {
    std::error_code ec;
    if (!Get().game_path.empty()) {
        fs::path p = fs::u8path(Get().game_path);
        if (fs::is_directory(p, ec)) return p;
        if (fs::is_regular_file(p, ec)) return p.parent_path();
        LOGR("game: game_path %s is not there", Get().game_path.c_str());
    }
    auto dir = ExeDir();
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

}  // namespace

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool FindGame(GameFiles& game) {
    fs::path assets = FindAssets();
    std::vector<fs::path> isos;
    std::error_code ec;
    if (!assets.empty())
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
            if (ext == L".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
    std::sort(isos.begin(), isos.end());
    // a disc image named in the settings goes first
    if (!Get().game_path.empty() && fs::is_regular_file(fs::u8path(Get().game_path), ec)) {
        fs::path chosen = fs::u8path(Get().game_path);
        isos.erase(std::remove(isos.begin(), isos.end(), chosen), isos.end());
        isos.insert(isos.begin(), chosen);
    }

    // the first copy with this xbe wins, the extracted files before any image; another release is kept to offer
    GameFiles other;
    std::unique_ptr<kernel::Disc> other_disc;
    fs::path other_name;
    auto consider = [&](const fs::path& name, std::vector<uint8_t> xbe, std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        bool match = sha == kImageSha1;
        LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, name.string().c_str(), sha.c_str(), match ? "" : ", another release");
        if (match) {
            game = {assets, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            return true;
        }
        if (other_name.empty()) {
            other = {assets, std::move(xbe)};
            other_disc = std::move(disc);
            other_name = name;
        }
        return false;
    };

    std::vector<uint8_t> xbe;
    std::wstring why;
    if (!assets.empty() && ReadHostFile(assets / IMAGE_XBE, xbe) && consider(assets, std::move(xbe), nullptr)) return true;
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        // in the release log and in the message: a release build drops warnings, and report 406 came with no reason
        if (!disc->Open(iso, error)) {
            LOGR("game: %s: %s", iso.string().c_str(), error.c_str());
            if (why.empty()) why = iso.filename().wstring() + L": " + std::wstring(error.begin(), error.end());
            continue;
        }
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGR("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
            if (why.empty()) {
                std::string name = IMAGE_XBE;
                why = iso.filename().wstring() + L": no " + std::wstring(name.begin(), name.end());
            }
            continue;
        }
        if (consider(iso, std::move(xbe), std::move(disc))) return true;
    }

    if (other_name.empty()) {
        LOGR("game: no copy found (assets %s, %zu disc images)", assets.string().c_str(), isos.size());
        std::wstring text = kGame.missing;
        if (!why.empty()) text += L"\n\n" + why;
        MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONERROR);
        return false;
    }
    // another game altogether (a player of FIFA Street 2 picked the first FIFA Street's disc, the launcher's report 39,
    // and was asked in English whether to start "a different release"): said so, and not offered
    std::wstring other_title = XbeTitle(other.xbe);
    LOGR("Game: %s is titled \"%ls\"", other_name.string().c_str(), other_title.c_str());
    if (!other_title.empty() && !SameTitle(other_title, kGame.title)) {
        std::wstring text = L"Esta imagem é de outro jogo: " + other_title + L", não " + kGame.title + L".\n\n" +
                            other_name.filename().wstring() + L"\n\nNo Otavix Launcher, escolha a imagem do disco de " +
                            kGame.title + L" (.iso).\n\nThis image is another game: " + other_title + L", not " +
                            kGame.title + L". In the Otavix Launcher, pick the " + kGame.title + L" disc image (.iso).";
        MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONERROR);
        return false;
    }
    std::wstring text = other_name.filename().wstring() +
                        L"\n\nEsta é outra versão do jogo, diferente da usada neste port, e o mais provável é que ele "
                        L"feche. Abrir mesmo assim?\n\nThis is a different release of the game than this build was made "
                        L"from, and will most likely crash. Start it anyway?";
    if (MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return false;
    game = std::move(other);
    kernel::MountDisc(std::move(other_disc));
    return true;
}

}  // namespace app
