#pragma once

/**
 * @file ZoomMod.hpp
 * @brief Flarial-style zoom mod for LeviLauncher (Minecraft Bedrock 26.40.5).
 *
 * Behaviour:
 *  - Hold mode: press & hold the on-screen zoom button -> zoom; slide up/down
 *    on the button (or use a second finger) to adjust the level; release to
 *    close the zoom and restore the view.
 *  - Toggle mode: tap the button to keep zoom on, tap again to turn it off;
 *    drag on the button (or a second finger) adjusts the level while active.
 *  - Smooth exponential ease-out animation (Flarial curve), configurable.
 *  - Low look sensitivity, hidden first-person hand, cinematic camera
 *    (turn smoothing + letterbox bars), zoom-level indicator, keybind.
 *
 * Touch design (Flarial way):
 *  The zoom button is DRAWN by this mod inside the game view and hit-tested
 *  through pl::input. A consumed button-finger would still leak into the game
 *  through later multi-touch MotionEvents (Android events carry ALL active
 *  pointers), which makes the game bind camera-look to the stationary button
 *  finger. To prevent that, this mod also hooks the game's own touch bridge
 *  `GameActivityMotionEvent_fromJava` (exported by libminecraftpe.so) and
 *  REMOVES the button finger from the motion event struct before the game's
 *  input system processes it. The game therefore only ever sees the camera
 *  finger, so looking works regardless of touch order — exactly like Flarial
 *  Client, which hooks the same bridge (GameActivityMotionEventfromJavaCallback
 *  / RawTouchSnapshot).
 */

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <pl/Input.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include "ZoomConfig.hpp"

namespace betterzoom {

class ZoomMod {
public:
    static ZoomMod &instance();

    // ---- lifecycle (PL_REGISTER_MOD) ----
    bool load(ll::mod::ModContext &context);
    bool enable(ll::mod::ModContext &context);
    bool disable(ll::mod::ModContext &context);
    bool unload(ll::mod::ModContext &context);

    // ---- called from the static hook trampolines (render thread) ----
    bool modEnabled() const { return m_modEnabled.load(std::memory_order_relaxed); }
    bool zooming() const { return m_zoomOn.load(std::memory_order_relaxed); }
    bool wantsHideHand() const {
        return m_zoomOn.load(std::memory_order_relaxed) &&
               settings().hideHand;
    }
    bool wantsHideHud() const {
        return m_zoomOn.load(std::memory_order_relaxed) &&
               settings().hideHud;
    }
    float fovHook(float originalFov, int enableVariableFOV);
    void applyTurnDelta(float &dx, float &dy);

    // Signaled when libminecraftpe.so becomes available.
    void onMinecraftLoaded();

    // External setters (button / keybind).
    void setZoomActive(bool active);
    void toggleZoom();
    void setZoomFov(float fov);
    void adjustZoomByFov(float deltaFov);

private:
    ZoomMod() = default;
    ~ZoomMod() = default;
    ZoomMod(const ZoomMod &) = delete;
    ZoomMod &operator=(const ZoomMod &) = delete;

    // ---- hooks ----
    bool installGameHooks();
    void installHooksWhenGameLoaded();
    void resolveAndInstallHooks();
    // Chooses the correct hide-HUD option-getter stub for this game build by
    // validating candidates against the verified hide-item-in-hand address.
    // Returns 0 when no candidate validates (hook is then skipped).
    std::uintptr_t resolveHideHudStub(std::uintptr_t hideHandAnchor) const;
    void stripButtonFingerFromGameEvent(void *outEvent);
    void handleScrollFromGameEvent(void *outEvent);

    // ---- mod menu / overlay ----
    void registerModMenu();
    void unregisterModMenu();
    void submitOverlay();
    void clearOverlay();
    void refreshOverlay();
    void updateIndicator();
    void updateCinematicBars();
    void updateBarsAnimation(const ZoomSettings &s);
    void onModuleToggle(bool enabled);
    void onConfigChanged(std::string_view key, std::string_view value);
    void onKeybind(std::string_view key, bool isDown);

    // ---- input ----
    bool onTouch(const pl::input::TouchEvent &event);

    // ---- helpers ----
    const ZoomSettings &settings() const {
        return m_settings.load(std::memory_order_relaxed);
    }
    float clampZoomFov(float fov) const;
    float easeAlpha();
    // Button center in screen pixels from the fraction config + screen size.
    void buttonCenter(float &cx, float &cy, float &half);
    long long nowMs() const;
    bool queryScreenSize(int &w, int &h);
    bool shouldButtonInteract();
    bool detectChineseLocale();
    void loadCustomButtonIcons();
    bool loadButtonImage(const char *fileName, const char *imageId);
    jobject getCurrentActivity();
    void sendF1Key();
    void loadConfig();
    void saveConfig();
    std::string configFilePath() const;

