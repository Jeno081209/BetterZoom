#include "ZoomMod.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android/log.h>

#include <pl/memory/Signature.hpp>

#include "Signatures.hpp"

namespace betterzoom {

namespace {

constexpr const char *kModuleId = "betterzoom.zoom";
constexpr const char *kFontId = "betterzoom.font";
constexpr const char *kConfigFileName = "config.json";

constexpr int ACTION_DOWN = 0;
constexpr int ACTION_UP = 1;
constexpr int ACTION_MOVE = 2;
constexpr int ACTION_CANCEL = 3;
constexpr int ACTION_POINTER_DOWN = 5;
constexpr int ACTION_POINTER_UP = 6;

constexpr long long kTapTimeoutMs = 500;
constexpr float kDragSlop = 24.0f; // px: button-finger drag vs tap

// ---------------------------------------------------------------------------
// Language-adaptive strings (the launcher's locale decides zh vs en).
// ---------------------------------------------------------------------------
bool g_langZh = false;
const char *T(const char *en, const char *zh) { return g_langZh ? zh : en; }

// GameActivityMotionEvent layout (verified against the disassembly of
// libminecraftpe.so 26.40.5 and the official game-activity header):
//   pointerCount            @ +56  (uint32)
//   slot(i)                 @ 60 + i * 0xd0
//     id                    @ +0   (int32)
//     axisValues[48]        @ +8   (X = axisValues[0] @ +8, Y = [1] @ +12)
//     rawX / rawY           @ +200 / +204
constexpr uint32_t kGaActionOffset = 8;   // GameActivityMotionEvent.action
constexpr uint32_t kGaCountOffset = 56;
constexpr uint32_t kGaSlotStride = 0xd0;
constexpr uint32_t kGaSlotBase = 60;
constexpr uint32_t kGaXOffset = 8;
constexpr uint32_t kGaYOffset = 12;
constexpr uint32_t kGaRawXOffset = 200;
constexpr uint32_t kGaRawYOffset = 204;

float (*g_getFovOrig)(void *, float, int) = nullptr;
void (*g_applyTurnDeltaOrig)(void *, void *) = nullptr;
bool (*g_getHideItemInHandOrig)(void *) = nullptr;
bool (*g_getHideHudOrig)(void *) = nullptr;
void *(*g_dlopenOrig)(const char *, int) = nullptr;
void (*g_fromJavaOrig)(void *, void *, void *, int32_t, int32_t) = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Static hook trampolines (defined in betterzoom scope so they can be
// ZoomMod member definitions; the g_* original pointers live in the unnamed
// namespace above and stay visible here).
// ---------------------------------------------------------------------------

float ZoomMod::getFovHookFn(void *self, float fov, int enableVariableFOV) {
    float orig = g_getFovOrig ? g_getFovOrig(self, fov, enableVariableFOV)
                              : fov;
    ZoomMod &mod = ZoomMod::instance();
    if (!mod.modEnabled()) return orig;
    const float out = mod.fovHook(orig, enableVariableFOV);
    return out;
}

void ZoomMod::applyTurnDeltaHookFn(void *self, void *deltaPtr) {
    ZoomMod &mod = ZoomMod::instance();
    if (!g_applyTurnDeltaOrig) return;
    if (!deltaPtr) {
        g_applyTurnDeltaOrig(self, deltaPtr);
        return;
    }
    if (mod.modEnabled() && mod.zooming()) {
        float *d = static_cast<float *>(deltaPtr);
        float dx = d[0];
        float dy = d[1];
        mod.applyTurnDelta(dx, dy);
        d[0] = dx;
        d[1] = dy;
    }
    g_applyTurnDeltaOrig(self, deltaPtr);
}

bool ZoomMod::getHideItemInHandHookFn(void *self) {
    bool hide =
        g_getHideItemInHandOrig ? g_getHideItemInHandOrig(self) : false;
    ZoomMod &mod = ZoomMod::instance();
    if (mod.modEnabled() && mod.wantsHideHand()) return true;
    return hide;
}

bool ZoomMod::getHideHudHookFn(void *self) {
    bool hide = g_getHideHudOrig ? g_getHideHudOrig(self) : false;
    ZoomMod &mod = ZoomMod::instance();
    if (mod.modEnabled() && mod.wantsHideHud()) return true;
    return hide;
}

void *ZoomMod::dlopenHookFn(const char *filename, int flags) {
    void *handle = g_dlopenOrig ? g_dlopenOrig(filename, flags) : nullptr;
    if (handle && filename && std::strstr(filename, "libminecraftpe.so")) {
        ZoomMod::instance().onMinecraftLoaded();
    }
    return handle;
}

void ZoomMod::fromJavaHookFn(void *env, void *motionEvent, void *outEvent,
                             int32_t arg4, int32_t arg5) {
    if (g_fromJavaOrig) g_fromJavaOrig(env, motionEvent, outEvent, arg4, arg5);
    // The strip function itself returns immediately when no finger is
    // registered for removal (button finger or zoom-adjust finger), so this
    // covers both hold mode and toggle mode.
    ZoomMod &mod = ZoomMod::instance();
    if (mod.modEnabled()) {
        // Mouse wheel: while zoomed the wheel adjusts the zoom level instead
        // of scrolling the hotbar.
        mod.handleScrollFromGameEvent(outEvent);
        mod.stripButtonFingerFromGameEvent(outEvent);
    }
}

// ---------------------------------------------------------------------------
// Singleton + lifecycle
// ---------------------------------------------------------------------------

ZoomMod &ZoomMod::instance() {
    static ZoomMod mod;
    return mod;
}

bool ZoomMod::load(ll::mod::ModContext &context) {
    m_self = ll::mod::NativeMod::current();
    m_javaVm = context.javaVm();
    ::mkdir(m_self->getConfigDir().c_str(), 0755);
    loadConfig();
    m_settings.store(m_cfg.s, std::memory_order_relaxed);
    m_zoomLevel.store(m_cfg.s.defaultZoomLevel, std::memory_order_relaxed);
    m_showZoomButton.store(m_cfg.showZoomButton, std::memory_order_relaxed);
    m_buttonBgSize.store(m_cfg.buttonBgSize, std::memory_order_relaxed);
    m_buttonIconSize.store(m_cfg.buttonIconSize, std::memory_order_relaxed);
    m_buttonIconOpacity.store(m_cfg.buttonIconOpacity, std::memory_order_relaxed);
    m_buttonBgOpacity.store(m_cfg.buttonBgOpacity, std::memory_order_relaxed);
    m_buttonX.store(m_cfg.buttonX, std::memory_order_relaxed);
    m_buttonY.store(m_cfg.buttonY, std::memory_order_relaxed);
    return true;
}

bool ZoomMod::enable(ll::mod::ModContext &context) {
    m_javaVm = context.javaVm();
    m_modEnabled.store(true, std::memory_order_relaxed);
    m_zoomOn.store(false, std::memory_order_relaxed);
    m_animFinished.store(true, std::memory_order_relaxed);
    m_showZoomButton.store(m_cfg.showZoomButton, std::memory_order_relaxed);
    m_buttonBgSize.store(m_cfg.buttonBgSize, std::memory_order_relaxed);
    m_buttonIconSize.store(m_cfg.buttonIconSize, std::memory_order_relaxed);
    m_buttonIconOpacity.store(m_cfg.buttonIconOpacity, std::memory_order_relaxed);
    m_buttonBgOpacity.store(m_cfg.buttonBgOpacity, std::memory_order_relaxed);
    m_buttonX.store(m_cfg.buttonX, std::memory_order_relaxed);
    m_buttonY.store(m_cfg.buttonY, std::memory_order_relaxed);

    m_zoomLevel.store(m_cfg.s.defaultZoomLevel, std::memory_order_relaxed);

    // Detect the launcher locale once for the mod menu strings.
    g_langZh = detectChineseLocale();

    // Custom button images (config/*.png): normal/pressed background and
    // normal/pressed icon. Each is optional and falls back to the built-in
    // drawing when the file is missing.
    loadCustomButtonIcons();

    // Register the process-wide touch callback once (there is no unregister
    // API; the callback itself checks the module state).
    static bool touchRegistered = false;
    if (!touchRegistered) {
        touchRegistered = true;
        pl::input::registerTouchCallback(&ZoomMod::touchCallbackFn);
    }

    registerModMenu();

    // Register the indicator font from the bundled TTF.
    if (!m_fontRegistered && m_self) {
        {
            const std::string fontPath =
                (m_self->getResourceDir() / "minecraft.ttf").string();
            const int fd = ::open(fontPath.c_str(), O_RDONLY);
            if (fd >= 0) {
                std::vector<unsigned char> data;
                char buf[4096];
                ssize_t n = 0;
                while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
                    data.insert(data.end(), buf, buf + n);
                }
                ::close(fd);
                if (!data.empty()) {
                    m_fontRegistered =
                        pl::modmenu::registerFont(kFontId, data);
                }
            }
        }
    }

    refreshOverlay();
    installHooksWhenGameLoaded();
    return true;
}

bool ZoomMod::disable(ll::mod::ModContext &context) {
    (void)context;
    m_modEnabled.store(false, std::memory_order_relaxed);
    setZoomActive(false);
    clearOverlay();
    unregisterModMenu();
    return true;
}

bool ZoomMod::unload(ll::mod::ModContext &context) {
    (void)context;
    m_modEnabled.store(false, std::memory_order_relaxed);
    setZoomActive(false);
    clearOverlay();
    unregisterModMenu();
    m_fovHook.reset();
    m_turnDeltaHook.reset();
    m_hideHandHook.reset();
    m_hideHudHook.reset();
    m_dlopenHook.reset();
    m_fromJavaHook.reset();
    m_hooksInstalled = false;
    m_hooksScheduled.store(false, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------

std::string ZoomMod::configFilePath() const {
    if (m_self) return (m_self->getConfigDir() / kConfigFileName).string();
    return std::string(kConfigFileName);
}

void ZoomMod::loadConfig() {
    {
        const std::string path = configFilePath();
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return;
        std::string contents;
        char buf[4096];
        ssize_t n = 0;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
            contents.append(buf, static_cast<size_t>(n));
        }
        ::close(fd);
        if (contents.empty()) return;
        ZoomConfig parsed = m_cfg;
        if (zoomConfigFromJson(parsed, contents)) m_cfg = parsed;
    }
}

void ZoomMod::saveConfig() {
    if (!m_self) return;
    {
        ::mkdir(m_self->getConfigDir().c_str(), 0755);
        const std::string contents = zoomConfigToJson(m_cfg);
        const std::string path = configFilePath();
        const int fd =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return;
        size_t written = 0;
        while (written < contents.size()) {
            const ssize_t w =
                ::write(fd, contents.data() + written,
                        contents.size() - written);
            if (w <= 0) break;
            written += static_cast<size_t>(w);
        }
        ::close(fd);
    }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

void ZoomMod::installHooksWhenGameLoaded() {
    // Already mapped? Resolve immediately.
    void *minecraft = dlopen("libminecraftpe.so", RTLD_NOW | RTLD_NOLOAD);
    if (minecraft) {
        dlclose(minecraft);
        onMinecraftLoaded();
        return;
    }
    // Otherwise wait for the game library via a dlopen detour.
    if (!m_dlopenHook.installed()) {
        m_dlopenHook = pl::memory::HookHandle(
            reinterpret_cast<void *>(&dlopen),
            reinterpret_cast<void *>(&ZoomMod::dlopenHookFn),
            reinterpret_cast<void **>(&g_dlopenOrig));
        if (m_self) {
            m_self->getLogger().info(
                "Waiting for libminecraftpe.so to load...");
        }
    }
}

void *ZoomMod::hookInstallThreadFn(void *arg) {
    static_cast<ZoomMod *>(arg)->resolveAndInstallHooks();
    return nullptr;
}

void ZoomMod::onMinecraftLoaded() {
    // The signature scan over libminecraftpe.so (~300 MB) is heavy and would
    // block the launcher's main thread (enable() runs there), which on some
    // devices trips an ANR. Defer it to a background thread and guard against
    // double-scheduling (this is also reached from the dlopen detour thread).
    bool expected = false;
    if (!m_hooksScheduled.compare_exchange_strong(expected, true)) {
        return; // already scheduled
    }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, &ZoomMod::hookInstallThreadFn,
                       this) == 0) {
        pthread_detach(thread);
        return;
    }
    // Extremely rare (thread creation failed): fall back to synchronous.
    m_hooksScheduled.store(false, std::memory_order_relaxed);
    resolveAndInstallHooks();
}

std::uintptr_t ZoomMod::resolveHideHudStub(std::uintptr_t hideHandAnchor) const {
    // The hide-HUD option id moved from 48 to 45 in the 26.50 build, so both
    // stub forms are tried. A candidate is accepted only when it sits exactly
    // kHideHudToHideHandDistance before the (independently verified)
    // hide-item-in-hand stub - the option getters form a fixed table, so the
    // real hide-HUD entry always satisfies this, while the stub of the *other*
    // option with the same shape does not.
    const char *candidates[] = {kGetHideHudPatternOption45,
                                kGetHideHudPatternOption48};
    for (const char *pattern : candidates) {
        const uintptr_t addr =
            pl::memory::resolveSignature(pattern, "libminecraftpe.so");
        if (!addr) continue;
        if (!hideHandAnchor) {
            // No anchor to validate against: take the newest-known form.
            if (m_self) {
                m_self->getLogger().warn(
                    "hide-HUD stub 0x{:x} accepted without anchor check "
                    "(hide-item-in-hand unresolved)", addr);
            }
            return addr;
        }
        if (hideHandAnchor > addr &&
            hideHandAnchor - addr == kHideHudToHideHandDistance) {
            return addr;
        }
        if (m_self) {
            m_self->getLogger().warn(
                "hide-HUD candidate 0x{:x} rejected: hides 0x{:x} before "
                "hide-item-in-hand 0x{:x}, expected 0x{:x}",
                addr, hideHandAnchor > addr ? hideHandAnchor - addr : 0,
                hideHandAnchor, kHideHudToHideHandDistance);
        }
    }
    return 0;
}

void ZoomMod::resolveAndInstallHooks() {
    const uintptr_t fov = pl::memory::resolveSignature(
        kGetFovPattern, "libminecraftpe.so");
    const uintptr_t turn = pl::memory::resolveSignature(
        kApplyTurnDeltaPattern, "libminecraftpe.so");
    const uintptr_t hand = pl::memory::resolveSignature(
        kGetHideItemInHandPattern, "libminecraftpe.so");
    const uintptr_t hud = resolveHideHudStub(hand);
    // Exported symbol of the game's touch bridge (dlsym by name).
    const uintptr_t fromJava = pl::memory::resolveSignature(
        "GameActivityMotionEvent_fromJava", "libminecraftpe.so");

    if (m_self) {
        m_self->getLogger().info(
            "GetFov=0x{:x} ApplyTurnDelta=0x{:x} HideItemInHand=0x{:x} "
            "HideHud=0x{:x} GameActivityMotionEvent_fromJava=0x{:x}",
            fov, turn, hand, hud, fromJava);
    }

    if (fov) {
        // Priority = Highest: the pl hook layer keeps one *chain* per target,
        // and the chain head's return value is what the game actually sees.
        // Other mods (e.g. BedrockTools' own zoom module) patch getFov too;
        // if they end up at the head, their value wins and our zoom has no
        // visible effect. Being the head makes our FOV authoritative while
        // still calling into the rest of the chain via g_getFovOrig.
        m_fovHook = pl::memory::HookHandle(
            reinterpret_cast<void *>(fov),
            reinterpret_cast<void *>(&ZoomMod::getFovHookFn),
            reinterpret_cast<void **>(&g_getFovOrig),
            pl::memory::HookPriority::Highest);
    } else if (m_self) {
        m_self->getLogger().error("Signature not found: GetFov");
    }
    if (turn) {
        m_turnDeltaHook = pl::memory::HookHandle(
            reinterpret_cast<void *>(turn),
            reinterpret_cast<void *>(&ZoomMod::applyTurnDeltaHookFn),
            reinterpret_cast<void **>(&g_applyTurnDeltaOrig));
    }
    if (hand) {
        m_hideHandHook = pl::memory::HookHandle(
            reinterpret_cast<void *>(hand),
            reinterpret_cast<void *>(&ZoomMod::getHideItemInHandHookFn),
            reinterpret_cast<void **>(&g_getHideItemInHandOrig));
    }
    if (!hud && m_self) {
        // Not fatal: "Hide HUD While Zoomed" also drives the game's own F1
        // toggle, which is version independent, so the feature still works.
        m_self->getLogger().warn(
            "hide-HUD option stub not resolved for this game build; "
            "falling back to the F1 HUD toggle only");
    }
    if (hud) {
        m_hideHudHook = pl::memory::HookHandle(
            reinterpret_cast<void *>(hud),
            reinterpret_cast<void *>(&ZoomMod::getHideHudHookFn),
            reinterpret_cast<void **>(&g_getHideHudOrig));
    }
    if (fromJava) {
        m_fromJavaHook = pl::memory::HookHandle(
            reinterpret_cast<void *>(fromJava),
            reinterpret_cast<void *>(&ZoomMod::fromJavaHookFn),
            reinterpret_cast<void **>(&g_fromJavaOrig));
    } else if (m_self) {
        m_self->getLogger().error(
            "Signature not found: GameActivityMotionEvent_fromJava");
    }

    m_hooksInstalled = m_fovHook.installed();
    if (m_self) {
        m_self->getLogger().info(
            "Game hooks installed: fov={} turn={} hand={} hud={} touchBridge={}",
            m_fovHook.installed(), m_turnDeltaHook.installed(),
            m_hideHandHook.installed(), m_hideHudHook.installed(),
            m_fromJavaHook.installed());
    }
}

// ---------------------------------------------------------------------------
// Game touch bridge: remove the zoom-button finger so the game's camera look
// never binds to it (this is the same seam Flarial Client hooks).
// ---------------------------------------------------------------------------

void ZoomMod::handleScrollFromGameEvent(void *outEvent) {
    if (!outEvent) return;
    auto *raw = static_cast<unsigned char *>(outEvent);
    // MotionEvent.ACTION_SCROLL == 8.
    const int32_t action = *reinterpret_cast<int32_t *>(raw + kGaActionOffset);
    if (action != 8) return;
    const uint32_t count = *reinterpret_cast<uint32_t *>(raw + kGaCountOffset);
    if (count == 0 || count > 8) return;

    // A scroll event carries one pointer (the mouse). Find the wheel axis:
    // the largest |value| among the non-position axes (skip X=0 / Y=1).
    unsigned char *slot = raw + kGaSlotBase;
    float scroll = 0.0f;
    int scrollAxis = -1;
    for (int a = 2; a < 48; ++a) {
        const float v =
            *reinterpret_cast<float *>(slot + kGaXOffset + a * 4);
        if (std::fabs(v) > std::fabs(scroll)) {
            scroll = v;
            scrollAxis = a;
        }
    }
    static int wheelLogCount = 0;
    if (wheelLogCount < 3 && m_self) {
        ++wheelLogCount;
        m_self->getLogger().info(
            "WHEEL action={} scroll[{}]={:.2f} level={:.1f}",
            action, scrollAxis, scroll,
            m_zoomLevel.load(std::memory_order_relaxed));
    }
    if (std::fabs(scroll) < 0.001f) return;

    // Not zoomed: let the game scroll the hotbar as usual.
    if (!m_zoomOn.load(std::memory_order_relaxed)) return;

    const ZoomSettings s = settings();
    if (s.useScroll) {
        // Multiplicative magnification adjust: every wheel notch changes the
        // level by the same relative amount, so the feel is consistent at any
        // zoom (the old FOV-delta adjust felt dead at 1x and wild at high zoom).
        const float base = m_baseFov.load(std::memory_order_relaxed);
        float level = m_zoomLevel.load(std::memory_order_relaxed);
        level = std::clamp(
            level * (1.0f + scroll * s.sensitivity * 0.02f), 1.0f, 80.0f);
        setZoomFov(base / level);
    }

    // While zoomed the scroll must never reach the game, otherwise it would
    // switch the hotbar. Blank the pointer count so the event is a no-op.
    *reinterpret_cast<uint32_t *>(raw + kGaCountOffset) = 0;
}

void ZoomMod::stripButtonFingerFromGameEvent(void *outEvent) {
    if (!outEvent) return;
    // Strip whenever the zoom button finger(s) are currently pressed.
    bool anyStrip = false;
    for (const bool b : m_stripPointerIds) {
        if (b) {
            anyStrip = true;
            break;
        }
    }
    if (!anyStrip) return;
    auto *raw = static_cast<unsigned char *>(outEvent);
    uint32_t count = *reinterpret_cast<uint32_t *>(raw + kGaCountOffset);
    if (count == 0 || count > 8) return;

    // Iterate all slots. For the zoom button finger(s): track their screen-Y
    // across calls to compute the slide delta (this hook sees ALL pointers, so
    // the button finger's slide is visible here even when it is not pointer
    // index 0) and adjust the zoom level; then remove the slot so the game's
    // camera-look uses a real screen finger. Matching is by pointer identity.
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char *slot = raw + kGaSlotBase + i * kGaSlotStride;
        const int32_t id = *reinterpret_cast<int32_t *>(slot);
        const bool isButtonFinger =
            id >= 0 && id < static_cast<int32_t>(m_stripPointerIds.size()) &&
            m_stripPointerIds[static_cast<size_t>(id)];
        if (isButtonFinger) {
            const size_t si = static_cast<size_t>(id);
            // Read Y (raw screen Y, or the axisValues[1] fallback).
            float y = *reinterpret_cast<float *>(slot + kGaRawYOffset);
            const float axY = *reinterpret_cast<float *>(slot + kGaYOffset);
            if (!(y > -1.0e6f && y < 1.0e6f)) y = axY;

            if (y > -1.0e6f && y < 1.0e6f) {
                // Drag detection (for the toggle tap-vs-drag distinction).
                if (!std::isnan(m_buttonDownY[si]) &&
                    std::fabs(y - m_buttonDownY[si]) > kDragSlop) {
                    m_buttonDraggedFlags[si] = true;
                }
                // Slide-to-adjust (only while zoomed).
                if (m_zoomOn.load(std::memory_order_relaxed) &&
                    settings().useScroll) {
                    if (!std::isnan(m_buttonLastY[si])) {
                        const float dy = y - m_buttonLastY[si];
                        // Swipe up (dy<0) = zoom in (smaller FOV), down = out.
                        adjustZoomByFov(dy *
                                        settings().sensitivity * 0.02f);
                    }
                    m_buttonLastY[si] = y;
                }
            }
        }
        if (isButtonFinger) {
            // Remove this slot: shift the remaining entries down.
            const uint32_t remaining = count - i - 1;
            if (remaining > 0) {
                std::memmove(slot, slot + kGaSlotStride,
                             remaining * kGaSlotStride);
            }
            *reinterpret_cast<uint32_t *>(raw + kGaCountOffset) = count - 1;
            --count;
            --i; // re-check the same index (next slot shifted into place)
        }
    }
}

// ---------------------------------------------------------------------------
// Core zoom logic
// ---------------------------------------------------------------------------

float ZoomMod::easeAlpha() {
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_lastFovTime).count();
    m_lastFovTime = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f; // clamp long gaps (pause menu, first call)
    // Flarial's curve: per-frame exponential ease-out, made
    // framerate-independent with the frame time.
    return 1.0f -
           std::exp(-dt * std::max(0.02f, settings().animSpeed) * 60.0f);
}

