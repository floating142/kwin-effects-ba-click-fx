# BA Click FX

[中文](README.md) · [Documentation index](docs/README.md)

A KDE Plasma / KWin port of Blue Archive’s `FX_Touch`. Click to show a central disc, dissolving rings, and triangle particles. Hold a trigger button and move to draw a ribbon trail with particles along its path.

[Features](#features) · [Runtime requirements](#runtime-requirements) · [Install and enable](#install-and-enable) · [Configure](#configure)

![BA Click FX click and trail preview](preview/logo.gif)

## Features

- **Original particles and ribbon.** Uses the original particle textures and `Cylinder002` mesh with its UVs. The port follows the authored colors, brightness, sizes, rotation, lifetime, and UV animation of Ring, MeshTri, Ring3, Ring4, and TrailRenderer. [FX_Touch parity notes](docs/unity-parity.md)
- **Native GPU rendering.** C++ and OpenGL blend particles in a linear RGBA16F scene, reproduce Unity PPv2’s `MXFinalBloom`, and composite for SDR or HDR using KWin’s output color description.
- **Partial repaints and per-output buffers.** The renderer limits background import and compositing to affected regions and reuses buffers per display. On standard SDR outputs, Bloom processing can also be restricted to the affected region. [Rendering architecture](docs/architecture.md)
- **Configurable mouse triggers.** Select any combination of left, right, and middle buttons. Toggle click bursts and trails independently, or let the trail follow the pointer without holding a button.
- **Multi-monitor and HiDPI support.** Set a scale for each display. The effect samples the trail at presentation time on the display under the pointer. After crossing to another display, new trail segments and triangle particles use that display’s scale.
- **Live preview and diagnostics.** Preview changes in the settings page and inspect repaint outlines, per-stage CPU/GPU timings, or structured diagnostic reports.

## Runtime requirements

| Component | Requirement |
| --- | --- |
| Desktop | KDE Plasma 6 |
| KWin | 6.8 is the primary target, with compatibility for 6.6 and 6.7 |
| Graphics | KWin OpenGL compositing with shader and RGBA16F framebuffer support |

The native plugin is tied to the KWin version it was built against. Use development packages that match your current session and rebuild the plugin after upgrading KWin.

## Install and enable

### Install build dependencies

Building from source requires a C++20 compiler, CMake 3.20+, ECM 6.22+, Qt 6.10+, KF6 6.22+, Gettext, OpenGL development files, and matching KWin development packages. Expand the command for your distribution.

<details>
<summary>Arch Linux</summary>

```bash
sudo pacman -S --needed base-devel cmake extra-cmake-modules kservice kwin libepoxy qt6-base \
  qt6-declarative vulkan-headers
```

</details>

<details>
<summary>Fedora</summary>

```bash
sudo dnf install -y cmake extra-cmake-modules gcc-c++ gettext kf6-kcmutils-devel kf6-ki18n-devel \
  kf6-kservice-devel kwin-devel libdrm-devel libepoxy-devel qt6-qtbase-devel qt6-qtdeclarative-devel \
  qt6-qttools-devel vulkan-headers
```

</details>

<details>
<summary>Kubuntu</summary>

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends appstream ca-certificates cmake extra-cmake-modules \
  g++ gettext git kwin-dev libdrm-dev libepoxy-dev libkf6config-dev libkf6coreaddons-dev libkf6i18n-dev \
  libkf6kcmutils-dev libkf6service-dev libkf6windowsystem-dev ninja-build pkg-config qt6-base-dev \
  qt6-declarative-dev qt6-tools-dev libvulkan-dev
```

</details>

### Build and enable the effect

1. Run the installer from the source directory as your regular user. It requests administrator access during the system installation step.

   ```bash
   ./install-local.sh --system
   ```

2. Open **System Settings → Appearance & Style → Desktop Effects**, then find and enable **BA Click FX**. If it is missing after a first install or KWin upgrade, log out of Plasma and back in.

The installer also accepts `--user`, `--no-reload`, and `--help`. Set `JOBS=4` to choose the number of parallel build jobs.

## Configure

Click the settings button next to **BA Click FX** in **Desktop Effects**. Select **Apply** to save your changes.

| Tab | Controls |
| --- | --- |
| **Effects** | Click animations, trigger buttons, trails, always-on trails, and particles along the trail |
| **Appearance** | Animation speed, overall size, and per-display scaling |
| **Where to show** | Desktop-only mode, fullscreen suppression, and app exclusions |
| **Diagnostics** | Log level, repaint-region outlines, and reports |

Click animations, trails, and distance particles are enabled by default, with the left button as the only trigger. Fullscreen suppression is off by default. Each newly pressed selected button plays one click burst. Selected buttons share a trail until the last one is released. **Always show trail** works with all trigger buttons cleared. Context menus and middle-click paste continue to work.

With per-display scaling enabled, each display uses its own scale. Displays without a saved value default to `1.00`.

With fullscreen suppression enabled, dragging into a fullscreen window stops emission and lets existing particles and trails fade. Leaving the window starts a new stroke at the current pointer position. To exclude an app, enable app exclusions under **Where to show** and use **Pick window…** to add it.

## Troubleshoot

Use the **Diagnostics** tab to copy status information or generate a report. To investigate rendering issues, temporarily raise the log level or enable repaint-region outlines. Turn outlines off when measuring performance.

```bash
journalctl --user --since=-2min -o cat QT_CATEGORY=kwin_effect_ba_click_fx
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx status
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx diagnostics
```

After rebuilding a plugin that KWin has already loaded, start a new KWin session to load the new code. For development, use the repository’s nested session script. [Testing and releases](docs/testing.md)

## Uninstall

```bash
./uninstall-local.sh --system
```

Use `--user` for a user installation. Settings are kept by default. Add `--purge-config` to remove them and clear the enabled state.

## Development docs

[Source layout](docs/architecture.md) · [Testing and releases](docs/testing.md) · [FX_Touch parity notes](docs/unity-parity.md) · [Roadmap](docs/roadmap.md)

## License

The project code is licensed under GNU GPL v3.0 or later. Blue Archive names, trademarks, and game-derived assets belong to their respective rights holders and are subject to their own terms. This is an unofficial implementation. [License](LICENSE)
