#!/usr/bin/env python3
"""
Packages the built mod into BetterZoom.levipack.

The levipack is a zip whose top-level directory name is the runtime mod id
(per the LeviLauncher developer guide: "目录名是运行期 mod id"):

    flarialzoom/
        manifest.json
        libflarialzoom.so
        icon.png
        config/config.json
        config/config.schema.json
        resources/minecraft.ttf
"""
import argparse
import json
import zipfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".", help="project root")
    parser.add_argument("--out", default="BetterZoom.levipack")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    mod_id = "betterzoom"
    lib = root / "build" / "libbetterzoom.so"
    manifest = root / "manifest.json"
    icon = root / "icon.png"
    config_json = root / "config" / "config.json"
    config_schema = root / "config" / "config.schema.json"
    font = root / "resources" / "minecraft.ttf"

    # Custom button icon: ship every image in the config folder so users can
    # replace the zoom button icon by swapping config/button_icon.png.
    extra_images = []
    config_dir = root / "config"
    if config_dir.is_dir():
        for f in sorted(config_dir.iterdir()):
            if f.suffix.lower() in (".png", ".jpg", ".jpeg", ".webp", ".svg"):
                extra_images.append(f)

    for p in (lib, manifest, icon, config_json, config_schema, font):
        if not p.is_file():
            raise SystemExit(f"missing file: {p}")

    manifest_data = json.loads(manifest.read_text(encoding="utf-8"))
    if manifest_data.get("entry") != lib.name:
        raise SystemExit("manifest entry does not match the built library")

    out = Path(args.out)
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9) as z:
        entries = {
            f"{mod_id}/manifest.json": manifest,
            f"{mod_id}/{lib.name}": lib,
            f"{mod_id}/icon.png": icon,
            f"{mod_id}/config/config.json": config_json,
            f"{mod_id}/config/config.schema.json": config_schema,
            **{f"{mod_id}/config/{f.name}": f for f in extra_images},
            f"{mod_id}/resources/minecraft.ttf": font,
        }
        for name, path in entries.items():
            z.write(path, name)

    with zipfile.ZipFile(out) as z:
        names = set(z.namelist())
        expected = set(entries)
        if names != expected:
            raise SystemExit(f"unexpected entries: {sorted(names ^ expected)}")

    print(f"packaged {out} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
