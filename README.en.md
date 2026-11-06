# BA Click FX

[中文](README.md) · [Documentation index](docs/README.md)

A KDE Plasma / KWin port of Blue Archive’s `FX_Touch`. Click to show a central disc, dissolving rings, and triangle particles. Hold a trigger button and move to draw a ribbon trail with particles along its path.

![BA Click FX click and trail preview](preview/logo.gif)

- **Separate click and trail controls.** Select any combination of left, right, and middle buttons, or enable an always-on trail.
- **Multi-monitor and HiDPI support.** Set the size per display. Trail sampling follows the presentation frames of the display under the cursor.
- **Rendering based on the original effect.** BA Click FX uses the original textures and mesh with native C++ / OpenGL, linear HDR blending, and Bloom. [FX_Touch parity notes](docs/unity-parity.md)

## Install and enable

| Component | Requirement |
| --- | --- |
| Desktop | KDE Plasma 6 with OpenGL compositing |
| KWin | 6.8 is the primary target, with 6.6 and 6.7 compatibility |
| Build tools | A C++20 compiler, CMake 3.20+, ECM 6.22+, Qt 6.10+, and KF6 6.22+ |

Use development packages that match your running KWin version. Rebuild the plugin after upgrading KWin.

### Install build dependencies

Expand the command for your distribution. The installed packages must meet the versions listed above.

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
  g++ git kwin-dev libdrm-dev libepoxy-dev libkf6config-dev libkf6coreaddons-dev libkf6i18n-dev \
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

| Tab | Controls |
| --- | --- |
| **Effects** | Click animations, trigger buttons, trails, always-on trails, and particles along the trail |
| **Appearance** | Animation speed, overall size, and per-display scaling |
| **Where to show** | Desktop-only mode, fullscreen suppression, and app exclusions |
| **Diagnostics** | Log level, repaint-region outlines, and reports |

Click animations, trails, and distance particles are enabled by default, with the left button as the only trigger. Fullscreen suppression is off by default. Each newly pressed selected button plays one click burst. Selected buttons share a trail until the last one is released. **Always show trail** works with all trigger buttons cleared. Context menus and middle-click paste continue to work.

With fullscreen suppression enabled, dragging into a fullscreen window stops emission and lets existing particles and trails fade. Leaving the window starts a new stroke at the current pointer position. To exclude an app, enable app exclusions under **Where to show** and use **Pick window…** to add it.

Preview changes in the settings page and select **Apply** to save them.

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
