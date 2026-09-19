**English** | [中文](README.md)

# BetterZoom v2 — Standalone Flarial-style Zoom Mod

A **standalone native zoom mod** for **LeviLauncher** (Minecraft Bedrock, **arm64-v8a**),
feature-matched to **Flarial Client**'s Zoom module (including its settings panel).
Built entirely on the official Levi native mod SDK (the `pl::` API from preloader-android)
and [BedrockTools](https://github.com/QYCottage/BedrockTools)-style byte-signature hooks.
It does **not** depend on Flarial itself, nor on LeviLauncher's built-in Zoom.

> **Compatibility**: verified on LeviLauncher 1.5.16 / 1.5.19 / 1.5.22 with
> Minecraft Bedrock 26.40.5 / 1.26.44.3 (all four byte signatures and the touch-bridge
> export resolve to exactly one location). Game hooks are installed on a background
> thread and never block the launcher's main thread.

> Earlier attempts (`FlarialZoom v1.3.0` / `ZoomPlus`) failed — the button was
> unregistered, Hold/Toggle was missing, or a missing `NEEDED libpreloader.so`
> prevented loading. This is a clean reimplementation.

## Improvements over LeviLauncher's built-in Zoom

| Built-in issue | BetterZoom v2 |
| --- | --- |
| Harsh, linear zoom animation | **Flarial's exponential ease-out curve** (framerate-independent, adjustable speed) |
| Cannot adjust the zoom level by sliding | **Slide up/down on the button** (or a second finger) to adjust live; release to restore |
| Tap-to-toggle only | **Hold and Toggle** modes, switchable live in the settings panel |
| No settings panel | Full settings panel (matching Flarial Zoom) |
| No level readout | Live magnification readout `x2.3` while zoomed |

## Installation

1. Open LeviLauncher → Mod management → remove any old `BetterZoom` / `ZoomPlus` import.
2. Import `BetterZoom.levipack` and enable it.
3. **Turn off LeviLauncher's built-in Zoom mod** (the Zoom toggle in the mod list),
   otherwise both will write to the FOV and you get double zoom.
4. Launch Minecraft from LeviLauncher (1.26.40.x). A magnifier button appears in-game.

## Usage

| Action | Effect |
| --- | --- |
| **Hold mode**: press and hold the magnifier button | Starts zooming (smooth ease-in) |
| While holding, **slide up/down on the button** (same finger) | Slide up to zoom in, down to zoom out, live |
| **Release the button** | Zoom closes and the view smoothly restores |
| **Toggle mode**: tap the button | Zoom turns on (stays on); tap again to turn off |
| While zoomed, **drag on the button** | Adjusts the zoom level (does not change the on/off state) |
| **Slide with any finger anywhere on screen** | Look around exactly as in vanilla — **any number of fingers, any order, any time** |
| Press the bound **Keybind** | Hold: press to zoom / Toggle: press to toggle |
| While zoomed | First-person hand hidden, turn sensitivity lowered (both optional) |

> **Adjusting zoom and looking around work simultaneously**: camera look is handled
> entirely by the game's native touch input (every screen finger is passed through
> untouched), while zoom adjustment only uses slides on the zoom button. The button
> finger's slide is tracked inside the **game touch-bridge hook** (which sees all
> pointers at once, so it works whether the button is the first or second finger down)
> and is removed from the game's touch stream — so "hold the button to adjust zoom" and
> "look around with another finger" never fight each other.

> **Why camera look works in any finger order**: this mod hooks the game's own touch
> bridge `GameActivityMotionEvent_fromJava` (an exported symbol of
> `libminecraftpe.so`) and removes the button finger from the touch event before the
> game's input system sees it (matched by `pointerId`, decrementing the pointer count
> and compacting the array). The game only ever sees the look finger — exactly what
> Flarial Client does. Pressing the button first and then looking, or the other way
> around, both work.

> **Touch note**: LeviLauncher's floating buttons live in a separate window and cannot
> provide slide data; and a self-drawn button without game-side touch handling leaks the
> button finger into the game as a "ghost pointer", binding camera look to the stationary
> finger. Hence this mod both draws its own button (same-finger slide to adjust) *and*
> hooks the game touch bridge to strip the button finger by `pointerId`. Button position
> is configured via **Button X / Button Y** (0–1000 proportional coordinates, anywhere on screen).

