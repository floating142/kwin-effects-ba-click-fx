# BA Click FX

[English](README.en.md) · [文档索引](docs/README.md)

为 KDE Plasma / KWin 移植《蔚蓝档案》的 `FX_Touch`。点击时出现中心圆盘、溶解圆环和小三角，按住拖动时生成光带和沿途粒子。

[特性](#特性) · [运行要求](#运行要求) · [安装与启用](#安装与启用) · [配置](#配置)

![BA Click FX 点击与拖尾预览](preview/logo.gif)

## 特性

- **还原原版粒子与光带。** 采用原版粒子贴图、`Cylinder002` 网格和 UV，移植 Ring、MeshTri、Ring3、Ring4 和 TrailRenderer 的颜色、发光强度、大小、旋转、寿命与 UV 动画。 [FX_Touch 核对记录](docs/unity-parity.md)
- **原生 GPU 渲染。** 通过 C++ 和 OpenGL 在 RGBA16F 线性场景中混合粒子，复刻 Unity PPv2 的 `MXFinalBloom`，并按 KWin 输出的色彩描述合成到 SDR 或 HDR 屏幕。
- **局部重绘与逐屏缓存。** 缩小背景导入和合成范围，复用各输出的渲染缓冲。普通 SDR 输出还可将辉光计算限制在受影响的区域。 [渲染架构](docs/architecture.md)
- **可组合的鼠标触发。** 左键、右键和中键可独立选择，点击动画与拖尾分别开关，也可在无需按住按键时始终显示拖尾。
- **多屏和 HiDPI。** 支持逐屏调整尺寸。拖尾按光标所在屏幕的呈现帧采样，跨屏时新生成的拖尾和小三角使用目标屏幕的倍率。
- **实时预览和诊断。** 在设置页预览改动，按需查看重绘范围、CPU/GPU 分阶段耗时和结构化诊断报告。

## 运行要求

| 项目 | 要求 |
| --- | --- |
| 桌面 | KDE Plasma 6 |
| KWin | 主要支持 6.8，兼容 6.6 和 6.7 |
| 图形 | KWin 的 OpenGL 合成，驱动需支持着色器和 RGBA16F 帧缓冲 |

原生插件绑定到构建时的 KWin 版本。请使用与当前会话匹配的 KWin 开发包，升级 KWin 后需重新编译插件。

## 安装与启用

### 安装构建依赖

源码构建需要 C++20 编译器、CMake 3.20+、ECM 6.22+、Qt 6.10+、KF6 6.22+、Gettext、OpenGL 开发文件和匹配版本的 KWin 开发包。可展开对应发行版的命令。

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

### 编译并启用

1. 在源码目录以普通用户运行安装脚本。安装到系统目录时，脚本会在安装阶段请求管理员权限。

   ```bash
   ./install-local.sh --system
   ```

2. 打开 **系统设置 → 外观与样式 → 桌面特效**，搜索并启用 **BA Click FX**。首次安装或升级 KWin 后若未出现，注销并重新登录 Plasma。

安装脚本也支持 `--user`、`--no-reload` 和 `--help`。可用 `JOBS=4` 指定编译并行数。

## 配置

在 **桌面特效** 中点击 **BA Click FX** 右侧的配置按钮。调整完成后点击 **应用** 保存。

| 设置页 | 可以调整的内容 |
| --- | --- |
| **效果** | 点击动画、触发按键、拖尾、始终显示拖尾和沿途小三角 |
| **外观** | 动画速度、整体尺寸和逐屏缩放 |
| **触发范围** | 仅在桌面显示、全屏抑制和按应用排除 |
| **诊断** | 日志级别、重绘区域边框和诊断报告 |

默认开启点击、拖尾和沿途小三角，仅左键触发。全屏抑制默认关闭。每个新按下的已选按键触发一次点击，多个已选按键共用一条拖尾，最后一个松开时才结束。清空按键选择后，“始终显示拖尾”仍可独立使用。右键菜单和中键粘贴照常工作。

开启显示器独立缩放后，每个屏幕使用自己的倍率。尚未配置的屏幕默认为 `1.00`。

开启全屏抑制后，按住拖入全屏窗口会停止发射，已有内容自然淡出。拖出时从当前位置重新起笔。若要排除指定应用，在“触发范围”中启用应用排除，再用“选取窗口…”将应用加入列表。

## 排查问题

从设置页的 **诊断** 复制诊断信息或生成报告。排查渲染问题时，可临时提高日志级别或开启重绘区域边框。性能测试时请关闭边框。

```bash
journalctl --user --since=-2min -o cat QT_CATEGORY=kwin_effect_ba_click_fx
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx status
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx diagnostics
```

重新编译并覆盖已加载的插件后，需要启动新的 KWin 会话才能加载新代码。开发时可使用仓库内的嵌套会话脚本。 [测试与发布](docs/testing.md)

## 卸载

```bash
./uninstall-local.sh --system
```

用户级安装使用 `--user`。默认保留设置，追加 `--purge-config` 可删除设置和启用状态。

## 开发文档

[源码结构](docs/architecture.md) · [测试与发布](docs/testing.md) · [FX_Touch 核对记录](docs/unity-parity.md) · [后续计划](docs/roadmap.md)

## 许可

项目代码采用 GNU GPL v3.0 或更高版本。《蔚蓝档案》的名称、商标和游戏来源素材归各自权利人所有，适用各自的授权条款。本项目为非官方实现。 [许可证](LICENSE)