    // ---- static trampolines ----
    static float getFovHookFn(void *self, float fov, int enableVariableFOV);
    static void applyTurnDeltaHookFn(void *self, void *deltaPtr);
    static bool getHideItemInHandHookFn(void *self);
    static bool getHideHudHookFn(void *self);
    static void *dlopenHookFn(const char *filename, int flags);
    static bool touchCallbackFn(const pl::input::TouchEvent &event);
    static void fromJavaHookFn(void *env, void *motionEvent, void *outEvent,
                               int32_t arg4, int32_t arg5);
    static void *hookInstallThreadFn(void *arg);

    ll::mod::NativeMod *m_self = nullptr;
    JavaVM *m_javaVm = nullptr;
    ZoomConfig m_cfg;

    // Settings snapshot consumed by the render thread.
    std::atomic<ZoomSettings> m_settings{};
    // Button geometry (written by the UI thread, read by the render thread
    // and by the game-touch hook on the same UI thread).
    std::atomic<bool> m_showZoomButton{true};
    // Custom button images decoded from <config>/*.png. Each pair is
    // (normal, pressed) where "pressed" == zoom active. A missing image
    // simply keeps the built-in fallback (colored rounded rect / magnifier),
    // so every one of the four is optional.
    std::atomic<bool> m_bgIconReady{false};
    std::atomic<bool> m_bgIconPressedReady{false};
    std::atomic<bool> m_customIconReady{false};
    std::atomic<bool> m_customIconPressedReady{false};
    std::atomic<int> m_buttonBgSize{200};
    std::atomic<int> m_buttonIconSize{150};
    std::atomic<float> m_buttonIconOpacity{0.5f};
    std::atomic<float> m_buttonBgOpacity{0.5f};
    std::atomic<int> m_buttonX{900};
    std::atomic<int> m_buttonY{850};
    // Base FOV tracked from the main render pass.
    std::atomic<float> m_baseFov{70.0f};
    std::atomic<float> m_currentFov{70.0f};
    std::atomic<float> m_zoomFov{14.0f};   // derived: baseFov / m_zoomLevel
    std::atomic<float> m_zoomLevel{5.0f};  // current magnification (1x-80x)
    std::atomic<bool> m_modEnabled{false};
    std::atomic<bool> m_zoomOn{false};
    std::atomic<bool> m_animFinished{true};

    std::chrono::steady_clock::time_point m_lastFovTime{};
    std::chrono::steady_clock::time_point m_lastTurnTime{};
    float m_smoothDx = 0.0f;
    float m_smoothDy = 0.0f;

    // Hook handles.
    pl::memory::HookHandle m_fovHook;
    pl::memory::HookHandle m_turnDeltaHook;
    pl::memory::HookHandle m_hideHandHook;
    pl::memory::HookHandle m_hideHudHook;
    pl::memory::HookHandle m_dlopenHook;
    pl::memory::HookHandle m_fromJavaHook;
    std::atomic<bool> m_hooksInstalled{false};
    // Guards the single background install run (onMinecraftLoaded may be called
    // from both the main thread and the dlopen detour thread).
    std::atomic<bool> m_hooksScheduled{false};

    // Touch tracking (UI thread only). The zoom button is drawn in the game
    // view and hit-tested here; the game-touch hook removes the button finger
    // from the game's own input stream so camera look always works.
    struct Pointer {
        bool active = false;      // finger currently down
        bool isButton = false;    // this finger is on the zoom button
        long long downTimeMs = 0; // DOWN time (toggle tap detection)
    };
    std::array<Pointer, 16> m_pointers{};
    // pointerIds that must be removed from the game's touch stream (the zoom
    // button finger(s)). Written by the touch callback (UI thread), read by
    // the game-touch hook (same UI thread).
    std::array<bool, 16> m_stripPointerIds{};
    // Last known screen-Y of each button finger, tracked in the game-touch
    // hook (which sees ALL pointers, unlike the single-pointer pl:: callback).
    std::array<float, 16> m_buttonLastY = []() {
        std::array<float, 16> a;
        a.fill(NAN);
        return a;
    }();
    std::array<float, 16> m_buttonDownY = []() {
        std::array<float, 16> a;
        a.fill(NAN);
        return a;
    }();
    std::array<bool, 16> m_buttonDraggedFlags{};

    // Screen size (queried via JNI; used for the letterbox bars). Written by
    // the UI thread and read by the render thread -> atomics.
    std::atomic<int> m_screenW{0};
    std::atomic<int> m_screenH{0};

    // Serializes overlay submissions from the render + UI threads.
    std::mutex m_overlayMutex;

    // UI state.
    bool m_moduleRegistered = false;
    bool m_fontRegistered = false;
    std::atomic<bool> m_overlayVisible{false};
    std::atomic<bool> m_barsEnabled{false};   // target state (bars should show)
    std::atomic<float> m_barsAnim{0.0f};      // animated height factor (0..1)
    std::chrono::steady_clock::time_point m_barsLastTime{}; // render-thread only
    std::atomic<bool> m_zoomVisual{false};
    std::atomic<float> m_lastIndicatorFactor{-1.0f};
};

} // namespace betterzoom
