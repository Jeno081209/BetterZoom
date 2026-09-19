#include "ZoomConfig.hpp"

#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace betterzoom {

namespace {
bool getBool(const nlohmann::json &j, const char *key, bool fallback) {
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    if (j.contains(key) && j[key].is_number()) return j[key].get<int>() != 0;
    return fallback;
}
float getFloat(const nlohmann::json &j, const char *key, float fallback) {
    if (j.contains(key) && j[key].is_number()) return j[key].get<float>();
    return fallback;
}
int getInt(const nlohmann::json &j, const char *key, int fallback) {
    if (j.contains(key) && j[key].is_number()) return j[key].get<int>();
    return fallback;
}
std::string getString(const nlohmann::json &j, const char *key,
                      const char *fallback) {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return fallback;
}
void putBool(nlohmann::json &j, const char *key, bool value) {
    j[key] = value;
}
void putFloat(nlohmann::json &j, const char *key, float value) {
    j[key] = value;
}
void putInt(nlohmann::json &j, const char *key, int value) { j[key] = value; }
void putString(nlohmann::json &j, const char *key, const std::string &value) {
    j[key] = value;
}
} // namespace

bool zoomConfigFromJson(ZoomConfig &cfg, const std::string &jsonText) {
    {
        const nlohmann::json j =
            nlohmann::json::parse(jsonText, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;

        cfg.zoomMode = getInt(j, "zoomMode", cfg.zoomMode);
        cfg.showZoomButton = getBool(j, "showZoomButton", cfg.showZoomButton);
        // Backward compat: the legacy single "buttonSize" seeds both layers.
        if (j.contains("buttonSize") && j["buttonSize"].is_number()) {
            const int legacy = j["buttonSize"].get<int>();
            cfg.buttonBgSize = legacy;
            cfg.buttonIconSize = legacy;
        }
        cfg.buttonBgSize = getInt(j, "buttonBgSize", cfg.buttonBgSize);
        cfg.buttonIconSize = getInt(j, "buttonIconSize", cfg.buttonIconSize);
        cfg.buttonX = getInt(j, "buttonX", cfg.buttonX);
        cfg.buttonY = getInt(j, "buttonY", cfg.buttonY);
        // Backward compat: the legacy "buttonOpacity" was the icon alpha.
        if (j.contains("buttonOpacity") && j["buttonOpacity"].is_number()) {
            cfg.buttonIconOpacity = j["buttonOpacity"].get<float>();
        }
        cfg.buttonIconOpacity =
            getFloat(j, "buttonIconOpacity", cfg.buttonIconOpacity);
        cfg.buttonBgOpacity = getFloat(j, "buttonBgOpacity", cfg.buttonBgOpacity);
        cfg.keybind = getString(j, "keybind", "");

        ZoomSettings &s = cfg.s;
        s.defaultZoomLevel = getFloat(j, "defaultZoomLevel", s.defaultZoomLevel);
        // Backward compat: the old key held a FOV in degrees.
        if (j.contains("defaultZoomFov")) {
            s.defaultZoomLevel = 5.0f; // ignore legacy value
        }
        s.useScroll = getBool(j, "useScroll", s.useScroll);
        s.sensitivity = getFloat(j, "zoomSensitivity", s.sensitivity);
        s.disableAnim = getBool(j, "disableAnimation", s.disableAnim);
        s.animSpeed = getFloat(j, "animationSpeed", s.animSpeed);
        s.saveZoomLevel = getBool(j, "saveZoomLevel", s.saveZoomLevel);
        s.alwaysAnimate = getBool(j, "alwaysAnimate", s.alwaysAnimate);
        s.hideHand = getBool(j, "hideHand", s.hideHand);
        s.lowSensitivity = getBool(j, "lowSensitivity", s.lowSensitivity);
        s.lowSensitivityStrength =
            getFloat(j, "lowSensitivityStrength", s.lowSensitivityStrength);
        s.cinematicMode = getBool(j, "cinematicCamera", s.cinematicMode);
        s.smoothing = getBool(j, "smoothing", s.smoothing);
        s.smoothness = getFloat(j, "smoothness", s.smoothness);
        s.cinematicBars = getBool(j, "cinematicBars", s.cinematicBars);
        s.cinematicBarHeight =
            getFloat(j, "cinematicBarHeight", s.cinematicBarHeight);
        if (j.contains("cinematicBarColor") && j["cinematicBarColor"].is_string()) {
            const std::string c = j["cinematicBarColor"].get<std::string>();
            if (!c.empty() && c[0] == '#') {
                char *end = nullptr;
                const unsigned long v =
                    std::strtoul(c.c_str() + 1, &end, 16);
                if (end != c.c_str() + 1) {
                    s.cinematicBarColor =
                        0xFF000000u | static_cast<uint32_t>(v & 0xFFFFFFu);
                }
            }
        }
        s.showZoomLevel = getBool(j, "showZoomLevel", s.showZoomLevel);
        s.hideHud = getBool(j, "hideHud", s.hideHud);
        return true;
    }
}

std::string zoomConfigToJson(const ZoomConfig &cfg) {
    nlohmann::json j;
    const ZoomSettings &s = cfg.s;

    putInt(j, "zoomMode", cfg.zoomMode);
    putBool(j, "showZoomButton", cfg.showZoomButton);
    putInt(j, "buttonBgSize", cfg.buttonBgSize);
    putInt(j, "buttonIconSize", cfg.buttonIconSize);
    putInt(j, "buttonX", cfg.buttonX);
    putInt(j, "buttonY", cfg.buttonY);
    putFloat(j, "buttonIconOpacity", cfg.buttonIconOpacity);
    putFloat(j, "buttonBgOpacity", cfg.buttonBgOpacity);
    putString(j, "keybind", cfg.keybind);

    putFloat(j, "defaultZoomLevel", s.defaultZoomLevel);
    putBool(j, "useScroll", s.useScroll);
    putFloat(j, "zoomSensitivity", s.sensitivity);
    putBool(j, "disableAnimation", s.disableAnim);
    putFloat(j, "animationSpeed", s.animSpeed);
    putBool(j, "saveZoomLevel", s.saveZoomLevel);
    putBool(j, "alwaysAnimate", s.alwaysAnimate);
    putBool(j, "hideHand", s.hideHand);
    putBool(j, "lowSensitivity", s.lowSensitivity);
    putFloat(j, "lowSensitivityStrength", s.lowSensitivityStrength);
    putBool(j, "cinematicCamera", s.cinematicMode);
    putBool(j, "smoothing", s.smoothing);
    putFloat(j, "smoothness", s.smoothness);
    putBool(j, "cinematicBars", s.cinematicBars);
    putFloat(j, "cinematicBarHeight", s.cinematicBarHeight);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%06X",
                  static_cast<unsigned>(s.cinematicBarColor & 0xFFFFFFu));
    putString(j, "cinematicBarColor", hex);
    putBool(j, "showZoomLevel", s.showZoomLevel);
    putBool(j, "hideHud", s.hideHud);

    return j.dump(2);
}

} // namespace betterzoom