float ZoomMod::fovHook(float originalFov, int enableVariableFOV) {
    // The first-person hand pass is the only caller with
    // (enableVariableFOV & 1) == 0: the game then returns a fixed 70.0f.
    // It must never drive the animation base. (The GUI pass renders at a
    // fixed 60.0f with the flag set, so it is detected by value.)
    const bool isHand = (enableVariableFOV & 1) == 0;
    const bool isGui = std::fabs(originalFov - 60.0f) < 0.01f;
    const ZoomSettings s = settings();

    if (!isHand && !isGui) {
        m_baseFov.store(originalFov, std::memory_order_relaxed);
        m_zoomFov.store(clampZoomFov(m_zoomFov.load(std::memory_order_relaxed)),
                        std::memory_order_relaxed);
        updateBarsAnimation(s);
    }

    if (m_zoomOn.load(std::memory_order_relaxed)) {
        // Zooming: only the WORLD renders at the animated FOV; the hand and
        // GUI keep their own fixed FOVs so the first-person hand isn't
        // stretched (it stays at its normal 70.0f).
        m_animFinished.store(false, std::memory_order_relaxed);
        float current = m_currentFov.load(std::memory_order_relaxed);
        const float target = m_zoomFov.load(std::memory_order_relaxed);
        if (s.disableAnim) {
            current = target;
        } else {
            current += (target - current) * easeAlpha();
        }
        m_currentFov.store(current, std::memory_order_relaxed);
        if (isGui || isHand) return originalFov;
        updateIndicator();
        return current;
    }

    // Unzooming / idle.
    if (s.disableAnim) {
        m_currentFov.store(originalFov, std::memory_order_relaxed);
        m_animFinished.store(true, std::memory_order_relaxed);
        if (!isHand && !isGui) updateIndicator();
        return originalFov;
    }
    if (!m_animFinished.load(std::memory_order_relaxed) || s.alwaysAnimate) {
        if (isHand || isGui) {
            // The hand and GUI always stay at their own fixed FOVs (they are
            // never zoomed, so the first-person hand is never stretched).
            return originalFov;
        }
        float current = m_currentFov.load(std::memory_order_relaxed);
        const float alpha = easeAlpha();
        current += (originalFov - current) * alpha;
        if (std::fabs(current - originalFov) < 0.5f) {
            current = originalFov;
            m_animFinished.store(true, std::memory_order_relaxed);
        }
        m_currentFov.store(current, std::memory_order_relaxed);
        updateIndicator();
        return current;
    }

    m_currentFov.store(originalFov, std::memory_order_relaxed);
    return originalFov;
}

