# 稳定性与兼容性验证

本项目不使用截图或像素回归测试。纯逻辑回归由 `ctest` 执行，显示系统相关行为使用
嵌套 KWin 会话和结构化诊断记录验证。

## 自动检查

```bash
./verify-release.sh
```

可通过 `JOBS=4` 调整并行任务数，或将自定义构建目录作为第一个参数传入。

`verify-release.sh` 会在临时 `DESTDIR` 中实际安装，并检查插件、KCM、OBJ、四张贴图
和全部 Shader。临时目录退出时自动删除，不修改用户或系统安装。

## 嵌套会话检查

```bash
./test-nested.sh --profile
./test-nested.sh --profile --scale=1.6 --size=2000x1250
./test-nested.sh --debug --no-plasma
```

每次验证以下行为：点击和拖尾正常结束；全屏窗口上方仍可绘制；禁用特效、切换输出或
结束会话后无残留；`skip_gpu`、`skip_target` 和 `skip_import` 保持为 0。

锁屏需要宿主会话授权，旋转输出和 HDR 需要对应虚拟输出或真实硬件，因此不声称由
普通单元测试自动覆盖。发布前在支持的机器上记录以下字段：KWin 版本、GPU 驱动、输出
逻辑尺寸、缩放、旋转、HDR 状态、HDR/bg/Bloom 尺寸和失败计数。

## 已验证组合

- KWin 6.7.4，Intel Arc（Mesa 26.1.6），OpenGL 4.6。
- 2000×1250、缩放 1.60 与 1920×1080、缩放 1.00 双输出。
- 跨屏拖动、逐输出 HDR Scene、背景纹理和 Bloom 金字塔分配正常。

## 已知驱动差异

- HDR 颜色由 KWin 的输出色彩描述和传递函数决定，不应比较不同驱动的逐像素结果。
- GLSL 编译详细错误由 KWin `kwin_opengl` 分类提供，本插件同时输出固定错误码。
- GPU timer 是异步采样，样本数通常少于帧数；这不代表丢帧。
- 多输出下 `skip_no_damage` 较高通常表示内容只与另一块输出相交。

## 升级与回滚

KWin ABI 变化后必须重新编译。升级失败时先运行 `uninstall-local.sh` 清除当前用户安装，
再从目标版本源码执行 `install-local.sh`。不要混用旧插件二进制和新 Shader/贴图；诊断中
的 build id、KWin 版本和资源路径应来自同一次安装。
