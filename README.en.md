# kwin-effects-ba-click-fx

[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)

Chinese documentation: [README.md](README.md)

**A KDE Plasma KWin click effect and cursor trail plugin ported parameter by parameter from the Blue Archive Unity UI/FX_Touch effect.**

`kwin-effects-ba-click-fx` recreates the ParticleSystem, TrailRenderer and post-processing parameters from `FX_Touch` in native KWin rendering. Clicks play the center disc, dissolving rings and Ring3 particles; holding the left button while moving creates the Ribbon trail and Ring4 distance-emitter particles. Rendering uses native C++ / OpenGL, a linear RGBA16F scene and Unity PPv2 MXFinalBloom without a scripting runtime.

![BA Click FX preview](preview/logo.gif)

## Features

- Parameter-by-parameter Unity port rather than a look-alike redesign.
- Recreates Ring, MeshTri, Ring3, Ring4 and TrailRenderer colors, sizes, rotation, lifetime, UV animation and HDR intensity.
- Uses the original Cylinder002 mesh and per-angle UVs; particle textures are sampled directly by shaders.
- Linear RGBA16F scene, HDR particle blending and Unity PPv2 MXFinalBloom.
- Sparse damage regions so only changed pixels are imported, processed and composited.
- Multi-monitor, HiDPI and different output scale support.
- KWin pointer events preserve high-polling-rate mouse movement for accurate trails.
- KCM configuration page, repaint-region markers and segmented CPU/GPU performance logs.
- Live preview in the settings page; changes are saved only on Apply.

## Requirements

- KDE Plasma 6 / KWin 6.6 or newer.
- OpenGL compositing; there is no CPU rendering fallback.
- CMake 3.20+, ECM 6.22+, Qt 6.10+ and KF6 6.22+.
- KWin development files matching the running KWin version.

KWin native effect plugins are ABI-bound to `EffectPluginFactory`. Rebuild with matching development packages after a KWin upgrade.

## Install

Arch Linux:

```bash
sudo pacman -S --needed base-devel cmake extra-cmake-modules kwin libepoxy qt6-base qt6-declarative vulkan-headers
```

Fedora:

```bash
sudo dnf install -y cmake extra-cmake-modules gcc-c++ gettext kf6-kcmutils-devel kf6-ki18n-devel kwin-devel libdrm-devel libepoxy-devel qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qttools-devel vulkan-headers
```

Kubuntu:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends appstream ca-certificates cmake extra-cmake-modules g++ git kwin-dev libdrm-dev libepoxy-dev libkf6config-dev libkf6coreaddons-dev libkf6i18n-dev libkf6kcmutils-dev libkf6windowsystem-dev ninja-build pkg-config qt6-base-dev qt6-declarative-dev qt6-tools-dev libvulkan-dev
```

CI and `scripts/verify-release.sh` additionally use `appstream`, `ripgrep` and `ninja-build`.

Run from the project directory:

```bash
./install-local.sh --system
```

Run the script as your normal user; it requests administrator access only for `/usr`. Then open **System Settings -> Appearance & Style -> Desktop Effects**, search for **BA Click FX**, and enable it. Log out and back in after a first install or KWin upgrade if it is not listed.

Useful options:

```bash
JOBS=4 ./install-local.sh --system
./install-local.sh --no-reload
./install-local.sh --help
```

The configuration page provides time scaling, overall scaling, per-display scaling, trail controls, debug logging and repaint-region borders. Unity-authored colors, particle counts, trail width, emission spacing and Bloom parameters remain fixed.

Remove a system installation with `./uninstall-local.sh --system`; use `--user` for a user installation. Add `--purge-config` to remove saved settings and the enabled state.

### Per-display scaling

Enable **Enable per-display scaling** to tune each output independently. Settings are stored in `~/.config/kwinrc` under `[Effect-ba-click-fx]`; `OutputScaleOverrides` is a JSON object keyed by output UUID:

```ini
OutputScaleEnabled=true
OutputScaleOverrides={"<output-uuid>":1.25}
```

Find UUIDs with `qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx outputs-json`. Reset saves an explicit `1.00`; legacy name-only entries are intentionally not migrated.

## Testing and compatibility

Automated tests, nested KWin sessions, dual-monitor/HiDPI/HDR checks and rollback notes are documented in [TESTING.md](TESTING.md).

## Logging and diagnostics

Use the configuration page to set log level, copy diagnostics, generate a report or open the diagnostics directory. If the page is unavailable:

```bash
kwriteconfig6 --file kwinrc --group Effect-ba-click-fx --key LogLevel 3
qdbus-qt6 org.kde.KWin /Effects reconfigureEffect kwin4_effect_ba_click_fx
journalctl --user --since=-2min -o cat QT_CATEGORY=kwin_effect_ba_click_fx
```

Log levels are `0=off`, `1=errors`, `2=instances`, `3=frame statistics` and `4=verbose debugging`. Query status or structured diagnostics with:

```bash
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx status
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx diagnostics
```

Reports are stored in `~/.cache/ba-click-fx/diagnostics/`. `DebugDamage` draws requested repaint regions and should be disabled for performance measurements.

## Hot-reload limitations

`reconfigureEffect` reloads configuration only. `unloadEffect` / `loadEffect` can recreate the effect object, but Qt caches the mapped native plugin factory, so replacing an already loaded `.so` does not replace code in the current KWin process. Configuration changes apply immediately; C++ changes require a new KWin process. Use `test-nested.sh` for development and log out after a system installation.

## Rendering architecture

Each frame imports changed desktop pixels into a linear RGBA16F scene, draws Trail, Ring/Ring3, Ring4 and MeshTri in Unity render-queue order, runs the PPv2 MXFinalBloom pyramid over the actual bright source region, then composites the scene and applies the KWin output transfer function. FBOs and Bloom pyramids are reused per output; damage regions reduce unchanged texel work without changing shaders, sampling kernels, geometry or particle parameters.

## License

The project code is licensed under [GNU GPL v3.0 or later](LICENSE). Blue Archive names, trademarks and game-derived visual assets belong to their respective rights holders and are not covered by the project code license. This is an unofficial technical research and desktop visual effect implementation.

Further plans are tracked in [TODO.md](TODO.md). Parameter analysis and cross-platform implementation references are available at [ba-click-fx](https://github.com/CialloKing/ba-click-fx).