## Mod menu options (matching Flarial Zoom)

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| Zoom Mode | Radio | Hold | `Hold` to zoom while pressed / `Toggle` to tap-switch |
| Default Zoom Level | Slider | 10 | Zoom magnification (**1x–80x; the value *is* the magnification**, e.g. 5 = 5x) |
| Use Scroll / Drag | Toggle | on | Allow slide/wheel zoom adjustment; **when off the level is fixed to Default Zoom Level** |
| Zoom Sensitivity | Slider | 10 | FOV delta per slide step |
| Disable Animation | Toggle | off | Instant zoom, no animation |
| Animation Speed | Slider | 0.30 | Animation speed (higher = faster) |
| Save Zoom Level | Toggle | on | When off, every activation returns to the default level |
| Always Animate | Toggle | off | Smooth FOV changes even when not zoomed (sprinting, etc.) |
| Hide Hand | Toggle | on | Hide the first-person hand while zoomed |
| Low Sensitivity | Toggle | on | Lower turn sensitivity while zoomed |
| Low Sensitivity Strength | Slider | 0.75 | How much the sensitivity is reduced |
| Cinematic Camera | Toggle | off | Cinematic camera mode |
| Smoothing | Toggle | on | Turn smoothing (requires Cinematic Camera) |
| Smoothness | Slider | 7.0 | Smoothing strength |
| Cinematic Bars | Toggle | off | Top/bottom letterbox bars (requires Cinematic Camera) |
| Cinematic Bar Height | Slider | 0.20 | Letterbox height ratio |
| Cinematic Bar Color | Color | #000000 | Letterbox color |
| Zoom Indicator | Toggle | on | Magnification readout (bottom center, e.g. `x2.3`) |
| Hide HUD While Zoomed | Toggle | off | Hide the game HUD (hotbar, crosshair, …) while zoomed |
| Keybind | Keybind | none | Optional keyboard key |
| Show Zoom Button | Toggle | on | Show/hide the magnifier button |
| Button Background Size | Slider | 200 | Button background size (px, 48–480) |
| Button Icon Size | Slider | 150 | Button icon size (px, 24–480, independent of the background) |
| Button X | Slider | 850 | Button X (0 = leftmost, 1000 = rightmost; shared by background and icon) |
| Button Y | Slider | 600 | Button Y (0 = top, 1000 = bottom; shared by background and icon) |
| Button Icon Opacity | Slider | 0.5 | Icon opacity (0–1.0, independent; affects the icon only) |
| Button Background Opacity | Slider | 0.5 | Background opacity (0–1.0, independent; affects the background only) |

> Background size, icon size and both opacities are independently adjustable; the
> position (**Button X / Button Y**) is still a single shared anchor, so both layers
> always move together and keep their relative alignment.

> **Mouse wheel**: while zoomed the wheel adjusts the zoom level (up = zoom in);
> when not zoomed it scrolls the hotbar as usual.

> **UI language**: the mod menu follows LeviLauncher's system locale automatically
> (Chinese in a Chinese environment, English otherwise).

### Custom button appearance (background + icon, with pressed states)

Drop any image (**PNG/JPEG/WebP/SVG**, square and with a transparent background is
recommended) into `mods/BetterZoom/config/` using the file names below, then restart
the game. Delete a file to restore that layer's built-in look. **All four files are
optional** — you can replace just one. For SVG simply use the `.svg` extension (e.g.
`button_icon.svg`); it is rasterized at 512×512 and then scaled with the button.

| File name | Purpose | Fallback |
| --- | --- | --- |
| `button_bg.png` / `.svg` | Button background (not pressed) | Default dark rounded rect |
| `button_bg_pressed.png` / `.svg` | Button background (pressed / zooming) | Default green rounded rect |
| `button_icon.png` / `.svg` | Button icon (not pressed) | Default magnifier |
| `button_icon_pressed.png` / `.svg` | Button icon (pressed / zooming) | Default magnifier (dark) |

> "Pressed" means zoom is active: in Hold mode while the button is held, in Toggle mode
> while zoom is switched on. The package ships a set of **LeviLauncher-style placeholder
> images** (green circular button background using the `bg_overlay_button` palette, plus
> a magnifier icon using the `ic_zoom` palette) — just replace them with your own.

Changes are written back to the mod's `config/config.json` and restored on the next launch.

## Implementation notes

- Lifecycle: `PL_REGISTER_MOD` + `load/enable/disable/unload` (official Levi template).
- Menu: `pl::modmenu::ModuleBuilder` (no hard-coded `modId`; the preloader assigns the
  owning mod).
- Input: `pl::input::registerTouchCallback` for the game view's raw touch stream
  (returning `true` consumes the event so neither the game nor floating buttons see it;
  used for button taps and the zoom-adjust gesture).
- Screen size: JNI reads `Resources.getSystem().getDisplayMetrics()` (public API only)
  for button hit-testing; falls back to estimating from touch coordinates.