void ZoomMod::applyTurnDelta(float &dx, float &dy) {
    const ZoomSettings s = settings();
    // Low sensitivity applies to the raw delta BEFORE cinematic smoothing, so
    // both features compose (previously the smoothing path returned early and
    // skipped the sensitivity scaling entirely).
    if (s.lowSensitivity) {
        const float base = m_baseFov.load(std::memory_order_relaxed);
        const float current = m_currentFov.load(std::memory_order_relaxed);
        if (base > 0.1f) {
            // Sensitivity scales with the current zoom ratio (Flarial formula).
            const float zoomRatio = current / base; // < 1 while zoomed
            float multiplier =
                1.0f - (1.0f - zoomRatio) * s.lowSensitivityStrength;
            multiplier = std::max(0.01f, std::min(1.0f, multiplier));
            dx *= multiplier;
            dy *= multiplier;
        }
    }
    if (s.cinematicMode && s.smoothing) {
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - m_lastTurnTime).count();
        m_lastTurnTime = now;
        if (dt < 0.0f || dt > 0.1f) dt = 0.016f;
        const float alpha =
            1.0f - std::exp(-dt * std::max(0.1f, 10.01f - s.smoothness));
        m_smoothDx += (dx - m_smoothDx) * alpha;
        m_smoothDy += (dy - m_smoothDy) * alpha;
        dx = m_smoothDx;
        dy = m_smoothDy;
        return;
    }
    m_smoothDx = 0.0f;
    m_smoothDy = 0.0f;
}

long long ZoomMod::nowMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

void ZoomMod::toggleZoom() {
    setZoomActive(!m_zoomOn.load(std::memory_order_relaxed));
}

void ZoomMod::setZoomActive(bool active) {
    const bool was = m_zoomOn.load(std::memory_order_relaxed);
    if (active == was) return;
    if (active) {
        // The configured value is the zoom MAGNIFICATION (1x-80x): the FOV is
        // baseFov / level. "Save Zoom Level" remembers the last adjusted
        // level; with scroll disabled the level is always the configured one.
        const float base = m_baseFov.load(std::memory_order_relaxed);
        float level = m_zoomLevel.load(std::memory_order_relaxed);
        if (!settings().saveZoomLevel || !settings().useScroll) {
            level = settings().defaultZoomLevel;
        }
        level = std::clamp(level, 1.0f, 80.0f);
        m_zoomLevel.store(level, std::memory_order_relaxed);
        const float target = clampZoomFov(base / level);
        m_zoomFov.store(target, std::memory_order_relaxed);
        if (settings().disableAnim) {
            m_currentFov.store(target, std::memory_order_relaxed);
        }
    } else {
        m_animFinished.store(false, std::memory_order_relaxed);
        m_smoothDx = 0.0f;
        m_smoothDy = 0.0f;
    }
    m_zoomOn.store(active, std::memory_order_relaxed);
    m_lastIndicatorFactor.store(-1.0f, std::memory_order_relaxed);
    updateIndicator();
    updateCinematicBars();

    // Full HUD hide while zoomed: the game's F1 toggles the whole HUD, the
    // same trick the launcher's built-in "Toggle HUD" mod uses. F1 is a
    // toggle, so send it once on zoom-in and once on zoom-out (net zero).
    // Also hide the launcher's own overlay buttons while zoomed.
    if (settings().hideHud) {
        sendF1Key();
    }
}

void ZoomMod::adjustZoomByFov(float deltaFov) {
    if (!m_zoomOn.load(std::memory_order_relaxed)) return;
    setZoomFov(m_zoomFov.load(std::memory_order_relaxed) + deltaFov);
}

void ZoomMod::setZoomFov(float fov) {
    const float clamped = clampZoomFov(fov);
    m_zoomFov.store(clamped, std::memory_order_relaxed);
    const float base = m_baseFov.load(std::memory_order_relaxed);
    if (base > 0.1f && clamped > 0.1f) {
        m_zoomLevel.store(std::clamp(base / clamped, 1.0f, 80.0f),
                          std::memory_order_relaxed);
    }
    updateIndicator();
}

float ZoomMod::clampZoomFov(float fov) const {
    const float base = m_baseFov.load(std::memory_order_relaxed);
    const float lower = std::max(0.1f, base / 80.0f); // 80x max magnification
    const float upper = std::max(1.0f, base);          // 1x (no zoom out)
    return std::max(lower, std::min(upper, fov));
}

// ---------------------------------------------------------------------------
// Button geometry helpers
// ---------------------------------------------------------------------------

