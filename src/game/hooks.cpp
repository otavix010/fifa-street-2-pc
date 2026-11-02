// Hooks for default.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }

#include <atomic>

#include "core/log.h"
#include "cpu/x86_ops.h"

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
