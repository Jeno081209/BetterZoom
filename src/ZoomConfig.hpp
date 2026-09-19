#pragma once

/**
 * @file ZoomConfig.hpp
 * @brief Zoom settings + JSON persistence.
 */

#include <cstdint>
#include <string>

namespace betterzoom {

// Trivially-copyable snapshot consumed by the render-thread hooks.
// (No std::string inside so it can live in a std::atomic.)
struct ZoomSettings {
    bool useScroll = true;
    float sensitivity = 10.0f;      // FOV delta per 50 px of slide
    bool disableAnim = false;
    float animSpeed = 0.30f;        // exponential ease-out speed
    bool saveZoomLevel = true;
    bool alwaysAnimate = false;
    bool hideHand = true;
    bool lowSensitivity = true;
    float lowSensitivityStrength = 0.75f;
    bool cinematicMode = true;
    bool smoothing = true;
    float smoothness = 7.0f;
    bool cinematicBars = false;
    float cinematicBarHeight = 0.20f;
    uint32_t cinematicBarColor = 0xFF000000u; // AARRGGBB
    bool showZoomLevel = true;
    bool hideHud = false;           // hide the game HUD (hotbar, crosshair) while zoomed
    float defaultZoomLevel = 5.0f;  // zoom magnification when activating (1x-80x)
};

// Full config (strings + non-atomic members live on the UI thread).
struct ZoomConfig {
    // mode: 0 = Hold (按住), 1 = Toggle (点击切换)
    int zoomMode = 0;
    ZoomSettings s;
    bool showZoomButton = true;
    int buttonBgSize = 200;        // px (background layer)
    int buttonIconSize = 150;      // px (icon layer)
    int buttonX = 850;              // 0..1000 = 0%..100% of screen width
    int buttonY = 600;              // 0..1000 = 0%..100% of screen height
    float buttonIconOpacity = 0.50f; // icon alpha (0..1.0, independent)
    float buttonBgOpacity = 0.50f;   // background alpha (0..1.0, independent)
    std::string keybind;            // int keycode as string ("" = none)
};

// JSON <-> config (nlohmann). Returns false when parsing fails.
bool zoomConfigFromJson(ZoomConfig &cfg, const std::string &jsonText);
std::string zoomConfigToJson(const ZoomConfig &cfg);

} // namespace betterzoom