- Hooks (signatures verified one-by-one on `libminecraftpe.so` 26.40.5 and 1.26.44.3,
  each matching exactly one location):
  - `LevelRendererPlayer::getFov` — the zoom itself (exponential ease-out animation)
  - `LocalPlayer::applyTurnDelta` — low sensitivity / cinematic smoothing
  - `BaseOptionRegistry::getHideItemInHand` — hide the held item
  - `BaseOptionRegistry::getHideHud` — hide the hotbar
  - `GameActivityMotionEvent_fromJava` — the game touch bridge (exported symbol,
    resolved by name); strips the zoom-button finger so camera look works in any
    touch order (the same seam Flarial Client uses)
- Only the **world** renders at the zoomed FOV; the first-person hand and the GUI keep
  their own fixed FOVs, so the hand is never stretched.
- If `libminecraftpe.so` is not loaded yet, a `dlopen` hook is installed first and the
  signatures are resolved once the game library loads. Hook installation runs on a
  background thread (the signature scan over a ~300 MB library must not block the
  launcher's main thread).
- Overlay: `submitDrawCommands` draws the button / zoom indicator / letterbox bars
  (negative-coordinate anchors: `<= -9000` right/bottom edge, `<= -19000` screen center).
- Dependency: `NEEDED libpreloader.so` (the link-time stub is `lib/libpreloader-1.5.16.so`;
  at runtime it resolves to the launcher's own preloader). `--exclude-libs` keeps
  `PLGetModRegistration` as the only exported symbol, matching working reference mods.

## Building

```bash
# Requires clang-17/lld-17 and an Android NDK r28c sysroot (arm64)
NDK_SYSROOT=/path/to/ndk/sysroot ./build.sh        # produces build/libbetterzoom.so
python3 scripts/package_levipack.py --root .       # produces BetterZoom.levipack
```

### Dependencies

- `include/pl/`, `third_party/`: SDK headers plus fmt / nlohmann-json, vendored in the repo.
- `lib/libpreloader-1.5.16.so`: the preloader stub required at **link time** to resolve
  the `pl::` symbols (at runtime they resolve to the launcher's own `libpreloader.so`).
  It comes from the open-source [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid)
  project; if missing, obtain the same-named file from its releases/build and place it in `lib/`.
- The build script finishes with `llvm-strip --strip-all` to shrink the `.so`
  (about 7 MB → 0.6 MB).

## Troubleshooting

- **No "Zoom" module in the mod menu**: check `adb logcat -s Preloader BetterZoom` — you
  should see `Mod menu module registered: true`. If `false`, look for the `Rejected …`
  reason.
- **Hooks not active (no zoom)**: `adb logcat -s BetterZoom` should show
  `GetFov=0x... ApplyTurnDelta=0x... HideItemInHand=0x...` and
  `Game hooks installed: fov=.. turn=.. hand=.. touchBridge=..`. If `fov=false`, the
  signatures did not match (verified on 26.40.5 / 1.26.44.3); please provide that
  version's `libminecraftpe.so` so it can be adapted.
- **Diagnosing simultaneous look + zoom adjustment** (key log lines):
  - `TOUCH action=.. pointerId=.. x=.. y=..` — touch events received by the mod
    (note whether `pointerId` is always 0 for `action=2`, i.e. MOVE)
  - `LATCHED adjust finger pointerId=..` — a fast vertical slide recognized as zoom adjustment
  - `BRIDGE arg4=.. arg5=.. count=..` — whether the game touch bridge was reached
  - `Stripping pointer id=.. (count .. -> ..)` — whether the button finger was removed
    from the game's touch stream
- **Load failure**: for `Failed to load mod ...` / `dlopen failed: cannot locate symbol`,
  make sure the levipack is a freshly packaged one (old v1.3.0 / ZoomPlus must be removed first).
- **Double zoom**: make sure the launcher's built-in Zoom is turned off.

## Known trade-offs (vs Flarial)

- Flarial's "Hide Modules" (hiding other HUD elements) cannot be implemented
  across mods and is omitted.
- Opening a GUI (inventory, etc.) keeps the zoom state but the GUI itself is not zoomed
  (same as OptiFine).
- Letterbox height is converted to pixels based on 1080p; the held-item pass is
  identified via the `enableVariableFOV` bit flag of `getFov` (fixed 70° pass) and the
  GUI pass by its 60° value: **only the world zooms**, while the held item and GUI keep
  their own fixed FOV (the hand is never stretched).

## License

This project is licensed under the **MIT License** — see [LICENSE](LICENSE).

Licenses and origins of bundled third-party components (the preloader stub, the `pl::`
SDK headers, fmt, nlohmann-json, the Minecraft font, …) are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
