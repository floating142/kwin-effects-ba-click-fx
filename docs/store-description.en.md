# BA Click FX

BA Click FX is a desktop click effect and cursor trail effect for KDE Plasma 6 / KWin.

It is a parameter-level port of the Blue Archive Unity `FX_Touch` effect. Mouse clicks show a central disc, dissolving rings and particle bursts. Dragging the pointer produces a ribbon trail and triangle particles emitted along the traveled path.

## Features

- Unity-style click rings, particles and cursor trail
- Adjustable time scale and overall size
- Optional trail, always-on pointer trail and distance particles
- Multi-monitor, HiDPI and per-output scale support
- HDR particle blending and Unity-style Bloom post-processing
- Native KWin Desktop Effects configuration page
- Log levels, damage-region visualization and structured diagnostics reports

## Compatibility

- KDE Plasma 6
- KWin 6.6 or newer
- OpenGL compositing
- Wayland sessions

KWin native effects are ABI-bound to the exact KWin version. Rebuild the effect with the matching KWin development packages after upgrading KWin.

## Installation

Download the source archive from GitHub Releases, for example:

```text
kwin-effects-ba-click-fx-1.0.0.tar.xz
```

This project is intended to be installed from the source archive. It does not provide universal prebuilt binaries. Extract the archive and run from the source directory:

```bash
./install-local.sh --system
```

The script configures, builds and installs the effect, configuration module, textures and shaders. When installing to `/usr`, it requests administrator access only during the install step; do not run the whole script with `sudo`.

Then open:

**System Settings → Appearance & Style → Desktop Effects**

Search for **BA Click FX** and enable it. After a first installation or a KWin upgrade, log out and back in if the effect is not listed.

For a user installation, use:

```bash
./install-local.sh --user
```

User installation is mainly intended for nested KWin testing. A normal KWin session may not search this plugin path automatically.

## Uninstallation

System installation:

```bash
./uninstall-local.sh --system
```

User installation:

```bash
./uninstall-local.sh --user
```

## Source code and support

GitHub:

https://github.com/floating142/kwin-effects-ba-click-fx

The GitHub repository contains the source code, source releases, installation scripts, testing notes and issue tracker.

## License

The project code is licensed under GNU GPL-3.0-or-later. Blue Archive names, trademarks and game-derived visual assets belong to their respective rights holders. This is an unofficial technical research and desktop visual effect project.
