# kwin-effects-ba-click-fx

[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)

**从 Blue Archive Unity UI/FX_Touch 逐参数移植的 KDE Plasma KWin 点击特效与光标拖尾插件。**

`kwin-effects-ba-click-fx` 将 `FX_Touch` 中 ParticleSystem、TrailRenderer 与后处理参数逐项还原到 KWin：点击时播放中心圆盘、溶解圆环和 Ring3 粒子，按住左键拖动时生成 Ribbon 拖尾与 Ring4 距离发射粒子。渲染由原生 C++ / OpenGL 完成，使用线性 RGBA16F Scene 和 Unity PPv2 MXFinalBloom，不依赖脚本运行时。

## 特性

- 从 Unity 参数逐项移植，不是相似风格的重新设计
- 还原 Ring、MeshTri、Ring3、Ring4 与 TrailRenderer 的颜色、大小、旋转、寿命、UV 动画和 HDR 强度
- 使用 Cylinder002 原始网格与逐角 UV，粒子贴图由 Shader 直接采样
- 线性 RGBA16F Scene、HDR 粒子混合与 Unity PPv2 MXFinalBloom
- 稀疏 damage Region，仅导入、计算和合成本帧可能变化的区域
- 支持多显示器、HiDPI 和不同输出缩放比例
- 使用 KWin 指针事件保留高回报率鼠标的真实拖动轨迹
- 提供 KCM 配置页、重绘区域标记和 CPU/GPU 分段性能日志

## 运行要求

- KDE Plasma 6 / KWin 6.7 或更高版本
- OpenGL 合成器；项目没有 CPU 渲染回退
- CMake 3.20+
- ECM 6.26+
- Qt 6.10+
- KF6 6.26+
- 与当前运行版本匹配的 KWin 开发包

KWin 原生特效插件与 `EffectPluginFactory` ABI 绑定。升级 KWin 后必须用新版本开发包重新编译，否则 KWin 会忽略旧二进制。

## 安装

系统级安装：

```bash
./install-local.sh --system
```

不要使用 `sudo ./install-local.sh`。脚本只在安装步骤内部调用 `sudo`，其余 D-Bus 操作必须连接当前用户的 KWin 会话。

脚本会完成配置、编译、安装和 effect 实例重建。首次安装新 native plugin 时，当前 KWin 进程可能尚未发现它；按脚本提示重新登录 Plasma，或重启 KWin Wayland 会话后再启用。

安装后打开「系统设置 → 外观与样式 → 桌面特效」，搜索 **BA Click FX** 或 **蔚蓝档案点击特效** 并启用。右侧配置按钮提供：

- 时间缩放
- 整体尺寸
- 拖尾开关
- 沿途小三角开关
- 调试日志
- 重绘区域边框

颜色、粒子数量、拖尾宽度、发射间距和 Bloom 参数保持 Unity 原始值，不作为用户调节项。

卸载系统级安装：

```bash
./uninstall-local.sh --system
```

卸载用户级安装时使用 `--user`。默认保留效果配置；如需同时删除配置和启用状态，追加 `--purge-config`。

## 嵌套测试

开发时建议使用独立的嵌套 KWin 会话：

```bash
./test-nested.sh
```

性能日志模式：

```bash
./test-nested.sh --profile
```

重绘区域可视化模式：

```bash
./test-nested.sh --debug
```

可额外指定逻辑尺寸与输出缩放：

```bash
./test-nested.sh --size=1600x900 --scale=2
```

嵌套会话使用独立 Wayland socket 和 D-Bus，会话关闭后进程与插件二进制都会重新加载，适合验证 C++ 修改。

## 日志与诊断

开启运行时日志：

```bash
kwriteconfig6 --file kwinrc --group Effect-ba-click-fx --key DebugLog true
qdbus-qt6 org.kde.KWin /Effects reconfigureEffect kwin4_effect_ba_click_fx
```

查看已有日志或持续跟踪：

```bash
journalctl --user --since=-2min -o cat QT_CATEGORY=kwin_effect_ba_click_fx
journalctl --user -f -n 0 -o cat QT_CATEGORY=kwin_effect_ba_click_fx
```

Qt 日志分类保存在 journal 的 `QT_CATEGORY` 字段中，消息正文通常只有“起实例”“帧统计”等内容。使用上面的字段匹配比在 `-o cat` 输出后追加文本过滤更可靠。

查询运行实例或主动生成测试日志：

```bash
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx status
qdbus-qt6 org.kde.KWin /Effects debug kwin4_effect_ba_click_fx log
```

显示重绘区域：

```bash
kwriteconfig6 --file kwinrc --group Effect-ba-click-fx --key DebugDamage true
qdbus-qt6 org.kde.KWin /Effects reconfigureEffect kwin4_effect_ba_click_fx
```

青色表示插件申请的逻辑重绘区域，洋红色表示 KWin 实际交给 effect 的输出渲染区域。性能测试时应关闭 `DebugDamage`。

## 热重载限制

`reconfigureEffect` 只重新读取配置。`unloadEffect` / `loadEffect` 可以重建 Effect 对象，但 Qt 会缓存已映射的 native plugin factory，因此覆盖一个正在使用的 `.so` 不会替换当前进程中的机器码。

- 配置修改可以立即生效
- 首次发现的新插件通常可以直接加载
- 已加载插件的 C++ 修改需要新的 KWin 进程
- 日常开发使用 `test-nested.sh`，正式安装后重新登录会话

## 渲染架构

每帧渲染顺序与 Unity UI Render Pass 对齐：

1. 将本帧变化区域的桌面像素导入线性 RGBA16F Scene。
2. 按 Unity Render Queue 顺序绘制 Trail、Ring/Ring3、Ring4 与 MeshTri。
3. 对真实亮源区域执行 PPv2 MXFinalBloom 金字塔。
4. 将 Scene 与 Bloom 合成并编码回 KWin 输出传递函数。

FBO 与 Bloom 金字塔按输出复用；damage Region 只减少无变化 texel 的导入、传播和合成，不改变 Shader、采样核、几何、粒子参数或最终视觉结果。

## 许可

项目代码采用 [GNU GPL v3.0 或更高版本](LICENSE)。Blue Archive 相关名称、商标及游戏来源的视觉资源归各自权利人所有，不包含在项目代码的 GPL 授权范围内。本项目是非官方的技术研究与桌面视觉效果实现，与 NEXON、NEXON Games、Yostar 等权利方无隶属或授权关系。

后续计划记录在 [TODO.md](TODO.md)。

参数分析与跨平台实现可参考 [ba-click-fx](https://github.com/CialloKing/ba-click-fx)。
