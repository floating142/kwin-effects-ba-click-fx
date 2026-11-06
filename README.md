# BA Click FX

[English](README.en.md) · [文档索引](docs/README.md)

为 KDE Plasma / KWin 移植《蔚蓝档案》的 `FX_Touch`。点击时出现中心圆盘、溶解圆环和小三角，按住拖动时生成光带和沿途粒子。

![BA Click FX 点击与拖尾预览](preview/logo.gif)

- **点击和拖尾可分别设置。** 左键、右键和中键可多选，也可单独启用始终显示拖尾。
- **支持多屏和 HiDPI。** 可逐屏调整尺寸，拖尾按光标所在屏幕的呈现帧采样。
- **按原版参数渲染。** 使用原版贴图和网格，通过 C++ / OpenGL 完成线性 HDR 混合和 Bloom。 [FX_Touch 核对记录](docs/unity-parity.md)

## 安装与启用

| 项目 | 要求 |
| --- | --- |
| 桌面 | KDE Plasma 6 和 OpenGL 合成 |
| KWin | 以 6.8 为主要目标，保留 6.6 / 6.7 兼容 |
| 构建 | C++20 编译器、CMake 3.20+、ECM 6.22+、Qt 6.10+、KF6 6.22+ |

KWin 开发包须与当前运行的 KWin 版本匹配。升级 KWin 后需要重新编译插件。

### 安装构建依赖

展开对应发行版的命令。构建依赖需满足上表版本。

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

### 编译并启用

1. 在源码目录以普通用户运行安装脚本。安装到系统目录时，脚本会在安装阶段请求管理员权限。

   ```bash
   ./install-local.sh --system
   ```

2. 打开 **系统设置 → 外观与样式 → 桌面特效**，搜索并启用 **BA Click FX**。首次安装或升级 KWin 后若未出现，注销并重新登录 Plasma。

安装脚本也支持 `--user`、`--no-reload` 和 `--help`。可用 `JOBS=4` 指定编译并行数。

## 配置

| 设置页 | 可以调整的内容 |
| --- | --- |
| **效果** | 点击动画、触发按键、拖尾、始终显示拖尾和沿途小三角 |
| **外观** | 动画速度、整体尺寸和逐屏缩放 |
| **触发范围** | 仅在桌面显示、全屏抑制和按应用排除 |
| **诊断** | 日志级别、重绘区域边框和诊断报告 |

默认开启点击、拖尾和沿途小三角，仅左键触发。全屏抑制默认关闭。每个新按下的已选按键触发一次点击，多个已选按键共用一条拖尾，最后一个松开时才结束。清空按键选择后，“始终显示拖尾”仍可独立使用。右键菜单和中键粘贴照常工作。

开启全屏抑制后，按住拖入全屏窗口会停止发射，已有内容自然淡出。拖出时从当前位置重新起笔。若要排除指定应用，在“触发范围”中启用应用排除，再用“选取窗口…”将应用加入列表。

配置支持实时预览，点击“应用”后保存。

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
