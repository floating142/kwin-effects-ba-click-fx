# 稳定性与兼容性验证

本项目不依赖桌面截图。`ctest` 执行纯逻辑回归，并在 KWin 6.8+、EGL 和 Mesa
surfaceless 上下文可用时执行离屏 GPU 回归；显示系统相关行为使用嵌套 KWin 会话和
结构化诊断记录验证。

## 自动检查

```bash
./scripts/verify-release.sh
```

可通过 `JOBS=4` 调整并行任务数，或将自定义构建目录作为第一个参数传入。

`scripts/verify-release.sh` 会在临时 `DESTDIR` 中实际安装，并检查插件、KCM、OBJ、四张贴图
和全部 Shader。临时目录退出时自动删除，不修改用户或系统安装。

离屏 GPU 回归使用当前源码中的资源和软件 GLES 上下文，不操作宿主桌面。检查包括：

- RGBA16F 附件实际保存大于 1 的高亮和微小暗部值；
- sRGB 采样硬件解码、原有 Alpha 上传语义和上传状态恢复；
- 暗背景经导入与最终合成后保持不变，覆盖 direct/blit 和 1.0/1.6 缩放；
- 真实 Bloom 金字塔将 HDR 亮源扩散到几何范围之外。

无法创建离屏 EGL 上下文时 GPU 用例会明确标记为跳过，不代表通过渲染验证。

KWin 6.8 改为 GLES 后，`GLTexture::allocate()` 的实际存储可能是普通 8 位格式，
即使 `internalFormat()` 仍报告 RGBA16F 或 sRGB。因此本插件显式分配 sized 存储，
GPU 回归检查实际附件类型和渲染数值，防止暗背景黑块及 Bloom 能量截断再次出现。

## KWin API 兼容层

KWin 6.8 是默认开发和验证目标。`src/kwincompat.h/.cpp` 统一处理生命周期入口，
特效主体只实现状态更新、叠层绘制和指针位置采样，不包含版本条件分支。

| KWin | 呈现时间戳 | 底层绘制结果 | 指针移动 |
| --- | --- | --- | --- |
| 6.8+（默认） | `RenderView` | `bool`，失败时不绘制叠层 | `pointerMotion` |
| 6.7 | `RenderView` | `void` | `pointerMotion` |
| 6.6 | `prePaintScreen` 参数 | `void` | `mouseChanged` / 帧级回退 |

CMake 仅为旧版本启用 `BA_CLICK_FX_KWIN_LEGACY_*` 宏，最低版本显式要求 6.6。
升级兼容逻辑时，应核对调用链只转发一次、时间戳单位为纳秒，以及 6.8 绘制失败的传播。
旧版本头文件编译检查不能代替匹配版本 KWin 的实际加载与运行验证。

## 嵌套会话检查

```bash
./test-nested.sh --profile
./test-nested.sh --profile --scale=1.6 --size=2000x1250
./test-nested.sh --debug --no-plasma
```

每次验证以下行为：点击和拖尾正常结束；全屏窗口上方仍可绘制；禁用特效、切换输出或
结束会话后无残留；`skip_gpu`、`skip_target` 和 `skip_import` 保持为 0。

点击动画默认为开启，全屏抑制默认为关闭。另在双输出会话中开启全屏抑制，按住左键
从普通输出拖入另一输出上的全屏窗口：停止新增拖尾和 Ring4，已有内容自然淡出；
保持按住并拖回时从当前位置重新起笔，不跨全屏区域连线。覆盖普通拖动、AlwaysTrail、
鼠标静止时窗口切为全屏，以及关闭抑制后持续跨屏拖动的情况。

锁屏需要宿主会话授权，旋转输出和 HDR 需要对应虚拟输出或真实硬件，因此不声称由
普通单元测试自动覆盖。发布前在支持的机器上记录以下字段：KWin 版本、GPU 驱动、输出
逻辑尺寸、缩放、旋转、HDR 状态、HDR/bg/Bloom 尺寸和失败计数。

## 已验证组合

- KWin 6.7.4，Intel Arc（Mesa 26.1.6），OpenGL 4.6。
- 2000×1250、缩放 1.60 与 1920×1080、缩放 1.00 双输出。
- 跨屏拖动、逐输出 HDR Scene、背景纹理和 Bloom 金字塔分配正常。

## 持续拖尾性能基线

KWin 6.7.4、约 78 Hz 合成频率下开启帧统计并持续移动光标：

- `mouseChanged` 约 1.6～1.7 次/帧，帧级回退为 0。
- 输入处理约 0.07～0.08 ms/30 帧。
- CPU 平均约 0.30～0.33 ms，峰值约 0.67 ms。
- GPU 平均约 0.94～1.05 ms，未出现 GPU、目标或背景导入失败。

该数据作为后续容量复用优化的对比基线，不据此引入视觉降级或 Bloom 简化模式。

阶段 5 的首轮容量复用仅减少拖尾几何生成中的临时分配，未改变 1000 点上限、顶点
顺序、damage 或 Bloom。需在相同持续拖动场景下重新采集 `cpu_trail_avg_ms`，确认是否
有稳定收益后再进行跨输出几何共享。

双输出、拖尾达到 1000 点附近的复测结果：`cpu_trail_avg_ms` 从约 0.278～0.285 ms
下降到 0.182～0.184 ms，约下降 35%；`cpu_avg_ms` 从约 0.456～0.493 ms 下降到
0.385～0.405 ms，约下降 17～20%。GPU 分段和重绘面积随拖动路径波动，没有观察到
由该 CPU 容量复用引起的渲染路径变化。

顶点临时容器复用后的另一组实测：`cpu_particle_avg_ms=0.064`、`cpu_avg_ms=0.244`，
`cpu_max_ms=0.432`。该次场景包含 `3200x2000@1.60` 输出，GPU 总平均为 `2.431 ms`，
不能直接与较低分辨率的 GPU 基线比较；`skip_gpu`、`skip_target` 和 `skip_import` 均为 0。

帧统计在多输出下会按输出分别打印 `output="宽x高@缩放"`，并附带该输出的 GPU
导入、Bloom、最终合成时间，以及实际重绘、申请区域和 Bloom 源区域的平均像素数与
矩形数量。不同输出应分别采集，不能把两块屏幕的 GPU 时间直接相加比较。

诊断摘要中的 `import_mode=direct` 表示背景解码直接采样 KWin RenderTarget 纹理，
`import_mode=blit` 表示目标不可采样，已回退到背景纹理复制。直接路径需实机检查普通
输出、分数缩放、双输出接缝和旋转输出，并用 `output_gpu_import_ms` 与 blit 基线比较。

直接模式下诊断摘要中的 `bg=0x0` 是预期结果：fallback 背景纹理采用延迟分配，只有
实际进入 `import_mode=blit` 时才创建。

## 已知驱动差异

- HDR 颜色由 KWin 的输出色彩描述和传递函数决定，不应比较不同驱动的逐像素结果。
- GLSL 编译详细错误由 KWin `kwin_opengl` 分类提供，本插件同时输出固定错误码。
- GPU timer 是异步采样，样本数通常少于帧数；这不代表丢帧。
- 多输出下 `skip_no_damage` 较高通常表示内容只与另一块输出相交。

## 升级与回滚

KWin ABI 变化后必须重新编译。升级失败时先运行 `uninstall-local.sh` 清除当前用户安装，
再从目标版本源码执行 `install-local.sh`。不要混用旧插件二进制和新 Shader/贴图；诊断中
的 build id、KWin 版本和资源路径应来自同一次安装。
