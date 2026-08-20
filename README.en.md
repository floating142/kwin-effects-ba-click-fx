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

- KDE Plasma 6 / KWin 6.7 or newer
- OpenGL compositing
- CMake 3.20+, ECM 6.26+, Qt 6.10+ and KF6 6.26+
- KWin development files matching the running KWin version

KWin native effects are ABI-bound to the exact KWin version. Rebuild this effect after upgrading KWin.

## Install

On Arch Linux, install the usual build dependencies first:

```bash
sudo pacman -S --needed base-devel cmake extra-cmake-modules kwin
```

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
./verify-release.sh
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

## License

The project code is licensed under [GNU GPL v3.0 or later](LICENSE). Blue Archive names, trademarks and game-derived visual assets belong to their respective rights holders. This is an unofficial technical research and desktop visual effect project.