void ZoomMod::buttonCenter(float &cx, float &cy, float &half) {
    // The touchable area spans the larger of the background / icon layers.
    const int bgSize =
        std::max(36, m_buttonBgSize.load(std::memory_order_relaxed));
    const int iconSize =
        std::max(24, m_buttonIconSize.load(std::memory_order_relaxed));
    int sw = m_screenW.load(std::memory_order_relaxed);
    int sh = m_screenH.load(std::memory_order_relaxed);
    if (sw <= 0 || sh <= 0) {
        queryScreenSize(sw, sh);
        m_screenW.store(sw, std::memory_order_relaxed);
        m_screenH.store(sh, std::memory_order_relaxed);
    }
    const int bx = std::clamp(m_buttonX.load(std::memory_order_relaxed), 0, 1000);
    const int by = std::clamp(m_buttonY.load(std::memory_order_relaxed), 0, 1000);
    cx = static_cast<float>(sw) * bx / 1000.0f;
    cy = static_cast<float>(sh) * by / 1000.0f;
    half = static_cast<float>(std::max(bgSize, iconSize)) / 2.0f + 8.0f;
}

// ---------------------------------------------------------------------------
// Language detection (JNI: Locale.getDefault().getLanguage())
// ---------------------------------------------------------------------------

bool ZoomMod::detectChineseLocale() {
    if (!m_javaVm) return false;
    JNIEnv *env = nullptr;
    bool attached = false;
    const jint st = m_javaVm->GetEnv(reinterpret_cast<void **>(&env),
                                     JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (m_javaVm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return false;
        attached = true;
    } else if (st != JNI_OK) {
        return false;
    }

    bool isZh = false;
    do {
        jclass localeCls = env->FindClass("java/util/Locale");
        if (!localeCls) break;
        jmethodID getDefault = env->GetStaticMethodID(
            localeCls, "getDefault", "()Ljava/util/Locale;");
        if (!getDefault) break;
        jobject locale = env->CallStaticObjectMethod(localeCls, getDefault);
        if (!locale) break;
        jmethodID getLang = env->GetMethodID(
            localeCls, "getLanguage", "()Ljava/lang/String;");
        if (!getLang) break;
        jstring lang = static_cast<jstring>(env->CallObjectMethod(locale, getLang));
        if (!lang) break;
        const char *s = env->GetStringUTFChars(lang, nullptr);
        if (s) {
            isZh = (std::strncmp(s, "zh", 2) == 0);
            env->ReleaseStringUTFChars(lang, s);
        }
        env->DeleteLocalRef(lang);
        env->DeleteLocalRef(locale);
        env->DeleteLocalRef(localeCls);
    } while (false);

    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) m_javaVm->DetachCurrentThread();
    return isZh;
}

// ---------------------------------------------------------------------------
// Full HUD hide via the game's F1 (hide-GUI) key, the same mechanism the
// launcher's built-in "Toggle HUD" mod uses: dispatch a synthetic F1 down/up
// KeyEvent through the Activity.
// ---------------------------------------------------------------------------

jobject ZoomMod::getCurrentActivity() {
    if (!m_javaVm) return nullptr;
    JNIEnv *env = nullptr;
    bool attached = false;
    const jint st = m_javaVm->GetEnv(reinterpret_cast<void **>(&env),
                                     JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (m_javaVm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return nullptr;
        attached = true;
    } else if (st != JNI_OK) {
        return nullptr;
    }

    jobject activity = nullptr;
    do {
        jclass atCls = env->FindClass("android/app/ActivityThread");
        if (!atCls) break;
        jmethodID currentThread = env->GetStaticMethodID(
            atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
        if (!currentThread) break;
        jobject at = env->CallStaticObjectMethod(atCls, currentThread);
        if (!at) break;
        jfieldID mActivities =
            env->GetFieldID(atCls, "mActivities", "Landroid/util/ArrayMap;");
        if (!mActivities) break;
        jobject map = env->GetObjectField(at, mActivities);
        if (!map) break;
        jclass mapCls = env->GetObjectClass(map);
        jmethodID size = env->GetMethodID(mapCls, "size", "()I");
        jmethodID valueAt =
            env->GetMethodID(mapCls, "valueAt", "(I)Ljava/lang/Object;");
        if (!size || !valueAt) break;
        const jint n = env->CallIntMethod(map, size);
        for (jint i = n - 1; i >= 0; --i) {
            jobject record = env->CallObjectMethod(map, valueAt, i);
            if (!record) continue;
            jclass recCls = env->GetObjectClass(record);
            jfieldID actF =
                env->GetFieldID(recCls, "activity", "Landroid/app/Activity;");
            if (!actF) continue;
            jobject act = env->GetObjectField(record, actF);
            if (act) {
                activity = env->NewGlobalRef(act);
                break;
            }
        }
    } while (false);

    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) m_javaVm->DetachCurrentThread();
    return activity;
}

void ZoomMod::sendF1Key() {
    if (!m_javaVm) return;
    jobject activity = getCurrentActivity();
    if (!activity) return;

    JNIEnv *env = nullptr;
    bool attached = false;
    const jint st = m_javaVm->GetEnv(reinterpret_cast<void **>(&env),
                                     JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (m_javaVm->AttachCurrentThread(&env, nullptr) == JNI_OK)
            attached = true;
    }
    if (!env) {
        // Rare: no JNI env available; leaking the global ref is safer than
        // dereferencing a null env.
        return;
    }

    do {
        jclass keCls = env->FindClass("android/view/KeyEvent");
        if (!keCls) break;
        jmethodID ctor =
            env->GetMethodID(keCls, "<init>", "(JJIIIIIIII)V");
        if (!ctor) break;
        jclass scCls = env->FindClass("android/os/SystemClock");
        if (!scCls) break;
        jmethodID uptime =
            env->GetStaticMethodID(scCls, "uptimeMillis", "()J");
        if (!uptime) break;
        const jlong now = env->CallStaticLongMethod(scCls, uptime);
        // KeyEvent(downTime, eventTime, action, code, repeat, meta, deviceId,
        //          scancode, flags, source)  action 0=DOWN 1=UP, F1=131,
        //          source KEYBOARD=0x100.
        jobject down = env->NewObject(keCls, ctor, now, now, (jint)0,
                                      (jint)131, (jint)0, (jint)0, (jint)-1,
                                      (jint)0, (jint)0, (jint)0x100);
        jobject up = env->NewObject(keCls, ctor, now, now + 10, (jint)1,
                                    (jint)131, (jint)0, (jint)0, (jint)-1,
                                    (jint)0, (jint)0, (jint)0x100);
        if (!down || !up) break;
        jclass actCls = env->GetObjectClass(activity);
        jmethodID dispatch = env->GetMethodID(
            actCls, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
        if (!dispatch) break;
        env->CallBooleanMethod(activity, dispatch, down);
        env->CallBooleanMethod(activity, dispatch, up);
    } while (false);

    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteGlobalRef(activity);
    if (attached) m_javaVm->DetachCurrentThread();
}

// ---------------------------------------------------------------------------
// Custom button images (JNI: decode <config>/*.{png,jpg,jpeg,webp,svg} into an
// Android Bitmap, extract RGBA pixels and register them so the launcher's
// overlay can draw them).
//
// Four optional images, each keyed by the zoom (pressed) state:
//   button_bg.png           background, not pressed
//   button_bg_pressed.png   background, pressed (zoom active)
//   button_icon.png         icon, not pressed
//   button_icon_pressed.png icon, pressed (zoom active)
// Any of them may use .svg instead (e.g. button_icon.svg). A missing / broken
// file simply leaves its built-in fallback active.
// ---------------------------------------------------------------------------

namespace {

constexpr int kSvgRenderSize = 512; // SVG is rasterized at this size, then scaled

// Decode an image file into an Android Bitmap (local reference, owned by the
// caller). Raster files use BitmapFactory.decodeFile; .svg files are rendered
// by the launcher's bundled SVG renderer (lunasvg), then decoded from the
// produced PNG bytes.
jobject decodeButtonImage(JNIEnv *env, const std::string &path) {
    const size_t dot = path.rfind('.');
    const bool isSvg =
        dot != std::string::npos && (path.size() - dot) == 4 &&
        (path[dot + 1] == 's' || path[dot + 1] == 'S') &&
        (path[dot + 2] == 'v' || path[dot + 2] == 'V') &&
        (path[dot + 3] == 'g' || path[dot + 3] == 'G');

    if (!isSvg) {
        jclass bf = env->FindClass("android/graphics/BitmapFactory");
        if (!bf) return nullptr;
        jmethodID decodeFile = env->GetStaticMethodID(
            bf, "decodeFile",
            "(Ljava/lang/String;)Landroid/graphics/Bitmap;");
        if (!decodeFile) {
            env->DeleteLocalRef(bf);
            return nullptr;
        }
        jstring jpath = env->NewStringUTF(path.c_str());
        if (!jpath) {
            env->DeleteLocalRef(bf);
            return nullptr;
        }
        jobject bitmap = env->CallStaticObjectMethod(bf, decodeFile, jpath);
        env->DeleteLocalRef(jpath);
        env->DeleteLocalRef(bf);
        return bitmap;
    }

    // --- SVG path ---
    // Read the SVG text.
    std::string svg;
    {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return nullptr;
        char buf[4096];
        ssize_t n = 0;
        while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
            svg.append(buf, static_cast<size_t>(n));
        }
        ::close(fd);
    }
    if (svg.empty()) return nullptr;

    // The launcher's preloader bundles lunasvg and exports the JNI entry point
    // below. Call it directly via dlsym instead of going through the Java
    // bridge (MoreButtonsSvgBridge.renderSvg): this avoids app-classloader
    // issues on the mod's native thread and avoids a Java round-trip.
    using RenderSvgFn = jbyteArray (*)(JNIEnv *, jclass, jbyteArray, jint, jint);
    static RenderSvgFn renderSvgFn = nullptr;
    if (renderSvgFn == nullptr) {
        renderSvgFn = reinterpret_cast<RenderSvgFn>(::dlsym(
            RTLD_DEFAULT,
            "Java_org_levimc_launcher_core_mods_inbuilt_MoreButtonsSvgBridge_"
            "nativeRenderSvgToPng"));
        if (renderSvgFn == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, "BetterZoom",
                                "SVG renderer symbol not found");
            return nullptr;
        }
    }

    jbyteArray svgArr = env->NewByteArray(static_cast<jsize>(svg.size()));
    if (!svgArr) return nullptr;
    env->SetByteArrayRegion(svgArr, 0, static_cast<jsize>(svg.size()),
                            reinterpret_cast<const jbyte *>(svg.data()));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(svgArr);
        return nullptr;
    }
    jbyteArray pngArr = renderSvgFn(env, nullptr, svgArr,
                                    static_cast<jint>(kSvgRenderSize),
                                    static_cast<jint>(kSvgRenderSize));
    env->DeleteLocalRef(svgArr);
    if (!pngArr) {
        __android_log_print(ANDROID_LOG_ERROR, "BetterZoom",
                            "SVG render failed: %s", path.c_str());
        return nullptr;
    }

    const jsize len = env->GetArrayLength(pngArr);
    jbyte *bytes = env->GetByteArrayElements(pngArr, nullptr);
    if (!bytes) {
        env->DeleteLocalRef(pngArr);
        return nullptr;
    }

    jclass bf = env->FindClass("android/graphics/BitmapFactory");
    jobject bitmap = nullptr;
    if (bf) {
        jmethodID decodeByteArray = env->GetStaticMethodID(
            bf, "decodeByteArray", "([BII)Landroid/graphics/Bitmap;");
        if (decodeByteArray) {
            bitmap = env->CallStaticObjectMethod(bf, decodeByteArray, pngArr,
                                                 static_cast<jint>(0),
                                                 static_cast<jint>(len));
        }
        env->DeleteLocalRef(bf);
    }
    env->ReleaseByteArrayElements(pngArr, bytes, JNI_ABORT);
    env->DeleteLocalRef(pngArr);
    return bitmap;
}

} // namespace

