#pragma once

/**
 * @file Signatures.hpp
 * @brief Byte signatures for Minecraft Bedrock arm64-v8a, 26.40 - 26.50.
 *
 * Every pattern below was verified UNIQUE (exactly one match) against all four
 * of these builds with tools/verify_signatures.py:
 *
 *     26.40.x      26.44.3      26.45.1      26.50.x
 *
 *  - GetFov                    LevelRendererPlayer::getFov  float(void*, float, int)
 *  - LocalPlayerApplyTurnDelta LocalPlayer::applyTurnDelta  void(void*, Vec2*)
 *  - BaseOptionRegistryGetHideItemInHand                    bool(void*)
 *  - GetHideHud (option getter, see kGetHideHudPattern* below)
 *
 * Regenerate / re-verify with:
 *      python3 tools/verify_signatures.py <libminecraftpe.so>
 *      python3 tools/derive_pattern.py --lib OLD.so --addr 0xADDR --find-in NEW.so
 */

#include <cstdint>

namespace betterzoom {

// ---------------------------------------------------------------------------
// getFov
// ---------------------------------------------------------------------------
// The plain 7-instruction prologue pattern became AMBIGUOUS on 26.50 (two
// matches: the real getFov and an unrelated function that happens to share the
// identical prologue + trailing FMOV). Those two differ at the instruction
// right after `fmov s8, s0`, so the pattern is extended with the two following
// NON-PC-relative instructions (`mov w20, w1`, `mov x19, x0`), which pins the
// correct function without sacrificing portability:
//   mov w20, w1 = F4 03 01 2A      mov x19, x0 = F3 03 00 AA
// Verified UNIQUE on 26.40 / 26.44.3 / 26.45.1 / 26.50.
// NOTE: the first 6 instructions (24 bytes) are WILDCARDED on purpose.
// Another mod may inline-hook getFov first (BedrockTools' zoom module installs
// its hook in onInit() and never removes it), which overwrites the function
// prologue -- a pattern pinned to the original prologue bytes would then scan
// as MISSING and this mod would silently lose zoom + the cinematic bars.
// Wildcards still match the unpatched prologue, so one pattern covers both.
// Verified UNIQUE (and at the function start) on 26.44.3 / 26.45.1 / 26.50.
inline constexpr const char *kGetFovPattern =
    "? ? ? ? ? ? ? ? ? ? ? ? "
    "? ? ? ? ? ? ? ? ? ? ? ? "
    "08 40 20 1E F4 03 01 2A F3 03 00 AA"
;

// ---------------------------------------------------------------------------
// LocalPlayer::applyTurnDelta
// ---------------------------------------------------------------------------
inline constexpr const char *kApplyTurnDeltaPattern =
    "? ? ? D1 ? ? ? FD ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 "
    "56 D0 3B D5 F3 03 00 AA F4 03 01 AA ? ? ? F9 ? ? ? F8 ? ? ? F9 ? ? ? F9";

// ---------------------------------------------------------------------------
// BaseOptionRegistry::getHideItemInHand (option getter)
// ---------------------------------------------------------------------------
// The option id itself is wildcarded, so this pattern survives option
// renumbering: it still targets hide-item-in-hand after the id moved from 49
// (<= 26.45) to 46 (26.50). Verified UNIQUE on all four builds.
inline constexpr const char *kGetHideItemInHandPattern =
    "? ? ? A9 FD 03 00 91 ? ? ? F9 ? ? ? 52 ? ? ? F9 00 01 3F D6 "
    "? ? ? A8 ? ? ? 14 ? ? ? A9 FD 03 00 91 ? ? ? F9 ? ? ? 52 ? ? ? F9 00 01 3F D6 ? ? ? B4";

// ---------------------------------------------------------------------------
// hide HUD option getter
// ---------------------------------------------------------------------------
// The option id for "hide HUD" moved from 48 to 45 with the 26.50 build, which
// changes the MOVZ immediate inside the getter stub, so one pattern can no
// longer cover every version. Both forms are listed here; ZoomMod picks the
// correct one with a structural check (see kHideHudToHideHandDistance) instead
// of relying on order or on trusting a match blindly.
//
//   option 45: mov w1, #0x2d  ->  A1 05 80 52   (26.50+)
//   option 48: mov w1, #0x30  ->  01 06 80 52   (26.40 - 26.45)
inline constexpr const char *kGetHideHudPatternOption45 =
    "FD 7B BF A9 FD 03 00 91 08 00 40 F9 A1 05 80 52 08 09 40 F9 00 01 3F D6";

inline constexpr const char *kGetHideHudPatternOption48 =
    "FD 7B BF A9 FD 03 00 91 08 00 40 F9 01 06 80 52 08 09 40 F9 00 01 3F D6";

// Distance from the hide-HUD option stub to the hide-item-in-hand option stub.
//
// The option getters are generated as a table of equally shaped stubs, and
// hide HUD sits exactly this far ahead of hide item in hand. MEASURED identical
// on 26.40 (0xa79bb94 -> 0xa79bc04), 26.44.3 (0xa79ce54 -> 0xa79cec4),
// 26.45.1 (0xa79cdb4 -> 0xa79ce24) and 26.50 (0xac09b4c -> 0xac09bbc).
//
// This is used only to VALIDATE a candidate against the independently verified
// hide-item-in-hand address: a candidate that does not satisfy the relation is
// rejected (it belongs to some other option), and if no candidate validates the
// hook is skipped rather than attached to the wrong option.
inline constexpr std::uintptr_t kHideHudToHideHandDistance = 0x70;

} // namespace betterzoom
