# Third-Party Notices

BetterZoom itself is licensed under the **MIT License** (see `LICENSE`).
It bundles or links against the following third-party components, each governed by
its own license. Their terms apply to those components only.

---

## 1. `lib/libpreloader-1.5.16.so` — preloader stub (link-time only)

- **Source**: [LiteLDev/LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid)
- **License**: Apache License 2.0

Used only at **link time** so the mod can resolve the `pl::` SDK symbols; at runtime
those symbols resolve to the launcher's own `libpreloader.so`.
If you redistribute this repository or a build of it, keep this notice and the
Apache-2.0 license terms for this file.

> Apache-2.0 requires that the license text and any NOTICE file be preserved.
> See the upstream repository for the full text and NOTICE.

## 2. `include/pl/` — Levi native mod SDK headers

- **Source**: [LiteLDev/LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid) (preloader-android)
- **License**: Apache License 2.0

These headers declare the official `pl::` mod API (`Mod`, `ModMenu`, `Input`,
`memory::Hook`, `memory::Signature`, …) used by this mod.

## 3. `third_party/fmt/` — {fmt} formatting library

- **Source**: https://github.com/fmtlib/fmt
- **License**: MIT License

## 4. `third_party/nlohmann/json.hpp` — JSON for Modern C++

- **Source**: https://github.com/nlohmann/json
- **License**: MIT License

Used for reading/writing the mod's `config/config.json`.

## 5. `resources/minecraft.ttf` — Minecraft font

- **Origin**: extracted from Minecraft Bedrock (© Mojang Studios / Microsoft)
- **Usage**: rendered on the in-game overlay for the zoom-level indicator
  (via `pl::modmenu::registerFont`).

This asset is the property of Mojang Studios/Microsoft and is **not** covered by
this project's MIT license. If you redistribute the mod, you do so under the terms
applicable to that asset. If you prefer to avoid shipping it, remove
`resources/minecraft.ttf` and the indicator falls back to the launcher's default font.

---

## Notes on inspiration vs. copied code

The byte-signature hook approach is *inspired by* the style used by
[BedrockTools](https://github.com/QYCottage/BedrockTools) (GPL-3.0), but this mod is an
independent implementation built on the official Levi `pl::` SDK: the signatures were
derived from the target `libminecraftpe.so` builds, and no BedrockTools source code is
copied into this project. If you later copy code from a GPL-3.0 project into this
repository, this project must be relicensed under GPL-3.0 as a whole.