bool ZoomMod::loadButtonImage(const char *fileName, const char *imageId) {
    if (!m_javaVm || !m_self) return false;

    JNIEnv *env = nullptr;
    bool attached = false;
    const jint st = m_javaVm->GetEnv(reinterpret_cast<void **>(&env),
                                     JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (m_javaVm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return false;
        attached = true;
    } else if (st != JNI_OK) {
        return false;
    }

    bool ok = false;
    do {
        const std::string path = (m_self->getConfigDir() / fileName).string();

        jobject bitmap = decodeButtonImage(env, path);
        if (!bitmap) break;  // no / broken image -> keep the built-in fallback

        jclass bmpCls = env->FindClass("android/graphics/Bitmap");
        if (!bmpCls) break;
        jmethodID getW = env->GetMethodID(bmpCls, "getWidth", "()I");
        jmethodID getH = env->GetMethodID(bmpCls, "getHeight", "()I");
        jmethodID getPixels =
            env->GetMethodID(bmpCls, "getPixels", "([IIIIIII)V");
        if (!getW || !getH || !getPixels) break;

        const int w = env->CallIntMethod(bitmap, getW);
        const int h = env->CallIntMethod(bitmap, getH);
        if (w <= 0 || h <= 0 || w * h > 1024 * 1024) break;

        jintArray arr = env->NewIntArray(w * h);
        if (!arr) break;
        env->CallVoidMethod(bitmap, getPixels, arr, 0, w, 0, 0, w, h);
        const jint *px = env->GetIntArrayElements(arr, nullptr);
        if (!px) break;

        std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4);
        for (int i = 0; i < w * h; ++i) {
            const uint32_t a = static_cast<uint32_t>(px[i]);
            rgba[static_cast<size_t>(i) * 4 + 0] = (a >> 16) & 0xFF; // R
            rgba[static_cast<size_t>(i) * 4 + 1] = (a >> 8) & 0xFF;  // G
            rgba[static_cast<size_t>(i) * 4 + 2] = a & 0xFF;         // B
            rgba[static_cast<size_t>(i) * 4 + 3] = (a >> 24) & 0xFF; // A
        }
        env->ReleaseIntArrayElements(arr, const_cast<jint *>(px), JNI_ABORT);
        env->DeleteLocalRef(arr);
        env->DeleteLocalRef(bitmap);
        env->DeleteLocalRef(bmpCls);

        ok = pl::modmenu::registerImage(imageId, rgba, w, h);
    } while (false);

    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) m_javaVm->DetachCurrentThread();
    return ok;
}

