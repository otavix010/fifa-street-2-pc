// Hooks for default.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }

#include <atomic>
#include <cstdlib>
#include <initializer_list>
#include <iterator>

#include "core/log.h"
#include "cpu/x86_ops.h"

namespace {
// calls the game's function at va as the game does (ecx for a method, stack arguments pushed right to left) on the
// guest stack below the hook's frame, and puts every register back after it; returns eax
uint32_t GameCall(X86Context& ctx, uint8_t* base, uint32_t va, uint32_t ecx, std::initializer_list<uint32_t> args) {
    const X86Context saved = ctx;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) X86_PUSH32(*it);
    X86_PUSH32(0u);  // the callee's ret pops it; nothing jumps there
    ctx.ecx = ecx;
    x86_icall(ctx, base, va);
    uint32_t eax = ctx.eax;
    ctx = saved;
    return eax;
}
}  // namespace

// The scene of a match (vtable 0x2C1260, the object the reports call "Player 822DA880") gets its stadium camera at
// [scene+0x62C] from sub_000F2490, which sub_000F1790 fills with a 0x1EC object it builds with sub_00086A00 while it
// loads the stadium ("FifastCameraPosition"). When that load stops early the field stays null, and sub_0013F1A0, which
// the scene's constructor sub_000AA7C0 calls right after (0xAB33F), reads [[scene+0x62C]+0x48] with no check
// (0x13F1E9: reports 1516, 1517 and 1521, all the same player, at 105 s, 503 s and 2729 s). [[hook]] at 0x13F1E3 in
// config/game.toml: a scene without one gets a camera the game's own way, its allocator sub_0015B660 and the
// constructor sub_00086A00, so every later reader finds the fields and pointers it expects (a block of zeros got past
// 0x13F1E9 and then crashed 5 s later on a pointer inside it, bench of 27/11). FS2_TEST_NO_SCENE_CAMERA=1 drops the
// camera the game made, for the bench.
void FifaSceneCamera(X86Context& ctx, uint8_t* base) {
    uint32_t scene = ctx.ecx;
    static const bool test = std::getenv("FS2_TEST_NO_SCENE_CAMERA") != nullptr;
    if (test && X86_MEM32(scene + 0x62Cu)) {
        LOGR("FS2_TEST_NO_SCENE_CAMERA: dropping the camera %08X of scene %08X", X86_MEM32(scene + 0x62Cu), scene);
        X86_MEM32(scene + 0x62Cu) = 0;
    }
    if (X86_MEM32(scene + 0x62Cu)) return;
    uint32_t camera = GameCall(ctx, base, 0x15B660u, ctx.ecx, {0x1ECu, 0x08u, 0u});
    if (camera) camera = GameCall(ctx, base, 0x86A00u, camera, {});
    static std::atomic<uint32_t> told{0};
    if (told.fetch_add(1) < 8)
        LOGR("Scene %08X has no stadium camera (+0x62C null): %s %08X, reports 1516/1517/1521", scene,
             camera ? "made a default one at" : "could not make one,", camera);
    if (camera) X86_MEM32(scene + 0x62Cu) = camera;
}

// sub_0004FBA0 (from the player's constructor sub_000AA7C0 by way of sub_00050ED0) scales the player's body matrix by
// the "cpfa" block of cpfa%04d.xsh, which [player+0x62C] holds by the appearance id. With a third party roster that
// block was never loaded and 0x4FDE6 read through a null pointer (reports 550/551). [[hook]] at 0x4FDE0 in
// config/game.toml: true skips to 0x4FEDA, the path the game takes for any other [player+0x5AC], so the matrix keeps
// the zeros 0x4FD3E..0x4FDCC stored.
bool FifaCpfaMissing(X86Context& ctx, uint8_t* base) {
    uint32_t player = ctx.eax;
    if (X86_MEM32(player + 0x62Cu)) return false;
    static std::atomic<bool> told{false};
    if (!told.exchange(true))
        LOGR("Player %08X has no appearance block (+0x62C null, +0x5AC %u): scale skipped, reports 550/551", player,
             X86_MEM32(player + 0x5ACu));
    return true;
}
