# kwin-effects-ba-click-fx

[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)

中文文档：[README.md](README.md)

**A KDE Plasma KWin click effect and cursor trail ported parameter by parameter from the Blue Archive Unity FX_Touch effect.**

![BA Click FX preview](preview/logo.gif)

## Features

- Recreates the Ring, MeshTri, Ring3, Ring4 and TrailRenderer elements from the Unity effect.
- Native C++ / OpenGL rendering with HDR particles and Unity-style PPv2 MXFinalBloom.
- Sparse damage regions for importing, processing and compositing only changed pixels.
- Multi-monitor and HiDPI support with per-output rendering resources.
- KCM configuration page with time scale, overall scale, trail and diagnostics controls.

## Requirements

- KDE Plasma 6 / KWin 6.6 or newer
- OpenGL compositing
- CMake 3.20+, ECM 6.22+, Qt 6.10+ and KF6 6.22+
- KWin development files matching the running KWin version

KWin native effects are ABI-bound to the exact KWin version. This project supports KWin 6.6 and newer, but rebuild it with the matching development packages after upgrading KWin.

## Install

Install the build dependencies first.

Arch Linux:

```bash
sudo pacman -S --needed base-devel cmake extra-cmake-modules kwin libepoxy qt6-base qt6-declarative vulkan-headers
```

Fedora:

```bash
sudo dnf install -y cmake extra-cmake-modules gcc-c++ gettext kf6-kcmutils-devel kf6-ki18n-devel kwin-devel libdrm-devel libepoxy-devel qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qttools-devel vulkan-headers
```

Running `scripts/verify-release.sh` additionally requires `appstream` and `ripgrep`; CI also uses
`ninja-build`.

Run the installer as your normal user:

```bash
./install-local.sh --system
```

The script requests administrator access only for installing into `/usr`. Then open **System Settings -> Appearance & Style -> Desktop Effects**, search for **BA Click FX**, and enable it. Log out and back in after a first install or KWin upgrade if the effect is not listed.

Useful options:

```bash
JOBS=4 ./install-local.sh --system
./install-local.sh --no-reload
./install-local.sh --help
```

Remove a system installation with:

```bash
./uninstall-local.sh --system
```

Use `--user` for a user installation. Add `--purge-config` to remove the saved settings and enabled state as well.

## Testing

Run the build, unit tests and temporary installation checks with:

```bash
./scripts/verify-release.sh
```

Run manual rendering checks in an isolated nested KWin session with:

```bash
./test-nested.sh --profile
```

See [TESTING.md](TESTING.md) for HiDPI, multi-monitor, HDR, diagnostics and rollback notes.

## Diagnostics

The configuration page can copy diagnostics, generate a report and open the diagnostics folder. Command-line status is also available:

```bash
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx status
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx diagnostics
```

Set `LogLevel` to `3` for frame statistics or `4` for verbose diagnostics. Reports are stored in
`~/.cache/ba-click-fx/diagnostics/`. The `DebugDamage` option draws the regions requested by the
effect and should be disabled when collecting performance numbers.

## Rendering architecture

Each frame imports the changed desktop pixels into a linear RGBA16F scene, draws the trail and
particle layers in Unity render-queue order, runs the PPv2 MXFinalBloom pyramid over the bright
source region, and composites the scene back through the output color transform. Per-output
targets are reused; damage regions reduce imported and processed pixels without changing shader
parameters or particle geometry.

## License

The project code is licensed under [GNU GPL v3.0 or later](LICENSE). Blue Archive names, trademarks and game-derived visual assets belong to their respective rights holders. This is an unofficial technical research and desktop visual effect project.