void ZoomMod::loadCustomButtonIcons() {
    // Prefer a user-provided vector icon (.svg) over the shipped PNG
    // placeholder: try `<base>.svg` first and fall back to `<base>.png`.
    auto load = [this](const char *base, const char *imageId) -> bool {
        const std::string svg = std::string(base) + ".svg";
        if (loadButtonImage(svg.c_str(), imageId)) return true;
        const std::string png = std::string(base) + ".png";
        return loadButtonImage(png.c_str(), imageId);
    };
    m_bgIconReady.store(load("button_bg", "betterzoom.buttonBg"),
                        std::memory_order_relaxed);
    m_bgIconPressedReady.store(
        load("button_bg_pressed", "betterzoom.buttonBgPressed"),
        std::memory_order_relaxed);
    m_customIconReady.store(load("button_icon", "betterzoom.buttonIcon"),
                            std::memory_order_relaxed);
    m_customIconPressedReady.store(
        load("button_icon_pressed", "betterzoom.buttonIconPressed"),
        std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Whether the zoom button should respond to touches. The launcher only shows
// its mod overlay (including the drawn button) when the HUD screen is open and
// no menu is showing (InbuiltOverlayManager: isHudScreenOpen && !isShowingMenu).
// Mirror that here so an invisible button is never clickable.
// ---------------------------------------------------------------------------

bool ZoomMod::shouldButtonInteract() {
    if (!m_javaVm) return true;
    JNIEnv *env = nullptr;
    bool attached = false;
    const jint st = m_javaVm->GetEnv(reinterpret_cast<void **>(&env),
                                     JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (m_javaVm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return true;
        attached = true;
    } else if (st != JNI_OK) {
        return true;
    }

    bool interact = true;
    do {
        static jclass preloaderCls = nullptr;
        static jmethodID isHudOpen = nullptr;
        static jmethodID isShowingMenu = nullptr;
        if (!preloaderCls) {
            jclass c = env->FindClass(
                "org/levimc/launcher/preloader/PreloaderInput");
            if (!c) break;
            preloaderCls = static_cast<jclass>(env->NewGlobalRef(c));
            env->DeleteLocalRef(c);
            if (!preloaderCls) break;
            isHudOpen = env->GetStaticMethodID(preloaderCls,
                                               "isHudScreenOpen", "()Z");
            isShowingMenu =
                env->GetStaticMethodID(preloaderCls, "isShowingMenu", "()Z");
        }
        if (!isHudOpen || !isShowingMenu) break;
        const bool hud = env->CallStaticBooleanMethod(preloaderCls, isHudOpen);
        const bool menu =
            env->CallStaticBooleanMethod(preloaderCls, isShowingMenu);
        interact = hud && !menu;
    } while (false);

    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) m_javaVm->DetachCurrentThread();
    return interact;
}

// ---------------------------------------------------------------------------
// Screen size (JNI: the app's Resources; fallback Resources.getSystem())
// ---------------------------------------------------------------------------

bool ZoomMod::queryScreenSize(int &w, int &h) {
    if (!m_javaVm) return false;
    JNIEnv *env = nullptr;
    bool attached = false;
    const jint st = m_javaVm->GetEnv(reinterpret_cast<void **>(&env),
                                     JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (m_javaVm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return false;
        attached = true;
    } else if (st != JNI_OK) {
        return false;
    }

    bool ok = false;
    jobject res = nullptr;
    jclass resCls = nullptr;
    jclass dmCls = nullptr;
    jobject dm = nullptr;
    do {
        resCls = env->FindClass("android/content/res/Resources");
        if (!resCls) break;

        jclass atCls = env->FindClass("android/app/ActivityThread");
        if (atCls) {
            jmethodID curApp = env->GetStaticMethodID(
                atCls, "currentApplication",
                "()Landroid/app/Application;");
            if (curApp) {
                jobject app = env->CallStaticObjectMethod(atCls, curApp);
                if (app) {
                    jclass appCls = env->GetObjectClass(app);
                    jmethodID getRes = env->GetMethodID(
                        appCls, "getResources",
                        "()Landroid/content/res/Resources;");
                    if (getRes) res = env->CallObjectMethod(app, getRes);
                    env->DeleteLocalRef(appCls);
                    env->DeleteLocalRef(app);
                }
            }
            env->DeleteLocalRef(atCls);
        }
        // A hidden-API denial or missing method leaves a pending exception;
        // clear it so the fallback below can run.
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!res) {
            jmethodID getSystem = env->GetStaticMethodID(
                resCls, "getSystem",
                "()Landroid/content/res/Resources;");
            if (!getSystem) break;
            res = env->CallStaticObjectMethod(resCls, getSystem);
        }
        if (!res) break;

        jmethodID getDm = env->GetMethodID(
            resCls, "getDisplayMetrics",
            "()Landroid/util/DisplayMetrics;");
        if (!getDm) break;
        dm = env->CallObjectMethod(res, getDm);
        if (!dm) break;
        dmCls = env->GetObjectClass(dm);
        jfieldID wF = env->GetFieldID(dmCls, "widthPixels", "I");
        jfieldID hF = env->GetFieldID(dmCls, "heightPixels", "I");
        if (!wF || !hF) break;
        w = env->GetIntField(dm, wF);
        h = env->GetIntField(dm, hF);
        ok = (w > 0 && h > 0);
    } while (false);

    if (dmCls) env->DeleteLocalRef(dmCls);
    if (dm) env->DeleteLocalRef(dm);
    if (res) env->DeleteLocalRef(res);
    if (resCls) env->DeleteLocalRef(resCls);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) m_javaVm->DetachCurrentThread();
    return ok;
}

// ---------------------------------------------------------------------------
// Touch input
// ---------------------------------------------------------------------------

bool ZoomMod::touchCallbackFn(const pl::input::TouchEvent &event) {
    return ZoomMod::instance().onTouch(event);
}

bool ZoomMod::onTouch(const pl::input::TouchEvent &event) {
    if (!m_modEnabled.load(std::memory_order_relaxed)) return false;
    if (event.pointerId < 0 ||
        event.pointerId >= static_cast<int>(m_pointers.size())) {
        return false;
    }

    Pointer &p = m_pointers[event.pointerId];

    switch (event.action) {
        case ACTION_DOWN:
        case ACTION_POINTER_DOWN: {
            // A slot can be left "active" if an UP/CANCEL was ever missed
            // (e.g. the activity paused mid-gesture). Treat a fresh DOWN as a
            // brand-new gesture instead of keeping the stale state.
            p = Pointer{};
            p.active = true;
            p.downTimeMs = nowMs();

            // Button hit-test (the pl:: callback sees each pointer's DOWN
            // with the correct id/position, so this is reliable). The button
            // is only interactive while the game HUD is on screen and no
            // menu (inventory/pause/...) is showing - otherwise it would be
            // invisible yet still clickable.
            if (m_showZoomButton.load(std::memory_order_relaxed) &&
                shouldButtonInteract()) {
                float cx, cy, half;
                buttonCenter(cx, cy, half);
                if (cx >= 0.0f && cy >= 0.0f &&
                    std::fabs(event.x - cx) <= half &&
                    std::fabs(event.y - cy) <= half) {
                    p.isButton = true;
                    const size_t si = static_cast<size_t>(event.pointerId);
                    m_stripPointerIds[si] = true;
                    m_buttonDownY[si] = event.y;
                    m_buttonLastY[si] = NAN;
                    m_buttonDraggedFlags[si] = false;
                    if (m_cfg.zoomMode == 0) {
                        // Hold mode: pressing the button zooms immediately.
                        setZoomActive(true);
                    }
                    // Never consume: the game-touch hook removes the button
                    // finger from the game's input stream and tracks its slide.
                }
            }
            return false;
        }
        case ACTION_UP:
        case ACTION_POINTER_UP:
        case ACTION_CANCEL: {
            if (!p.active) return false;
            p.active = false;

            if (p.isButton) {
                const size_t si = static_cast<size_t>(event.pointerId);
                if (m_cfg.zoomMode == 0) {
                    // Hold mode: releasing the LAST button finger closes the
                    // zoom (two fingers resting on the button: lifting one
                    // keeps the zoom active).
                    bool otherButtonFinger = false;
                    for (int i = 0;
                         i < static_cast<int>(m_pointers.size()); ++i) {
                        if (i == event.pointerId) continue;
                        const Pointer &bp = m_pointers[i];
                        if (bp.active && bp.isButton) {
                            otherButtonFinger = true;
                            break;
                        }
                    }
                    if (!otherButtonFinger) setZoomActive(false);
                } else {
                    // Toggle mode: a quick, non-dragged tap toggles. The drag
                    // flag is tracked in the game-touch hook (which sees the
                    // actual button-finger position).
                    const long long elapsed = nowMs() - p.downTimeMs;
                    if (!m_buttonDraggedFlags[si] && elapsed < kTapTimeoutMs) {
                        toggleZoom();
                    }
                }
                p.isButton = false;
                m_stripPointerIds[si] = false;
                m_buttonLastY[si] = NAN;
                m_buttonDownY[si] = NAN;
                m_buttonDraggedFlags[si] = false;
            }
            // Never consume: keep the game's pointer tracking consistent.
            return false;
        }
        case ACTION_MOVE: {
            if (!p.active) return false;
            // Never consume MOVE events. Camera look is handled by the game
            // (which sees only the screen fingers after the game-touch hook
            // strips the button finger); zoom adjustment is tracked in that
            // hook from the button finger's position. Keep the touch-maxima
            // estimate fresh.
            return false;
        }
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Mod menu registration + config handling
// ---------------------------------------------------------------------------

void ZoomMod::registerModMenu() {
    if (m_moduleRegistered) return;

    const auto &c = m_cfg;
    const ZoomSettings &s = c.s;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", s.defaultZoomLevel);
    const std::string defaultZoomLevel(buf);
    std::snprintf(buf, sizeof(buf), "%.2f", s.sensitivity);
    const std::string zoomSensitivity(buf);
    std::snprintf(buf, sizeof(buf), "%.2f", s.animSpeed);
    const std::string animationSpeed(buf);
    std::snprintf(buf, sizeof(buf), "%.2f", s.lowSensitivityStrength);
    const std::string lowSensitivityStrength(buf);
    std::snprintf(buf, sizeof(buf), "%.2f", c.buttonIconOpacity);
    const std::string buttonIconOpacity(buf);
    std::snprintf(buf, sizeof(buf), "%.2f", c.buttonBgOpacity);
    const std::string buttonBgOpacity(buf);
    std::snprintf(buf, sizeof(buf), "%.2f", s.smoothness);
    const std::string smoothness(buf);
    std::snprintf(buf, sizeof(buf), "%.2f", s.cinematicBarHeight);
    const std::string cinematicBarHeight(buf);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%08X",
                  static_cast<unsigned>(s.cinematicBarColor));
    const bool ok = pl::modmenu::ModuleBuilder(kModuleId, T("Zoom", "缩放"))
        .description(
            T("Flarial-style zoom: hold the on-screen button to zoom and slide "
               "up/down to adjust the level (or tap to toggle); smooth ease-out "
               "animation, low sensitivity, hidden hand, cinematic camera.",
               "Flarial 风格缩放：按住屏幕上的按钮缩放并上下滑动调整倍率（或点击切换）；"
               "平滑缓出动画、低灵敏度、隐藏手部、电影镜头。"))
        .defaultEnabled(true)
        .hideInHudEditor()
        .config("zoomMode", T("Zoom Mode", "缩放模式"), pl::modmenu::ConfigType::Radio,
                std::to_string(c.zoomMode), T("Hold,Toggle", "按住,点击切换"))
        .config("defaultZoomLevel", T("Default Zoom Level", "默认缩放等级"),
                pl::modmenu::ConfigType::SliderFloat,
                defaultZoomLevel, "1", "80")
        .config("useScroll", T("Use Scroll / Drag", "允许滑动/拖动"),
                pl::modmenu::ConfigType::Toggle, s.useScroll ? "true" : "false")
        .config("buttonBgSize", T("Button Background Size", "按钮背景大小"),
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(c.buttonBgSize), "48", "480")
        .config("buttonIconSize", T("Button Icon Size", "按钮图标大小"),
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(c.buttonIconSize), "24", "480")
        .config("buttonX", T("Button X (0=left, 1000=right)", "按钮 X（0=最左，1000=最右）"),
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(c.buttonX), "0", "1000")
        .config("buttonY", T("Button Y (0=top, 1000=bottom)", "按钮 Y（0=最上，1000=最下）"),
                pl::modmenu::ConfigType::SliderInt,
                std::to_string(c.buttonY), "0", "1000")
        .config("buttonIconOpacity", T("Button Icon Opacity", "按钮图标不透明度"),
                pl::modmenu::ConfigType::SliderFloat,
                buttonIconOpacity, "0.0", "1.0")
        .config("buttonBgOpacity", T("Button Background Opacity", "按钮背景不透明度"),
                pl::modmenu::ConfigType::SliderFloat,
                buttonBgOpacity, "0.0", "1.0")
        .config("keybind", T("Keybind", "按键绑定"), pl::modmenu::ConfigType::Keybind,
                c.keybind.empty() ? "0" : c.keybind)
        .config("cinematicCamera", T("Cinematic Camera", "电影镜头"),
                pl::modmenu::ConfigType::Toggle, s.cinematicMode ? "true" : "false")
        .config("smoothing", T("Smoothing", "转向平滑"), pl::modmenu::ConfigType::Toggle,
                s.smoothing ? "true" : "false", "", "", "cinematicCamera")
        .config("smoothness", T("Smoothness", "平滑强度"),
                pl::modmenu::ConfigType::SliderFloat,
                smoothness, "1.0", "10.0", "cinematicCamera")
        .config("cinematicBars", T("Cinematic Bars", "上下黑边"),
                pl::modmenu::ConfigType::Toggle, s.cinematicBars ? "true" : "false",
                "", "", "cinematicCamera")
        .config("cinematicBarHeight", T("Cinematic Bar Height", "黑边高度"),
                pl::modmenu::ConfigType::SliderFloat,
                cinematicBarHeight, "0.05", "0.50", "cinematicBars")
        .config("cinematicBarColor", T("Cinematic Bar Color", "黑边颜色"),
                pl::modmenu::ConfigType::Color, hex, "", "", "cinematicBars")
        .config("showZoomLevel", T("Zoom Indicator", "倍率指示器"),
                pl::modmenu::ConfigType::Toggle, s.showZoomLevel ? "true" : "false")
        .config("lowSensitivity", T("Low Sensitivity", "低灵敏度"),
                pl::modmenu::ConfigType::Toggle, s.lowSensitivity ? "true" : "false")
        .config("lowSensitivityStrength", T("Low Sensitivity Strength", "低灵敏度强度"),
                pl::modmenu::ConfigType::SliderFloat,
                lowSensitivityStrength, "0.0", "1.0", "lowSensitivity")
        .config("animationSpeed", T("Animation Speed", "动画速度"),
                pl::modmenu::ConfigType::SliderFloat,
                animationSpeed, "0.05", "2.0")
        .config("saveZoomLevel", T("Save Zoom Level", "保存缩放等级"),
                pl::modmenu::ConfigType::Toggle, s.saveZoomLevel ? "true" : "false")
        .config("zoomSensitivity", T("Zoom Sensitivity", "缩放灵敏度"),
                pl::modmenu::ConfigType::SliderFloat,
                zoomSensitivity, "1", "30")
        .config("alwaysAnimate", T("Always Animate", "始终平滑动画"),
                pl::modmenu::ConfigType::Toggle, s.alwaysAnimate ? "true" : "false")
        .config("hideHand", T("Hide Hand", "隐藏手部"),
                pl::modmenu::ConfigType::Toggle, s.hideHand ? "true" : "false")
        .config("hideHud", T("Hide HUD While Zoomed", "缩放时隐藏HUD"),
                pl::modmenu::ConfigType::Toggle, s.hideHud ? "true" : "false")
        .config("disableAnimation", T("Disable Animation", "禁用动画"),
                pl::modmenu::ConfigType::Toggle, s.disableAnim ? "true" : "false")
        .config("showZoomButton", T("Show Zoom Button", "显示缩放按钮"),
                pl::modmenu::ConfigType::Toggle, c.showZoomButton ? "true" : "false")
        .onToggle([](std::string_view moduleId, bool enabled) {
            (void)moduleId;
            ZoomMod::instance().onModuleToggle(enabled);
        })
        .onConfigChanged([](std::string_view moduleId, std::string_view key,
                            std::string_view value) {
            (void)moduleId;
            ZoomMod::instance().onConfigChanged(key, value);
        })
        .onKeybind([](std::string_view moduleId, std::string_view key,
                      bool isDown) {
            (void)moduleId;
            ZoomMod::instance().onKeybind(key, isDown);
        })
        .registerModule();

    if (m_self) {
        m_self->getLogger().info("Mod menu module registered: {}", ok);
    }
    m_moduleRegistered = ok;
}

void ZoomMod::unregisterModMenu() {
    if (m_moduleRegistered) {
        pl::modmenu::unregisterModule(kModuleId);
        m_moduleRegistered = false;
    }
}

void ZoomMod::onModuleToggle(bool enabled) {
    m_modEnabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        setZoomActive(false);
        clearOverlay();
    } else {
        refreshOverlay();
    }
}

void ZoomMod::onConfigChanged(std::string_view key, std::string_view value) {
    const std::string k(key);
    const std::string v(value);
    {
        if (k == "zoomMode") {
            m_cfg.zoomMode = std::atoi(v.c_str());
        } else if (k == "defaultZoomLevel") {
            m_cfg.s.defaultZoomLevel = std::strtof(v.c_str(), nullptr);
        } else if (k == "useScroll") {
            m_cfg.s.useScroll = (v == "true" || v == "1");
        } else if (k == "zoomSensitivity") {
            m_cfg.s.sensitivity = std::strtof(v.c_str(), nullptr);
        } else if (k == "disableAnimation") {
            m_cfg.s.disableAnim = (v == "true" || v == "1");
        } else if (k == "animationSpeed") {
            m_cfg.s.animSpeed = std::strtof(v.c_str(), nullptr);
        } else if (k == "saveZoomLevel") {
            m_cfg.s.saveZoomLevel = (v == "true" || v == "1");
        } else if (k == "alwaysAnimate") {
            m_cfg.s.alwaysAnimate = (v == "true" || v == "1");
        } else if (k == "hideHand") {
            m_cfg.s.hideHand = (v == "true" || v == "1");
        } else if (k == "lowSensitivity") {
            m_cfg.s.lowSensitivity = (v == "true" || v == "1");
        } else if (k == "lowSensitivityStrength") {
            m_cfg.s.lowSensitivityStrength = std::strtof(v.c_str(), nullptr);
        } else if (k == "cinematicCamera") {
            m_cfg.s.cinematicMode = (v == "true" || v == "1");
            updateCinematicBars();
        } else if (k == "smoothing") {
            m_cfg.s.smoothing = (v == "true" || v == "1");
        } else if (k == "smoothness") {
            m_cfg.s.smoothness = std::strtof(v.c_str(), nullptr);
        } else if (k == "cinematicBars") {
            m_cfg.s.cinematicBars = (v == "true" || v == "1");
            updateCinematicBars();
        } else if (k == "cinematicBarHeight") {
            m_cfg.s.cinematicBarHeight = std::strtof(v.c_str(), nullptr);
            updateCinematicBars();
        } else if (k == "cinematicBarColor") {
            std::string c = v;
            if (!c.empty() && c[0] == '#') {
                const unsigned long argb =
                    std::strtoul(c.substr(1).c_str(), nullptr, 16);
                // The launcher's color picker sends "#AARRGGBB".
                m_cfg.s.cinematicBarColor =
                    static_cast<uint32_t>(argb & 0xFFFFFFFFu);
            }
            updateCinematicBars();
        } else if (k == "showZoomLevel") {
            m_cfg.s.showZoomLevel = (v == "true" || v == "1");
            // Store the snapshot immediately so submitOverlay() sees the new
            // value, then force a redraw. (updateIndicator() would early-return
            // because `zoomed` didn't change, leaving the stale indicator.)
            m_settings.store(m_cfg.s, std::memory_order_relaxed);
            refreshOverlay();
        } else if (k == "hideHud") {
            m_cfg.s.hideHud = (v == "true" || v == "1");
        } else if (k == "keybind") {
            m_cfg.keybind = v;
        } else if (k == "showZoomButton") {
            m_cfg.showZoomButton = (v == "true" || v == "1");
            m_showZoomButton.store(m_cfg.showZoomButton,
                                   std::memory_order_relaxed);
            refreshOverlay();
        } else if (k == "buttonBgSize") {
            m_cfg.buttonBgSize = std::atoi(v.c_str());
            m_buttonBgSize.store(m_cfg.buttonBgSize, std::memory_order_relaxed);
            refreshOverlay();
        } else if (k == "buttonIconSize") {
            m_cfg.buttonIconSize = std::atoi(v.c_str());
            m_buttonIconSize.store(m_cfg.buttonIconSize, std::memory_order_relaxed);
            refreshOverlay();
        } else if (k == "buttonX") {
            m_cfg.buttonX = std::atoi(v.c_str());
            m_buttonX.store(m_cfg.buttonX, std::memory_order_relaxed);
            refreshOverlay();
        } else if (k == "buttonY") {
            m_cfg.buttonY = std::atoi(v.c_str());
            m_buttonY.store(m_cfg.buttonY, std::memory_order_relaxed);
            refreshOverlay();
        } else if (k == "buttonIconOpacity") {
            m_cfg.buttonIconOpacity = std::strtof(v.c_str(), nullptr);
            m_buttonIconOpacity.store(m_cfg.buttonIconOpacity,
                                      std::memory_order_relaxed);
            refreshOverlay();
        } else if (k == "buttonBgOpacity") {
            m_cfg.buttonBgOpacity = std::strtof(v.c_str(), nullptr);
            m_buttonBgOpacity.store(m_cfg.buttonBgOpacity,
                                    std::memory_order_relaxed);
            refreshOverlay();
        }
    }
    m_settings.store(m_cfg.s, std::memory_order_relaxed);
    m_showZoomButton.store(m_cfg.showZoomButton, std::memory_order_relaxed);
    m_buttonBgSize.store(m_cfg.buttonBgSize, std::memory_order_relaxed);
    m_buttonIconSize.store(m_cfg.buttonIconSize, std::memory_order_relaxed);
    m_buttonIconOpacity.store(m_cfg.buttonIconOpacity, std::memory_order_relaxed);
    m_buttonBgOpacity.store(m_cfg.buttonBgOpacity, std::memory_order_relaxed);
    m_buttonX.store(m_cfg.buttonX, std::memory_order_relaxed);
    m_buttonY.store(m_cfg.buttonY, std::memory_order_relaxed);
    saveConfig();
}

void ZoomMod::onKeybind(std::string_view key, bool isDown) {
    if (key != "keybind") return;
    if (m_cfg.zoomMode == 0) {
        // Hold mode: press = zoom on, release = zoom off.
        setZoomActive(isDown);
    } else {
        // Toggle mode: toggle on the press edge.
        if (isDown) toggleZoom();
    }
}

// ---------------------------------------------------------------------------
// Overlay: zoom button + level indicator + cinematic bars
// ---------------------------------------------------------------------------

void ZoomMod::updateIndicator() {
    if (!m_modEnabled.load(std::memory_order_relaxed)) {
        clearOverlay();
        return;
    }
    const bool zoomed = m_zoomOn.load(std::memory_order_relaxed);
    if (!zoomed || !settings().showZoomLevel) {
        if (m_zoomVisual.load(std::memory_order_relaxed) != zoomed)
            submitOverlay();
        return;
    }
    const float current = m_currentFov.load(std::memory_order_relaxed);
    const float base = m_baseFov.load(std::memory_order_relaxed);
    const float factor =
        std::round(base / std::max(1.0f, current) * 10.0f) / 10.0f;
    if (m_overlayVisible.load(std::memory_order_relaxed) &&
        m_zoomVisual.load(std::memory_order_relaxed) == zoomed &&
        std::fabs(factor -
                  m_lastIndicatorFactor.load(std::memory_order_relaxed)) <
            0.05f) {
        return; // no change -> keep the submitted commands
    }
    m_lastIndicatorFactor.store(factor, std::memory_order_relaxed);
    submitOverlay();
}

void ZoomMod::updateCinematicBars() {
    const bool want = m_zoomOn.load(std::memory_order_relaxed) &&
                      settings().cinematicMode && settings().cinematicBars;
    m_barsEnabled.store(want, std::memory_order_relaxed);
}

void ZoomMod::updateBarsAnimation(const ZoomSettings &s) {
    // Smoothly animate the letterbox bars in/out: the height factor
    // (m_barsAnim) eases toward the target (1 = fully shown) with an
    // exponential curve, and the overlay is resubmitted while it moves.
    const bool want = m_zoomOn.load(std::memory_order_relaxed) &&
                      s.cinematicMode && s.cinematicBars;
    m_barsEnabled.store(want, std::memory_order_relaxed);
    const float target = want ? 1.0f : 0.0f;
    float anim = m_barsAnim.load(std::memory_order_relaxed);
    if (s.disableAnim) {
        // Snap straight to the target. There is no interpolated redraw in this
        // branch, so the overlay must be resubmitted explicitly whenever the
        // target changes — otherwise a stale bar height survives the zoom-out
        // and the letterbox stays on screen forever.
        if (anim != target) {
            anim = target;
            m_barsAnim.store(anim, std::memory_order_relaxed);
            refreshOverlay();
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_barsLastTime).count();
    m_barsLastTime = now;
    if (dt < 0.0f || dt > 0.1f) dt = 0.016f;
    // ~0.25s ease-out, independent of the FOV animation speed.
    anim += (target - anim) * (1.0f - std::exp(-dt * 18.0f));
    if (std::fabs(target - anim) < 0.002f) anim = target;
    m_barsAnim.store(anim, std::memory_order_relaxed);
    if (std::fabs(anim - target) > 0.002f) {
        refreshOverlay(); // keep redrawing while the bars animate
    }
}

void ZoomMod::refreshOverlay() {
    m_lastIndicatorFactor.store(-1.0f, std::memory_order_relaxed);
    submitOverlay();
}

void ZoomMod::submitOverlay() {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    std::vector<pl::modmenu::DrawCommand> cmds;
    const bool zoomed = m_zoomOn.load(std::memory_order_relaxed);
    const ZoomSettings s = settings();

    // --- zoom button (fraction of the screen, so it can reach any corner) ---
    if (m_showZoomButton.load(std::memory_order_relaxed)) {
        const int bgSize =
            std::max(36, m_buttonBgSize.load(std::memory_order_relaxed));
        const int iconSize =
            std::max(24, m_buttonIconSize.load(std::memory_order_relaxed));
        int sw = m_screenW.load(std::memory_order_relaxed);
        int sh = m_screenH.load(std::memory_order_relaxed);
        if (sw <= 0 || sh <= 0) {
            queryScreenSize(sw, sh);
            m_screenW.store(sw, std::memory_order_relaxed);
            m_screenH.store(sh, std::memory_order_relaxed);
        }
        if (sw <= 0 || sh <= 0) {
            sw = 1920;
            sh = 1080;
        }
        // Shared anchor point: both layers stay centered on the same
        // buttonX / buttonY position even when their sizes differ.
        const float anchorX =
            static_cast<float>(sw) *
            std::clamp(m_buttonX.load(std::memory_order_relaxed), 0, 1000) /
            1000.0f;
        const float anchorY =
            static_cast<float>(sh) *
            std::clamp(m_buttonY.load(std::memory_order_relaxed), 0, 1000) /
            1000.0f;

        // Independent opacities: background alpha and icon alpha are now
        // fully separate (a common "button opacity" no longer exists).
        const float iconOpacity = std::clamp(
            m_buttonIconOpacity.load(std::memory_order_relaxed), 0.0f, 1.0f);
        const float bgOpacity = std::clamp(
            m_buttonBgOpacity.load(std::memory_order_relaxed), 0.0f, 1.0f);
        auto applyAlpha = [](uint32_t c, float mult) -> uint32_t {
            const uint32_t a = static_cast<uint32_t>(
                static_cast<float>((c >> 24) & 0xFFu) * mult);
            return (c & 0x00FFFFFFu) | (a << 24);
        };
        // Custom images are optional: pick the pressed/normal variant for the
        // current zoom state, otherwise fall back to the built-in drawing.
        const bool bgImageReady = zoomed
            ? m_bgIconPressedReady.load(std::memory_order_relaxed)
            : m_bgIconReady.load(std::memory_order_relaxed);
        const bool iconImageReady = zoomed
            ? m_customIconPressedReady.load(std::memory_order_relaxed)
            : m_customIconReady.load(std::memory_order_relaxed);

        const uint32_t bgColor =
            applyAlpha(zoomed ? 0xFF2FBF71u : 0xE62A2E33u, bgOpacity);
        const uint32_t fgColor =
            applyAlpha(zoomed ? 0xFF0B1A12u : 0xFFE8ECF1u, iconOpacity);
        // Bitmap drawing ignores the Paint RGB (only its alpha byte is used),
        // so tint white and fade via alpha to keep the image's own colors.
        const uint32_t bgImageColor = applyAlpha(0xFFFFFFFFu, bgOpacity);
        const uint32_t iconImageColor = applyAlpha(0xFFFFFFFFu, iconOpacity);

        // --- background layer (sized independently) ---
        const float bgX = anchorX - static_cast<float>(bgSize) * 0.5f;
        const float bgY = anchorY - static_cast<float>(bgSize) * 0.5f;
        if (bgImageReady) {
            pl::modmenu::DrawCommand img = {};
            img.type = pl::modmenu::DrawCommandType::Image;
            img.x = bgX;
            img.y = bgY;
            img.w = static_cast<float>(bgSize);
            img.h = static_cast<float>(bgSize);
            img.color = bgImageColor;
            img.imageId =
                zoomed ? "betterzoom.buttonBgPressed" : "betterzoom.buttonBg";
            cmds.push_back(img);
        } else {
            pl::modmenu::DrawCommand bg = {};
            bg.type = pl::modmenu::DrawCommandType::RectFilled;
            bg.x = bgX;
            bg.y = bgY;
            bg.w = static_cast<float>(bgSize);
            bg.h = static_cast<float>(bgSize);
            bg.x3 = static_cast<float>(bgSize) * 0.24f; // corner radius
            bg.color = bgColor;
            cmds.push_back(bg);
        }

        // --- icon layer (sized independently, centered on the same anchor) ---
        const float iconX = anchorX - static_cast<float>(iconSize) * 0.5f;
        const float iconY = anchorY - static_cast<float>(iconSize) * 0.5f;
        if (iconImageReady) {
            pl::modmenu::DrawCommand img = {};
            img.type = pl::modmenu::DrawCommandType::Image;
            img.x = iconX;
            img.y = iconY;
            img.w = static_cast<float>(iconSize);
            img.h = static_cast<float>(iconSize);
            img.color = iconImageColor;
            img.imageId = zoomed ? "betterzoom.buttonIconPressed"
                                 : "betterzoom.buttonIcon";
            cmds.push_back(img);
        } else {
            const float cx = anchorX;
            const float cy = anchorY;
            const float r = static_cast<float>(iconSize) * 0.22f;
            pl::modmenu::DrawCommand glass = {};
            glass.type = pl::modmenu::DrawCommandType::CircleFilled;
            glass.x = cx - static_cast<float>(iconSize) * 0.06f;
            glass.y = cy - static_cast<float>(iconSize) * 0.10f;
            glass.size = r;
            glass.color = fgColor;
            cmds.push_back(glass);

            pl::modmenu::DrawCommand handle = {};
            handle.type = pl::modmenu::DrawCommandType::Line;
            handle.x = cx - static_cast<float>(iconSize) * 0.06f + r * 0.62f;
            handle.y = cy - static_cast<float>(iconSize) * 0.10f + r * 0.62f;
            handle.w = cx + static_cast<float>(iconSize) * 0.30f - handle.x;
            handle.h = cy + static_cast<float>(iconSize) * 0.30f - handle.y;
            handle.size = std::max(3.0f, static_cast<float>(iconSize) * 0.09f);
            handle.color = fgColor;
            cmds.push_back(handle);
        }
    }

    // --- cinematic letterbox bars ---
    // The bars must cover the whole overlay width regardless of the queried
    // screen size (which is wrong on some devices and left the right edge
    // uncovered): draw from the left edge with a huge width (the canvas clips
    // it) and anchor the bottom bar to the bottom edge.
    // NOTE: draw whenever the animated height factor is still non-zero, not
    // only while `zoomed` is true. On zoom-out `m_zoomOn` flips to false
    // immediately, so gating on `zoomed` hid the bars instantly and killed the
    // ease-out (they could only ease in).
    if (s.cinematicMode && s.cinematicBars) {
        const float barAnim = m_barsAnim.load(std::memory_order_relaxed);
        if (barAnim > 0.001f) {
            const float barPx =
                std::max(8.0f, s.cinematicBarHeight * 1080.0f) * barAnim;
            constexpr float hugeWidth = 100000.0f;

            pl::modmenu::DrawCommand top = {};
            top.type = pl::modmenu::DrawCommandType::RectFilled;
            top.x = 0.0f;
            top.y = 0.0f;
            top.w = hugeWidth;
            top.h = barPx;
            top.color = s.cinematicBarColor;
            cmds.push_back(top);

            pl::modmenu::DrawCommand bottom = {};
            bottom.type = pl::modmenu::DrawCommandType::RectFilled;
            bottom.x = 0.0f;
            // y <= -9000 anchors to the bottom edge: getHeight() + (y + 10000).
            bottom.y = -10000.0f - barPx;
            bottom.w = hugeWidth;
            bottom.h = barPx;
            bottom.color = s.cinematicBarColor;
            cmds.push_back(bottom);
        }
    }

    // --- zoom level indicator (bottom center) ---
    // Drawn AFTER the letterbox bars so the black mask never covers it.
    if (zoomed && s.showZoomLevel) {
        constexpr float centerX = -20000.0f; // screen center anchor
        constexpr float bottomY = -10000.0f; // screen bottom edge anchor
        const float chipY = bottomY - 180.0f;

        char buf[24];
        std::snprintf(buf, sizeof(buf), "x%.1f",
                      m_baseFov.load(std::memory_order_relaxed) /
                          std::max(1.0f, m_currentFov.load(std::memory_order_relaxed)));

        pl::modmenu::DrawCommand bg = {};
        bg.type = pl::modmenu::DrawCommandType::RectFilled;
        bg.x = centerX - 52.0f;
        bg.y = chipY - 18.0f;
        bg.w = 104.0f;
        bg.h = 34.0f;
        bg.x3 = 8.0f;
        // Light gray-white semi-transparent scrim: clearly visible on a pure
        // black letterbox bar (a dark chip blended in too much). The text is
        // switched to dark so it stays readable on the light chip.
        bg.color = 0x80E6E6E6u;
        cmds.push_back(bg);

        pl::modmenu::DrawCommand txt = {};
        txt.type = pl::modmenu::DrawCommandType::Text;
        // Use the launcher's box-centering mode (w > 0 && h > 0): it draws the
        // text centered inside this box, so the baseline lands in the middle
        // of the chip instead of sitting on its bottom edge (the old w=-2
        // mode used the y as the text BASELINE, which pushed the text up).
        txt.x = centerX - 52.0f;
        txt.y = chipY - 18.0f;
        txt.w = 104.0f;
        txt.h = 34.0f;
        txt.size = 22.0f;
        txt.color = 0xFF151515u; // dark text for the light chip background
        txt.fontId = kFontId;
        txt.text = buf;
        cmds.push_back(txt);
    }

    pl::modmenu::submitDrawCommands(kModuleId, cmds);
    m_overlayVisible.store(!cmds.empty(), std::memory_order_relaxed);
    m_zoomVisual.store(zoomed, std::memory_order_relaxed);
}

void ZoomMod::clearOverlay() {
    std::lock_guard<std::mutex> lock(m_overlayMutex);
    if (m_overlayVisible.load(std::memory_order_relaxed)) {
        pl::modmenu::submitDrawCommands(kModuleId, {});
    }
    m_overlayVisible.store(false, std::memory_order_relaxed);
    m_barsEnabled.store(false, std::memory_order_relaxed);
    m_lastIndicatorFactor.store(-1.0f, std::memory_order_relaxed);
}

} // namespace betterzoom
