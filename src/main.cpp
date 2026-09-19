/**
 * @file main.cpp
 * @brief FlarialZoom - Flarial-style zoom mod for LeviLauncher.
 *
 * Lifecycle entry point following the LeviLauncher native mod developer
 * guide (PL_REGISTER_MOD). All game interaction is handled by ZoomMod.
 */

#include <pl/Mod.hpp>

#include "ZoomMod.hpp"

namespace {

class FlarialZoomMod {
public:
    static FlarialZoomMod &instance() {
        static FlarialZoomMod mod;
        return mod;
    }

    bool load(ll::mod::ModContext &context) {
        return betterzoom::ZoomMod::instance().load(context);
    }

    bool enable(ll::mod::ModContext &context) {
        return betterzoom::ZoomMod::instance().enable(context);
    }

    bool disable(ll::mod::ModContext &context) {
        return betterzoom::ZoomMod::instance().disable(context);
    }

    bool unload(ll::mod::ModContext &context) {
        return betterzoom::ZoomMod::instance().unload(context);
    }
};

} // namespace

PL_REGISTER_MOD(FlarialZoomMod, FlarialZoomMod::instance())
