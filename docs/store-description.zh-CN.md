# BA Click FX

BA Click FX 是一个面向 KDE Plasma 6 / KWin 的桌面点击特效与光标拖尾特效。

它根据 Blue Archive Unity `FX_Touch` 效果进行参数级移植：点击时显示中心圆盘、溶解圆环和粒子迸发；拖动鼠标时显示 Ribbon 光带拖尾，并沿移动路径发射小三角粒子。

## 主要功能

- Unity 风格的点击圆环、粒子和光标拖尾
- 可调时间缩放和整体尺寸
- 可选启用拖尾、持续光标拖尾和沿途小三角
- 支持多显示器、HiDPI 和不同输出缩放比例
- HDR 粒子混合与 Unity 风格 Bloom 后处理
- 提供 KWin 桌面特效配置页面
- 提供日志级别、重绘区域显示和结构化诊断报告

## 兼容性

- KDE Plasma 6
- KWin 6.7 或更高版本
- OpenGL 合成器
- Wayland 会话

KWin 原生特效与 KWin 的精确版本 ABI 绑定。升级 KWin 后，需要使用当前 KWin 开发包重新编译插件。

## 安装

请从 GitHub Releases 下载源码压缩包，例如：

```text
kwin-effects-ba-click-fx-1.0.0.tar.xz
```

本项目应从源码包安装，不提供通用的预编译二进制包。解压后，在源码目录运行：

```bash
./install-local.sh --system
```

脚本会配置、编译并安装 effect、配置模块、贴图和 Shader。安装到 `/usr` 时，脚本会在安装步骤请求管理员权限；请不要使用 `sudo ./install-local.sh`。

安装后打开：

**系统设置 → 外观与样式 → 桌面特效**

搜索 **BA Click FX** 并启用。首次安装或升级 KWin 后，如果特效没有出现，请注销并重新登录 Plasma。

用户级安装可使用：

```bash
./install-local.sh --user
```

用户级安装主要用于嵌套 KWin 测试，普通 KWin 会话通常不会自动搜索该插件路径。

## 卸载

系统级安装：

```bash
./uninstall-local.sh --system
```

用户级安装：

```bash
./uninstall-local.sh --user
```

## 源码与问题反馈

GitHub：

https://github.com/floating142/kwin-effects-ba-click-fx

源码、Release 源码包、安装脚本、测试说明和问题反馈均位于 GitHub 仓库。

## 许可证

项目代码采用 GNU GPL-3.0-or-later。Blue Archive 相关名称、商标和游戏来源的视觉资源归各自权利人所有。本项目是非官方的技术研究与桌面视觉效果实现。
